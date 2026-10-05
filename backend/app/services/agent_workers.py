"""Bounded worker threads with cooperative cancellation of agent event loops."""
import asyncio
import threading
from concurrent.futures import ThreadPoolExecutor

_CANCELLED = object()


class AgentStopped(RuntimeError):
    """The user stopped a child independently of its parent task."""


class AgentWorkers:
    def __init__(self, limit):
        self.limit = limit
        self.condition = threading.Condition()
        self.running = 0
        self.handles = {}
        self.executor = ThreadPoolExecutor(max_workers=16, thread_name_prefix='studio-agent')

    def wake(self):
        with self.condition:
            self.condition.notify_all()

    def bind(self, key, loop, task):
        with self.condition:
            handle = self.handles[key]
            handle.update(loop=loop, task=task)
            if handle['cancelled'].is_set():
                loop.call_soon_threadsafe(task.cancel)

    def cancel(self, key):
        with self.condition:
            handle = self.handles.get(key)
            if not handle:
                return
            first_cancel = not handle['cancelled'].is_set()
            handle['cancelled'].set()
            if first_cancel and handle.get('future'):
                handle['future'].cancel()
            if first_cancel and handle.get('loop') and handle.get('task'):
                try:
                    handle['loop'].call_soon_threadsafe(handle['task'].cancel)
                except RuntimeError:
                    pass  # The worker loop may have just finished and closed.
            self.condition.notify_all()

    async def run(self, key, function):
        handle = {'cancelled': threading.Event()}
        with self.condition:
            self.handles[key] = handle

        def worker():
            acquired = False
            try:
                with self.condition:
                    while self.running >= self.limit() and not handle['cancelled'].is_set():
                        self.condition.wait(.2)
                    if handle['cancelled'].is_set():
                        raise asyncio.CancelledError()
                    self.running += 1
                    acquired = True
                return function(handle['cancelled'])
            except asyncio.CancelledError:
                # Thread futures represent this as an exception, not cancellation.
                # A normal sentinel avoids shield logging an unhandled BaseException.
                return _CANCELLED
            finally:
                if acquired:
                    with self.condition:
                        self.running -= 1
                        self.condition.notify_all()

        future = self.executor.submit(worker)
        with self.condition:
            handle['future'] = future
        wrapped = asyncio.wrap_future(future)
        try:
            result = await asyncio.shield(wrapped)
            if result is _CANCELLED:
                raise AgentStopped('Agent stopped by user')
            return result
        except asyncio.CancelledError:
            self.cancel(key)
            if not asyncio.current_task().cancelling():
                raise AgentStopped('Agent stopped by user')
            # Do not purge a worker's temp files while its subprocess is stopping.
            while not wrapped.done():
                try:
                    await asyncio.shield(wrapped)
                except asyncio.CancelledError:
                    continue
                except BaseException:
                    break
            try:
                wrapped.result()
            except BaseException:
                pass
            raise
        finally:
            with self.condition:
                self.handles.pop(key, None)

    async def stop(self, key):
        self.cancel(key)
        with self.condition:
            future = self.handles.get(key, {}).get('future')
        if future is not None:
            wrapped = asyncio.wrap_future(future)
            while not wrapped.done():
                try:
                    await asyncio.shield(wrapped)
                except asyncio.CancelledError:
                    continue
                except BaseException:
                    break
            try:
                wrapped.result()
            except BaseException:
                pass

    async def close(self):
        with self.condition:
            keys = list(self.handles)
        for key in keys:
            self.cancel(key)
        await asyncio.to_thread(self.executor.shutdown, wait=True, cancel_futures=True)
