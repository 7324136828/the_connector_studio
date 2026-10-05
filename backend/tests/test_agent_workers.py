"""Thread-worker bounds and warning-free cancellation of running/queued agents."""
import asyncio
import gc
import threading
import unittest

from concurrent.futures import ThreadPoolExecutor
from backend.app.services.agent_workers import AgentWorkers, AgentStopped


class AgentWorkerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.loop = asyncio.get_running_loop()
        self.original_handler = self.loop.get_exception_handler()
        self.loop_errors = []
        self.loop.set_exception_handler(lambda loop, context: self.loop_errors.append(context))

    async def asyncTearDown(self):
        try:
            gc.collect()
            await asyncio.sleep(0)
            await asyncio.sleep(0)
            self.assertEqual(self.loop_errors, [], 'Worker futures must not log unhandled cancellation exceptions')
        finally:
            self.loop.set_exception_handler(self.original_handler)

    async def started(self, event):
        self.assertTrue(await asyncio.to_thread(event.wait, 2), 'Worker did not start')

    async def test_limits_one_and_two_bound_actual_worker_execution(self):
        for limit in (1, 2):
            with self.subTest(limit=limit):
                workers = AgentWorkers(lambda: limit)
                lock = threading.Lock()
                events = [threading.Event(), threading.Event()]
                release = threading.Event()
                state = {'active': 0, 'maximum': 0}
                names = []

                def work(index):
                    def invoke(cancelled):
                        with lock:
                            state['active'] += 1
                            state['maximum'] = max(state['maximum'], state['active'])
                            names.append(threading.current_thread().name)
                        events[index].set()
                        try:
                            if not release.wait(2):
                                raise RuntimeError('Fixture worker was not released')
                            return index
                        finally:
                            with lock:
                                state['active'] -= 1
                    return invoke

                first = asyncio.create_task(workers.run('first', work(0)))
                second = None
                try:
                    await self.started(events[0])
                    second = asyncio.create_task(workers.run('second', work(1)))
                    await asyncio.sleep(0)
                    if limit == 2:
                        await self.started(events[1])
                    else:
                        self.assertFalse(events[1].is_set())
                    release.set()
                    self.assertEqual(await asyncio.gather(first, second), [0, 1])
                    self.assertEqual(state['maximum'], limit)
                    self.assertTrue(all(name.startswith('studio-agent') for name in names))
                    self.assertEqual(workers.running, 0)
                    self.assertEqual(workers.handles, {})
                finally:
                    release.set()
                    await asyncio.gather(*(task for task in (first, second) if task is not None), return_exceptions=True)
                    await workers.close()

    async def test_cancelling_a_running_agent_stops_its_bound_loop_without_future_warnings(self):
        workers = AgentWorkers(lambda: 1)
        started, stopped = threading.Event(), threading.Event()

        def work(cancelled):
            async def main():
                workers.bind('running', asyncio.get_running_loop(), asyncio.current_task())
                started.set()
                try:
                    await asyncio.Event().wait()
                finally:
                    stopped.set()
            asyncio.run(main())

        task = asyncio.create_task(workers.run('running', work))
        try:
            await self.started(started)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            await self.started(stopped)
            self.assertEqual(workers.running, 0)
            self.assertEqual(workers.handles, {})
        finally:
            if not task.done():
                task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await workers.close()

    async def test_cancelling_a_queued_agent_never_executes_it_or_disturbs_the_running_agent(self):
        workers = AgentWorkers(lambda: 1)
        running, queued_entered, release = threading.Event(), threading.Event(), threading.Event()

        def keep_running(cancelled):
            running.set()
            if not release.wait(2):
                raise RuntimeError('Fixture running worker was not released')
            return 'Completed original worker'

        def queued(cancelled):
            queued_entered.set()
            return 'Queued worker must not run'

        first = asyncio.create_task(workers.run('running', keep_running))
        second = None
        try:
            await self.started(running)
            second = asyncio.create_task(workers.run('queued', queued))
            await asyncio.sleep(0)
            self.assertIn('queued', workers.handles)
            second.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await second
            self.assertFalse(queued_entered.is_set())
            self.assertFalse(first.done())
            self.assertEqual(workers.running, 1)
            self.assertNotIn('queued', workers.handles)
            release.set()
            self.assertEqual(await first, 'Completed original worker')
            self.assertEqual(workers.running, 0)
            self.assertEqual(workers.handles, {})
        finally:
            release.set()
            if second is not None:
                await asyncio.gather(second, return_exceptions=True)
            await asyncio.gather(first, return_exceptions=True)
            await workers.close()

    async def test_repeated_cancellation_waits_for_delayed_worker_cleanup_without_warnings(self):
        workers = AgentWorkers(lambda: 1)
        started = threading.Event()
        cleanup_started = threading.Event()
        cleanup_finished = threading.Event()
        release_cleanup = threading.Event()

        def work(cancelled):
            async def main():
                workers.bind('repeated', asyncio.get_running_loop(), asyncio.current_task())
                started.set()
                try:
                    await asyncio.Event().wait()
                finally:
                    cleanup_started.set()
                    if not await asyncio.to_thread(release_cleanup.wait, 2):
                        raise RuntimeError('Fixture worker cleanup was not released')
                    cleanup_finished.set()
            asyncio.run(main())

        task = asyncio.create_task(workers.run('repeated', work))
        try:
            await self.started(started)
            task.cancel()
            await self.started(cleanup_started)
            for _ in range(3):
                workers.cancel('repeated')
                task.cancel()
                await asyncio.sleep(0)
                self.assertFalse(task.done(), 'Parent cancellation escaped before worker cleanup finished')
                self.assertFalse(cleanup_finished.is_set())
            self.assertEqual(workers.running, 1)
            self.assertIn('repeated', workers.handles)
            release_cleanup.set()
            with self.assertRaises(asyncio.CancelledError):
                await asyncio.wait_for(asyncio.shield(task), 2)
            self.assertTrue(cleanup_finished.is_set(), 'Worker cleanup must finish before cancellation is returned')
            self.assertEqual(workers.running, 0)
            self.assertEqual(workers.handles, {})
        finally:
            release_cleanup.set()
            if not task.done():
                task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await workers.close()


    async def test_independent_stop_cancels_an_executor_queued_agent_without_stopping_running_work(self):
        workers = AgentWorkers(lambda: 1)
        workers.executor.shutdown(wait=True)
        workers.executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix='studio-agent')
        started, queued_entered = threading.Event(), threading.Event()
        release = threading.Event()
        def running(cancelled):
            started.set()
            if not release.wait(2):
                raise RuntimeError('Running fixture was not released')
            return 'original completed'
        def queued(cancelled):
            queued_entered.set()
            return 'must not run'
        first = asyncio.create_task(workers.run('active', running))
        second = None
        try:
            await self.started(started)
            second = asyncio.create_task(workers.run('queued', queued))
            await asyncio.sleep(0)
            await asyncio.wait_for(workers.stop('queued'), .5)
            with self.assertRaises(AgentStopped):
                await second
            self.assertFalse(queued_entered.is_set())
            self.assertFalse(first.done())
            self.assertEqual(workers.running, 1)
            release.set()
            self.assertEqual(await first, 'original completed')
        finally:
            release.set()
            await asyncio.gather(*(task for task in (first, second) if task is not None), return_exceptions=True)
            await workers.close()


if __name__ == '__main__':
    unittest.main()
