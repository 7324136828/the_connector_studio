"""Bounded provider retry budgets, frozen task requests and cancellation."""
import asyncio
from copy import deepcopy
import json
import threading
import time
import unittest
import zipfile
from types import SimpleNamespace
from pathlib import Path
from unittest.mock import AsyncMock, patch

import httpx

from backend.app.services.connector import Connector, ConnectorError, ConnectorTimeout, ProviderTimeoutExhausted
from backend.app.services.pipeline import DeadlineConnector
from backend.app.services.task_context import CONTINUE_PROMPT
from backend.app.services import lattice
from backend.tests import test_effort_pipeline as fixtures

URL = 'http://127.0.0.1:8301/v1'
ORIGINAL = 'Finish the exact original task.\nPreserve C:\\project\\input.json and \u4f60\u597d.'
TOOLS = [{'type': 'function', 'function': {'name': 'inspect', 'parameters': {'type': 'object'}}}]


class DeadlineConnectorTests(unittest.IsolatedAsyncioTestCase):
    async def test_each_effort_has_exact_timeout_attempt_budget_and_immutable_payload(self):
        for effort in ('low', 'medium', 'high', 'extra_high', 'max'):
            with self.subTest(effort=effort):
                requests, events = [], []
                class Provider:
                    async def turn(self, url, model, messages, tools=None, **kwargs):
                        requests.append((deepcopy(messages), deepcopy(tools), kwargs))
                        # A connector cannot alter a subsequent retry's request.
                        messages[0]['text'] = 'mutated connector input'
                        tools.clear()
                        raise ConnectorTimeout('fixture deadline')
                messages = [{'role': 'user', 'text': ORIGINAL}]
                adapter = DeadlineConnector(Provider(), 1.25, 2, effort,
                    lambda *event: events.append(event))
                with self.assertRaises(ProviderTimeoutExhausted) as error:
                    await adapter.turn(URL, 'test-config', messages, TOOLS, validate_model=False)
                attempts = 1 if effort == 'low' else 3
                self.assertEqual(error.exception.attempts, attempts)
                self.assertEqual(len(requests), attempts)
                self.assertEqual(len(events), attempts - 1)
                for copied_messages, tools, options in requests:
                    self.assertEqual(copied_messages, messages)
                    self.assertEqual(copied_messages[0]['text'], ORIGINAL)
                    self.assertEqual(tools, TOOLS)
                    self.assertEqual(options, {'validate_model': False, 'response_timeout': 1.25})

    async def test_successful_model_get_is_not_repeated_when_post_times_out(self):
        models, posts = [], []
        async def respond(request):
            if request.url.path.endswith('/models'):
                models.append(request)
                if len(models) == 1:
                    raise httpx.ReadTimeout('model discovery deadline')
                return httpx.Response(200, json={'data': [{'id': 'test-config'}]})
            posts.append(request.content)
            if len(posts) < 3:
                raise httpx.ReadTimeout('completion deadline')
            return fixtures.completion('Done')
        async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
            connector = Connector(SimpleNamespace(provider_response_timeout=120), client)
            adapter = DeadlineConnector(connector, 300.5, 2, 'medium')
            result = await adapter.turn(URL, 'test-config', [{'role': 'user', 'text': ORIGINAL}], TOOLS)
        self.assertEqual(result['text'], 'Done')
        self.assertEqual(len(models), 2)
        self.assertEqual(len(posts), 3)
        self.assertTrue(all(body == posts[0] for body in posts))
        for request in models:
            self.assertEqual(request.extensions['timeout']['read'], 300.5)
            self.assertEqual(request.extensions['timeout']['connect'], 10)

    async def test_whole_response_deadline_cancels_unresponsive_transport_and_retries(self):
        attempts, cancellations = [], []
        async def respond(request):
            attempts.append(request)
            try:
                await asyncio.sleep(30)
            except asyncio.CancelledError:
                cancellations.append(request)
                raise
        async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
            connector = Connector(SimpleNamespace(provider_response_timeout=120), client)
            adapter = DeadlineConnector(connector, .01, 1, 'high')
            with self.assertRaises(ProviderTimeoutExhausted):
                await adapter.turn(URL, 'test-config', [{'role': 'user', 'text': ORIGINAL}], validate_model=False)
        self.assertEqual(len(attempts), 2)
        self.assertEqual(len(cancellations), 2)

    async def test_cancellation_during_retry_callback_prevents_next_provider_request(self):
        entered = asyncio.Event()
        calls = []
        class Provider:
            async def turn(self, *args, **kwargs):
                calls.append(args)
                raise ConnectorTimeout('deadline')
        async def retry(*args):
            entered.set()
            await asyncio.sleep(30)
        adapter = DeadlineConnector(Provider(), 1, 10, 'max', retry)
        task = asyncio.create_task(adapter.turn(URL, 'test-config', [{'role': 'user', 'text': ORIGINAL}], validate_model=False))
        await asyncio.wait_for(entered.wait(), 1)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertEqual(len(calls), 1)

    async def test_generic_failure_is_never_retried_by_timeout_adapter(self):
        calls = []
        class Provider:
            async def turn(self, *args, **kwargs):
                calls.append(args)
                raise ConnectorError('generic server failure')
        adapter = DeadlineConnector(Provider(), 1, 10, 'max')
        with self.assertRaisesRegex(ConnectorError, 'generic server failure'):
            await adapter.turn(URL, 'test-config', [{'role': 'user', 'text': ORIGINAL}], validate_model=False)
        self.assertEqual(len(calls), 1)


class TimeoutPipelineTests(unittest.TestCase):
    # Reuse the existing isolated project fixture without inheriting its tests.
    setUp = fixtures.EffortPipelineTests.setUp
    tearDown = fixtures.EffortPipelineTests.tearDown
    classification = staticmethod(fixtures.EffortPipelineTests.classification)
    default_handler = fixtures.EffortPipelineTests.default_handler
    effort = fixtures.EffortPipelineTests.effort
    start = fixtures.EffortPipelineTests.start
    wait = fixtures.EffortPipelineTests.wait
    current = fixtures.EffortPipelineTests.current
    small_context = fixtures.EffortPipelineTests.small_context
    assert_original_prompt = fixtures.EffortPipelineTests.assert_original_prompt
    assert_public_and_enabled_tools = fixtures.EffortPipelineTests.assert_public_and_enabled_tools

    def configure(self, timeout=1.25, retries=2):
        return self.app.state.project_memory.set_preferences(self.project['id'],
            provider_response_timeout=timeout, provider_timeout_retries=retries)

    def test_all_efforts_stop_at_timeout_exhaustion_without_extra_max_recovery(self):
        self.configure()
        async def handler(body):
            raise httpx.ReadTimeout('isolated deadline fixture')
        self.handler = handler
        for effort in ('low', 'medium', 'high', 'extra_high', 'max'):
            with self.subTest(effort=effort):
                self.requests.clear()
                started = self.start(effort, ORIGINAL)
                self.assertEqual(started['provider_response_timeout'], 1.25)
                self.assertEqual(started['provider_timeout_retries'], 2)
                job = self.wait(started['id'])
                attempts = 1 if effort == 'low' else 3
                self.assertEqual(job['status'], 'failed', job)
                self.assertTrue(job['provider_timeout_exhausted'])
                self.assertEqual(len(self.requests), attempts)
                self.assertTrue(all(body == self.requests[0] for body in self.requests))
                self.assertTrue(all(self.classification(body) == 'work' for body in self.requests))
                self.assertIn(f'after {attempts} attempt(s)', job['error_message'])
                retries = [line for line in job['logs'] if 'Retrying request' in line]
                self.assertEqual(len(retries), attempts - 1)
                session = self.current()
                self.assertEqual(session['draft'], ORIGINAL)
                self.assertIsNone(session['pending_job'])
                self.assertEqual(session['execution_state']['phase'], 'failed')
                self.assertEqual(session['execution_state']['summary_count'], 0)
                if effort != 'low':
                    self.assertEqual(session['execution_state']['retry_attempt'], 2)
                    self.assertEqual(session['execution_state']['retry_limit'], 2)
                    self.assertEqual(session['execution_state']['provider_response_timeout'], 1.25)
                    self.assertEqual(lattice.decode(Path(session['saved_path']).read_bytes())['execution_state'], session['execution_state'])
                self.assert_public_and_enabled_tools(ORIGINAL)

    def test_internal_agent_jobs_obey_every_effort_timeout_policy(self):
        # Low delegation cannot be created through the UI, but migrated/internal
        # agent records still receive the same authoritative execution policy.
        self.configure(retries=2)
        async def handler(body):
            raise httpx.ReadTimeout('internal child timeout fixture')
        self.handler = handler
        for effort in ('low', 'medium', 'high', 'extra_high', 'max'):
            with self.subTest(effort=effort):
                self.requests.clear()
                files = self.app.state.session_files
                child = files.new('Internal agent ' + effort, self.project['id'], persist=False)
                child = files.update(child['id'], lambda session: session.update(
                    is_agent=True, hidden=True, read_only=True, agent_name='Internal worker',
                    agent_status='queued', parent_session_id=self.session['id'],
                    model='test-config', effort=effort, messages=[{'role': 'user', 'text': ORIGINAL}]))
                job = self.app.state.pipeline.new(child, 'agent', ORIGINAL)
                files.update(child['id'], lambda session: session.update(pending_job=job['id']))
                self.client.portal.call(self.app.state.pipeline.chat, job, URL, None, True)
                state = self.wait(job['id'])
                self.assertEqual(state['status'], 'failed', state)
                self.assertEqual(len(self.requests), 1 if effort == 'low' else 3)
                self.assertTrue(all(body == self.requests[0] for body in self.requests))
                self.assertTrue(all(self.classification(body) == 'work' for body in self.requests))
                saved = self.app.state.store.get('session', child['id'])
                self.assertEqual(saved['agent_status'], 'failed')
                self.assertIsNone(saved['pending_job'])
                self.assertEqual(saved['messages'][0]['text'], ORIGINAL)

    def test_zero_retries_stops_medium_after_one_attempt(self):
        self.configure(retries=0)
        async def handler(body):
            raise httpx.ReadTimeout('isolated deadline fixture')
        self.handler = handler
        job = self.wait(self.start('medium', ORIGINAL)['id'])
        self.assertEqual(job['status'], 'failed', job)
        self.assertEqual(len(self.requests), 1)
        self.assertFalse(any('Retrying request' in line for line in job['logs']))

    def test_success_after_retry_preserves_logs_original_prompt_and_tools(self):
        self.configure(retries=1)
        work = 0
        async def handler(body):
            nonlocal work
            if self.classification(body) == 'probe':
                return fixtures.completion('{"continue":"no"}')
            work += 1
            if work == 1:
                raise httpx.ReadTimeout('first attempt timed out')
            return fixtures.completion('Verified public result after retry.')
        self.handler = handler
        job = self.wait(self.start('medium', ORIGINAL)['id'])
        self.assertEqual(job['status'], 'completed', job)
        self.assertEqual([self.classification(body) for body in self.requests], ['work', 'work', 'probe'])
        self.assertEqual(self.requests[0], self.requests[1])
        self.assertEqual(sum('Retrying request' in line for line in job['logs']), 1)
        self.assertEqual(self.current()['messages'][-1]['text'], 'Verified public result after retry.')
        self.assert_public_and_enabled_tools(ORIGINAL)
        archive = self.app.state.store.get('job', job['id'])['zip_path']
        with zipfile.ZipFile(archive) as export:
            self.assertIn('Retrying request (1/1)', export.read('logs.txt').decode())

    def test_completed_tool_is_not_reexecuted_when_following_provider_turn_retries(self):
        self.configure(retries=1)
        count = 0
        async def handler(body):
            nonlocal count
            if self.classification(body) == 'probe':
                return fixtures.completion('{"continue":"no"}')
            count += 1
            if count == 1:
                return fixtures.completion(calls=[fixtures.tool_call('run_python_script', {'code': 'print(42)'})])
            if count == 2:
                raise httpx.ReadTimeout('timeout after tool result')
            return fixtures.completion('Tool returned 42.')
        self.handler = handler
        executed = AsyncMock(return_value={'stdout': '42\n', 'stderr': '', 'exit_code': 0})
        with patch.object(self.app.state.tool_runtime, 'execute', executed):
            job = self.wait(self.start('medium', ORIGINAL)['id'])
        self.assertEqual(job['status'], 'completed', job)
        self.assertEqual(executed.await_count, 1)
        self.assertEqual(self.requests[1], self.requests[2])
        self.assertEqual(self.requests[1]['messages'][-1]['role'], 'tool')
        self.assertEqual(self.current()['execution_traces'][0]['steps'][0]['status'], 'completed')
        self.assert_public_and_enabled_tools(ORIGINAL)

    def test_continuation_probe_retries_same_control_and_original_prompt(self):
        self.configure(retries=2)
        probes = 0
        async def handler(body):
            nonlocal probes
            if self.classification(body) != 'probe':
                return fixtures.completion('Public result before probing.')
            probes += 1
            if probes < 3:
                raise httpx.ReadTimeout('probe timeout')
            return fixtures.completion('{"continue":"no"}')
        self.handler = handler
        job = self.wait(self.start('medium', ORIGINAL)['id'])
        self.assertEqual(job['status'], 'completed', job)
        self.assertEqual([self.classification(body) for body in self.requests], ['work', 'probe', 'probe', 'probe'])
        self.assertTrue(all(body == self.requests[1] for body in self.requests[1:]))
        self.assertEqual(sum(message['content'] == CONTINUE_PROMPT for message in self.requests[1]['messages']), 1)
        self.assertEqual(sum('Retrying request' in line for line in job['logs']), 2)
        self.assertTrue(any(update['text'] == 'Public result before probing.' for update in self.current()['live_updates']))
        self.assert_public_and_enabled_tools(ORIGINAL)

    def seed_history(self):
        history = []
        for index in range(8):
            history.extend([{'role': 'user', 'text': 'Prior task ' + str(index) + ': ' + 'u' * 1200},
                            {'role': 'assistant', 'text': 'Prior public result: ' + 'a' * 1200}])
        self.app.state.session_files.update(self.session['id'], lambda current: current.update(messages=history))

    def test_summary_synthesis_retries_without_tools_and_keeps_original_prompt(self):
        self.configure(retries=1)
        self.seed_history()
        summaries = 0
        async def handler(body):
            nonlocal summaries
            kind = self.classification(body)
            if kind == 'summary':
                summaries += 1
                if summaries == 1:
                    raise httpx.ReadTimeout('synthesis timeout')
                return fixtures.completion('A public summary of completed project work.')
            return fixtures.completion('{"continue":"no"}' if kind == 'probe' else 'Completed with compacted history.')
        self.handler = handler
        with self.small_context():
            job = self.wait(self.start('high', ORIGINAL)['id'])
        self.assertEqual(job['status'], 'completed', job)
        self.assertEqual(self.requests[0], self.requests[1])
        self.assertNotIn('tools', self.requests[0])
        self.assertGreaterEqual(summaries, 2)
        self.assert_public_and_enabled_tools(ORIGINAL)

    def test_max_summary_timeout_exhaustion_never_invokes_another_recovery(self):
        self.configure(retries=2)
        self.seed_history()
        async def handler(body):
            self.assertEqual(self.classification(body), 'summary')
            raise httpx.ReadTimeout('summary permanently timed out')
        self.handler = handler
        with self.small_context():
            job = self.wait(self.start('max', ORIGINAL)['id'])
        self.assertEqual(job['status'], 'failed', job)
        self.assertEqual(len(self.requests), 3)
        self.assertTrue(all(body == self.requests[0] for body in self.requests))
        self.assertFalse(self.contexts[-1].recovery_used)
        self.assert_original_prompt(ORIGINAL)

    def test_cancelling_a_retry_awaits_stop_and_sends_no_more_requests(self):
        self.configure(retries=4)
        entered = threading.Event()
        async def handler(body):
            if len(self.requests) == 1:
                raise httpx.ReadTimeout('first attempt timed out')
            entered.set()
            await asyncio.sleep(30)
            return fixtures.completion('Must not finish after user cancellation.')
        self.handler = handler
        started = self.start('high', ORIGINAL)
        self.assertTrue(entered.wait(4))
        current = self.current()
        self.assertEqual(current['execution_state']['phase'], 'retrying')
        self.assertEqual(current['execution_state']['retry_attempt'], 1)
        response = self.client.post('/api/jobs/' + started['id'] + '/discard')
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(self.wait(started['id'])['status'], 'discarded')
        time.sleep(.05)
        self.assertEqual(len(self.requests), 2)
        session = self.current()
        self.assertIsNone(session['pending_job'])
        self.assertEqual(session['draft'], ORIGINAL)
        self.assertEqual(session['execution_state']['phase'], 'cancelled')
        self.assertEqual(sum('Retrying request' in line for line in self.wait(started['id'])['logs']), 1)

    def test_failed_child_inherits_parent_policy_and_cannot_restart_after_exhaustion(self):
        self.configure(timeout=1.75, retries=1)
        parent_work = 0
        child_task = 'Child task with exact original prompt.'
        async def handler(body):
            nonlocal parent_work
            users = [message['content'] for message in body['messages'] if message['role'] == 'user']
            if child_task in users:
                raise httpx.ReadTimeout('child permanently timed out')
            if self.classification(body) == 'probe':
                return fixtures.completion('{"continue":"no"}')
            parent_work += 1
            if parent_work == 1:
                # Settings changed mid-flight must not affect this parent's child.
                self.configure(timeout=9.5, retries=5)
            if parent_work <= 2:
                return fixtures.completion(calls=[fixtures.tool_call('run_agent', {'name': 'Worker', 'task': child_task})])
            return fixtures.completion('Reviewed the failed child and completed independent work.')
        self.handler = handler
        self.effort('max')
        response = self.client.patch('/api/projects/' + self.project['id'] + '/resources',
            json={'id': '.skill/run_agent', 'enabled': True, 'effort': 'max'})
        self.assertEqual(response.status_code, 200, response.text)
        job = self.wait(self.start('max', ORIGINAL)['id'])
        self.assertEqual(job['status'], 'completed', job)
        self.assertEqual(job['failed_agents'], ['worker'])
        children = [item for item in self.client.get('/api/jobs').json() if item.get('parent_job_id') == job['id']]
        self.assertEqual(len(children), 1)
        child = children[0]
        self.assertEqual(child['status'], 'failed', child)
        self.assertEqual(child['provider_response_timeout'], 1.75)
        self.assertEqual(child['provider_timeout_retries'], 1)
        self.assertTrue(child['provider_timeout_exhausted'])
        child_requests = [body for body in self.requests if child_task in
            [message['content'] for message in body['messages'] if message['role'] == 'user']]
        self.assertEqual(len(child_requests), 2)
        self.assertEqual(child_requests[0], child_requests[1])
        self.assertTrue(all(self.classification(body) == 'work' for body in child_requests))
        parent_steps = self.current()['execution_traces'][0]['steps']
        self.assertEqual([step['status'] for step in parent_steps], ['failed', 'failed'])
        self.assertIn('exhausted its provider timeout retries', parent_steps[1]['result']['error'])

    def test_individual_child_stop_during_retry_preserves_parent_execution(self):
        self.configure(retries=3)
        entered = threading.Event()
        child_task = 'Cancellable child original task.'
        child_calls = 0
        async def handler(body):
            nonlocal child_calls
            users = [message['content'] for message in body['messages'] if message['role'] == 'user']
            if child_task in users:
                child_calls += 1
                if child_calls == 1:
                    raise httpx.ReadTimeout('child first timeout')
                entered.set()
                await asyncio.sleep(30)
                return fixtures.completion('Must not respond after child stop.')
            if self.classification(body) == 'probe':
                return fixtures.completion('{"continue":"no"}')
            if body['messages'][-1]['role'] == 'tool':
                return fixtures.completion('The parent continued after child cancellation.')
            return fixtures.completion(calls=[fixtures.tool_call('run_agent', {'name': 'Stop Worker', 'task': child_task})])
        self.handler = handler
        self.effort('max')
        response = self.client.patch('/api/projects/' + self.project['id'] + '/resources',
            json={'id': '.skill/run_agent', 'enabled': True, 'effort': 'max'})
        self.assertEqual(response.status_code, 200, response.text)
        parent = self.start('max', ORIGINAL)
        self.assertTrue(entered.wait(4))
        children = [item for item in self.client.get('/api/jobs').json() if item.get('parent_job_id') == parent['id']]
        self.assertEqual(len(children), 1)
        child = children[0]
        response = self.client.post('/api/jobs/' + child['id'] + '/discard')
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(self.wait(child['id'])['status'], 'discarded')
        job = self.wait(parent['id'])
        self.assertEqual(job['status'], 'completed', job)
        self.assertEqual(child_calls, 2)
        self.assertEqual(job['stopped_agents'], ['stop-worker'])
