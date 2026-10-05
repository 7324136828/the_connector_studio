"""Effort harness requests, public partial persistence and authoritative tool gates."""
import asyncio
import json
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import httpx
from fastapi.testclient import TestClient

from backend.app.config import Settings
from backend.app.main import create_app
from backend.app.services import lattice
from backend.app.services.task_context import EffortContext


def completion(text=None, calls=None):
    message = {'role': 'assistant', 'content': text, 'reasoning_content': 'private-effort-fixture'}
    if calls:
        message['tool_calls'] = calls
    return httpx.Response(200, json={'choices': [{'message': message}]})


def tool_call(name, arguments):
    return {'id': 'call_' + name, 'type': 'function', 'function': {'name': name, 'arguments': json.dumps(arguments)}}


class EffortPipelineTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='studio-effort-integration-')
        self.root = Path(self.directory.name).resolve()
        self.requests = []
        self.lock = threading.Lock()
        self.gates = []
        self.handler = self.default_handler
        self.contexts = []
        async def respond(request):
            if request.url.path == '/v1/models':
                return httpx.Response(200, json={'data': [{'id': 'test-config'}]})
            body = json.loads(request.content)
            with self.lock:
                self.requests.append(body)
            return await self.handler(body)
        self.app = create_app(Settings(data_dir=self.root / 'data', workspace_root=self.root / 'workspace', browser_roots=[self.root]), httpx.MockTransport(respond))
        self.context = TestClient(self.app)
        self.client = self.context.__enter__()
        response = self.client.post('/api/projects', json={'name': 'Effort integration'})
        self.assertEqual(response.status_code, 201, response.text)
        self.project = response.json()
        response = self.client.post('/api/sessions', json={'title': 'Effort conversation', 'project_id': self.project['id']})
        self.assertEqual(response.status_code, 201, response.text)
        self.session = response.json()

    def tearDown(self):
        for gate in self.gates:
            gate.set()
        self.context.__exit__(None, None, None)
        for path in self.app.state.pipeline.temp.root.glob('job-*'):
            self.app.state.pipeline.temp.purge(path)
        self.directory.cleanup()

    @staticmethod
    def classification(body):
        if body['messages'][0]['content'].startswith('Write a concise public task progress summary'):
            return 'summary'
        return 'probe' if body['messages'][-1]['content'].startswith('Can you continue?') else 'work'

    async def default_handler(self, body):
        return completion('{"continue":"no"}' if self.classification(body) == 'probe' else 'A final public answer.')

    def effort(self, level):
        response = self.client.patch('/api/sessions/' + self.session['id'], json={'effort': level})
        self.assertEqual(response.status_code, 200, response.text)

    def start(self, level='low', text='Perform the requested project task.'):
        self.effort(level)
        response = self.client.post('/api/sessions/' + self.session['id'] + '/messages', json={'text': text, 'model': 'test-config'})
        self.assertEqual(response.status_code, 202, response.text)
        return response.json()

    def wait(self, job_id, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            job = self.client.get('/api/jobs/' + job_id).json()
            if job['status'] not in ('queued', 'in_progress'):
                return job
            time.sleep(.01)
        self.fail('Effort job did not settle')

    def current(self):
        return self.client.get('/api/sessions/' + self.session['id']).json()

    def small_context(self, limit=5000):
        def factory(effort, messages, **kwargs):
            context = EffortContext(effort, messages, token_limit=limit, **kwargs)
            self.contexts.append(context)
            return context
        return patch('backend.app.services.pipeline.EffortContext', side_effect=factory)

    def assert_original_prompt(self, original):
        with self.lock:
            requests = list(self.requests)
        self.assertTrue(requests)
        for body in requests:
            if self.classification(body) == 'summary':
                source = json.loads(body['messages'][1]['content'])
                task = source['current_task']
                self.assertEqual(task.get('content', task.get('text')), original)
            else:
                self.assertTrue(any(message['role'] == 'user' and message['content'] == original
                                    for message in body['messages']), body['messages'])

    def assert_public_and_enabled_tools(self, original='Perform the requested project task.'):
        with self.lock:
            requests = list(self.requests)
        work = [body for body in requests if self.classification(body) != 'summary']
        self.assertTrue(work)
        schemas = work[0]['tools']
        for body in work:
            self.assertEqual(body['tools'], schemas)
            self.assertEqual(body['tool_choice'], 'auto')
        session = self.current()
        self.assertNotIn('private-effort-fixture', json.dumps(session))
        self.assertNotIn('private-effort-fixture', json.dumps(requests))
        if session.get('saved_path'):
            self.assertNotIn(b'private-effort-fixture', Path(session['saved_path']).read_bytes())
        self.assert_original_prompt(original)

    def test_low_returns_after_one_model_answer_without_continuation_probe(self):
        job = self.wait(self.start()['id'])
        self.assertEqual(job['status'], 'completed', job)
        self.assertEqual(len(self.requests), 1)
        session = self.current()
        self.assertEqual(session['messages'][-1]['text'], 'A final public answer.')
        self.assertFalse(session.get('live_updates'))
        self.assertNotIn('execution_state', session)
        self.assertEqual(lattice.decode(Path(session['saved_path']).read_bytes())['messages'][-1]['text'], 'A final public answer.')
        self.assert_public_and_enabled_tools()

    def test_medium_yes_then_no_retains_partial_results_and_tools_on_every_request(self):
        counts = {'work': 0, 'probe': 0}
        async def handler(body):
            kind = self.classification(body)
            counts[kind] += 1
            if kind == 'probe':
                return completion(json.dumps({'continue': 'yes' if counts[kind] == 1 else 'no'}))
            return completion('Public stage ' + str(counts[kind]))
        self.handler = handler
        job = self.wait(self.start('medium')['id'])
        self.assertEqual(job['status'], 'completed', job)
        self.assertEqual([self.classification(body) for body in self.requests], ['work', 'probe', 'work', 'probe'])
        self.assertIn('Public stage 1', json.dumps(self.requests[2]['messages']))
        session = self.current()
        self.assertEqual([item['text'] for item in session['live_updates']], ['Public stage 1', 'Public stage 2'])
        self.assertEqual(session['messages'][-1]['text'], 'Public stage 2')
        restored = lattice.decode(Path(session['saved_path']).read_bytes())
        self.assertEqual(restored['live_updates'], session['live_updates'])
        self.assertEqual(restored['effort'], 'medium')
        self.assertEqual(restored['execution_state']['phase'], 'completed')
        self.assert_public_and_enabled_tools()

    def test_high_extra_high_and_max_compact_public_interactions_at_their_windows(self):
        for level, multiplier in [('high', 1), ('extra_high', 2), ('max', 2.5)]:
            with self.subTest(effort=level):
                self.requests.clear()
                counts = {'work': 0, 'probe': 0, 'summary': 0}
                async def handler(body):
                    kind = self.classification(body)
                    counts[kind] += 1
                    if kind == 'summary':
                        self.assertNotIn('tools', body)
                        return completion('Public summary: inspected input; remaining checks continue.')
                    if kind == 'probe':
                        return completion(json.dumps({'continue': 'yes' if counts[kind] < 3 else 'no'}))
                    return completion(('Stage ' + str(counts[kind]) + ': ' + 'x' * 9000) if counts[kind] < 3 else 'Final verified public result.')
                self.handler = handler
                with self.small_context():
                    job = self.wait(self.start(level)['id'])
                self.assertEqual(job['status'], 'completed', job)
                self.assertGreaterEqual(counts['summary'], 1)
                self.assertEqual(self.contexts[-1].window_limit, int(5000 * multiplier))
                session = self.current()
                self.assertGreaterEqual(session['execution_state']['summary_count'], 1)
                self.assertLessEqual(session['execution_state']['context_tokens'], int(5000 * multiplier))
                self.assertTrue(any(item['kind'] == 'status' and item['text'].startswith('Public summary:') for item in session['live_updates']))
                self.assert_public_and_enabled_tools()
                self.app.state.session_files.update(self.session['id'], lambda current: current.update(messages=[], live_updates=[], execution_state={'phase': 'completed', 'iteration': 0}))
                self.app.state.store.update('project', self.project['id'], lambda project: project.update(preferences={}))
                memory_path = Path(self.project['path']) / '.memory' / 'interactions'
                for path in memory_path.glob('*.json'):
                    path.unlink()

    def seed_repeated_task_history(self, original):
        history = [{'role': 'user', 'text': original},
                   {'role': 'assistant', 'text': 'A previous answer to the same original request.'}]
        for index in range(10):
            history.extend([{'role': 'user', 'text': f'Older task {index}: ' + 'u' * 1200},
                            {'role': 'assistant', 'text': f'Older public result {index}: ' + 'a' * 1200}])
        self.app.state.session_files.update(self.session['id'], lambda current: current.update(
            messages=history, live_updates=[], execution_state={'phase': 'completed', 'iteration': 0}))

    def test_verbatim_original_prompt_survives_repeated_work_probes_and_compaction(self):
        original = 'Finish "this exact project request" for \u4f60\u597d.\nRead C:\\project\\input.json.\nPreserve all original constraints and scope.'
        for level in ('medium', 'high', 'extra_high', 'max'):
            with self.subTest(effort=level):
                self.requests.clear()
                self.seed_repeated_task_history(original)
                counts = {'work': 0, 'probe': 0, 'summary': 0}
                async def handler(body):
                    kind = self.classification(body)
                    counts[kind] += 1
                    if kind == 'summary':
                        return completion('Public summary intentionally omits the original request wording.')
                    if kind == 'probe':
                        return completion(json.dumps({'continue': 'yes' if counts[kind] < 3 else 'no'}))
                    return completion(('Public stage ' + str(counts[kind]) + ': ' + 'r' * 5000)
                                      if counts[kind] < 3 else 'The original project task is complete.')
                self.handler = handler
                with self.small_context(6000):
                    job = self.wait(self.start(level, original)['id'])
                self.assertEqual(job['status'], 'completed', job)
                self.assertEqual(counts['work'], 3)
                self.assertEqual(counts['probe'], 3)
                self.assertEqual(self.contexts[-1].task['text'], original)
                if level == 'medium':
                    self.assertEqual(counts['summary'], 0)
                else:
                    self.assertGreater(counts['summary'], 0)
                self.assert_public_and_enabled_tools(original)
                # Clear only this isolated fixture's completed project recall.
                for path in (Path(self.project['path']) / '.memory' / 'interactions').glob('*.json'):
                    path.unlink()

    def test_max_recovery_keeps_original_prompt_after_summary_of_summaries_and_yes(self):
        original = 'Resume the original "Max task".\nKeep C:\\project\\source.txt and \u4f60\u597d exactly as requested.'
        self.seed_repeated_task_history(original)
        counts = {'work': 0, 'probe': 0, 'summary': 0}
        async def handler(body):
            kind = self.classification(body)
            counts[kind] += 1
            if kind == 'summary':
                return completion('Public progress summary intentionally omits the original task wording.')
            if kind == 'probe':
                return completion(json.dumps({'continue': 'yes' if counts[kind] < 3 else 'no'}))
            if counts[kind] == 2:
                raise httpx.ReadTimeout('Fixture stalled after continuation')
            return completion('First public checkpoint.' if counts[kind] == 1 else 'Verified the original task after recovery.')
        self.handler = handler
        with self.small_context(6000):
            job = self.wait(self.start('max', original)['id'])
        self.assertEqual(job['status'], 'completed', job)
        self.assertEqual(counts['work'], 3)
        self.assertEqual(counts['probe'], 3)
        self.assertGreaterEqual(counts['summary'], 2)
        self.assertTrue(self.contexts[-1].recovery_used)
        summaries = [json.loads(body['messages'][1]['content']) for body in self.requests
                     if self.classification(body) == 'summary']
        self.assertTrue(any(source['earlier_public_summaries'] for source in summaries))
        self.assertEqual(self.current()['messages'][-1]['text'], 'Verified the original task after recovery.')
        self.assert_public_and_enabled_tools(original)

    def test_invalid_continuation_json_does_not_implicitly_continue(self):
        async def handler(body):
            return completion('yes' if self.classification(body) == 'probe' else 'The first public result.')
        self.handler = handler
        job = self.wait(self.start('medium')['id'])
        self.assertEqual(job['status'], 'failed', job)
        self.assertIn('only continue', job['error_message'])
        self.assertEqual([self.classification(body) for body in self.requests], ['work', 'probe'])
        session = self.current()
        self.assertIsNone(session['pending_job'])
        self.assertEqual(session['live_updates'][0]['text'], 'The first public result.')
        self.assertEqual(session['execution_state']['phase'], 'failed')

    def test_max_nonresponse_summarizes_then_no_stops_without_another_work_request(self):
        counts = {'work': 0, 'probe': 0, 'summary': 0}
        async def handler(body):
            kind = self.classification(body)
            counts[kind] += 1
            if kind == 'summary':
                return completion('Public recovery summary: first result received; later connection stalled.')
            if kind == 'probe':
                return completion(json.dumps({'continue': 'yes' if counts[kind] == 1 else 'no'}))
            if counts[kind] == 2:
                raise httpx.ReadTimeout('Fixture connector stopped responding')
            return completion('First public candidate answer.')
        self.handler = handler
        job = self.wait(self.start('max')['id'])
        self.assertEqual(job['status'], 'completed', job)
        self.assertEqual([self.classification(body) for body in self.requests], ['work', 'probe', 'work', 'summary', 'probe'])
        self.assertEqual(self.current()['messages'][-1]['text'], 'First public candidate answer.')
        self.assertEqual(self.current()['execution_state']['summary_count'], 1)
        self.assert_public_and_enabled_tools()

    def test_cancel_during_continuation_probe_preserves_partial_and_sends_no_followup(self):
        entered, release = threading.Event(), threading.Event()
        self.gates.append(release)
        async def handler(body):
            if self.classification(body) == 'probe':
                entered.set()
                await asyncio.to_thread(release.wait)
                return completion('{"continue":"yes"}')
            return completion('A public partial result before the continuation check.')
        self.handler = handler
        job = self.start('medium')
        self.assertTrue(entered.wait(4))
        self.assertIsNotNone(self.current()['pending_job'])
        response = self.client.post('/api/jobs/' + job['id'] + '/discard')
        self.assertEqual(response.status_code, 200, response.text)
        release.set()
        self.assertEqual(self.wait(job['id'])['status'], 'discarded')
        self.assertEqual([self.classification(body) for body in self.requests], ['work', 'probe'])
        session = self.current()
        self.assertIsNone(session['pending_job'])
        self.assertEqual(session['execution_state']['phase'], 'cancelled')
        self.assertEqual(session['live_updates'][0]['text'], 'A public partial result before the continuation check.')
        self.assertEqual(lattice.decode(Path(session['saved_path']).read_bytes())['live_updates'], session['live_updates'])

    def test_cancel_during_summary_synthesis_never_sends_work_or_a_probe(self):
        entered, release = threading.Event(), threading.Event()
        self.gates.append(release)
        async def handler(body):
            self.assertEqual(self.classification(body), 'summary')
            entered.set()
            await asyncio.to_thread(release.wait)
            return completion('A summary that should not become a new work request.')
        self.handler = handler
        history = []
        for index in range(8):
            history.extend([{'role': 'user', 'text': 'Old task ' + str(index) + ': ' + 'u' * 1200}, {'role': 'assistant', 'text': 'Old public result: ' + 'a' * 1200}])
        self.app.state.session_files.update(self.session['id'], lambda current: current.update(messages=history))
        with self.small_context():
            job = self.start('high')
            self.assertTrue(entered.wait(4))
            response = self.client.post('/api/jobs/' + job['id'] + '/discard')
            self.assertEqual(response.status_code, 200, response.text)
        release.set()
        self.assertEqual(self.wait(job['id'])['status'], 'discarded')
        self.assertEqual([self.classification(body) for body in self.requests], ['summary'])
        self.assertEqual(self.current()['execution_state']['phase'], 'cancelled')

    def test_low_agent_calls_and_generic_agent_creation_are_rejected_authoritatively(self):
        async def handler(body):
            if body['messages'][-1]['role'] == 'tool':
                return completion('Agent tools were unavailable; no delegation performed.')
            return completion(calls=[tool_call('run_agent', {'name': 'blocked', 'task': 'Must not run'}), tool_call('create_agent', {'name': 'blocked-alias', 'instructions': 'Must not create'}), tool_call('create_tool_from_conversation', {'kind': 'agent', 'name': 'blocked-generic', 'instructions': 'Must not create'})])
        self.handler = handler
        job = self.wait(self.start('low')['id'])
        self.assertEqual(job['status'], 'completed', job)
        trace = self.current()['execution_traces'][0]
        self.assertEqual([step['status'] for step in trace['steps']], ['failed', 'failed', 'failed'])
        self.assertFalse((Path(self.project['path']) / '.agent').exists())
        self.assertFalse(any(session.get('is_agent') for session in self.client.get('/api/sessions', params={'include_agents': True}).json()))
        exposed = {tool['function']['name'] for tool in self.requests[0]['tools']}
        self.assertNotIn('run_agent', exposed)
        self.assertNotIn('create_agent', exposed)
        generic = next(tool['function'] for tool in self.requests[0]['tools'] if tool['function']['name'] == 'create_tool_from_conversation')
        self.assertNotIn('agent', generic['parameters']['properties']['kind']['enum'])
        blocked = self.client.patch('/api/projects/' + self.project['id'] + '/resources', json={'id': '.skill/run_agent', 'enabled': True, 'effort': 'low'})
        self.assertEqual(blocked.status_code, 409, blocked.text)


    def test_max_opt_out_does_not_allow_generic_agent_creation(self):
        async def handler(body):
            if self.classification(body) == 'probe':
                return completion('{"continue":"no"}')
            if body['messages'][-1]['role'] == 'tool':
                return completion('Multi-agent mode stayed disabled.')
            return completion(calls=[tool_call('create_tool_from_conversation', {'kind': 'agent', 'name': 'blocked-opt-out', 'instructions': 'Must not create without opt-in'})])
        self.handler = handler
        job = self.wait(self.start('max')['id'])
        self.assertEqual(job['status'], 'completed', job)
        self.assertFalse((Path(self.project['path']) / '.agent').exists())
        self.assertEqual(self.current()['execution_traces'][0]['steps'][0]['status'], 'failed')
        self.assertIn('Enable the create_agent', self.current()['execution_traces'][0]['steps'][0]['result']['error'])

    def test_max_recovery_can_continue_once_then_a_second_stall_fails(self):
        async def handler(body):
            kind = self.classification(body)
            if kind == 'summary':
                return completion('A public recovery summary of the stalled task.')
            if kind == 'probe':
                return completion('{"continue":"yes"}')
            raise httpx.ReadTimeout('Fixture connector remains unavailable')
        self.handler = handler
        job = self.wait(self.start('max')['id'])
        self.assertEqual(job['status'], 'failed', job)
        self.assertEqual([self.classification(body) for body in self.requests], ['work', 'summary', 'probe', 'work'])
        self.assertEqual(self.current()['execution_state']['summary_count'], 1)
        self.assertEqual(self.current()['execution_state']['phase'], 'failed')
        self.assert_public_and_enabled_tools()

    def test_independent_child_stop_allows_parent_to_finish_and_cannot_restart_same_agent(self):
        child_entered = threading.Event()
        counts = {'parent_work': 0}
        async def handler(body):
            kind = self.classification(body)
            if kind == 'probe':
                return completion('{"continue":"no"}')
            users = [message['content'] for message in body['messages'] if message['role'] == 'user']
            if 'Long child task.' in users:
                child_entered.set()
                await asyncio.sleep(30)
                return completion('This child must not complete after user Stop.')
            counts['parent_work'] += 1
            if counts['parent_work'] <= 2:
                return completion(calls=[tool_call('run_agent', {'name': 'Worker', 'task': 'Long child task.'})])
            return completion('The parent completed after the child was stopped.')
        self.handler = handler
        self.effort('max')
        response = self.client.patch('/api/projects/' + self.project['id'] + '/resources', json={'id': '.skill/run_agent', 'enabled': True, 'effort': 'max'})
        self.assertEqual(response.status_code, 200, response.text)
        job = self.start('max')
        self.assertTrue(child_entered.wait(4))
        children = [session for session in self.client.get('/api/sessions', params={'include_agents': True}).json() if session.get('parent_session_id') == self.session['id']]
        self.assertEqual(len(children), 1)
        child = children[0]
        self.assertTrue(child['hidden'])
        self.assertTrue(child['read_only'])
        response = self.client.post('/api/jobs/' + child['pending_job'] + '/discard')
        self.assertEqual(response.status_code, 200, response.text)
        root_job = self.wait(job['id'])
        self.assertEqual(root_job['status'], 'completed', root_job)
        self.assertEqual(root_job['stopped_agents'], ['worker'])
        parent = self.current()
        self.assertEqual(parent['messages'][-1]['text'], 'The parent completed after the child was stopped.')
        steps = parent['execution_traces'][0]['steps']
        self.assertEqual([step['status'] for step in steps], ['cancelled', 'failed'])
        self.assertIn('stopped by the user', steps[1]['result']['error'])
        child = self.client.get('/api/sessions/' + child['id']).json()
        self.assertEqual(child['agent_status'], 'cancelled')
        self.assertIsNone(child['pending_job'])
        self.assertEqual(child['execution_state']['phase'], 'cancelled')
        self.assertEqual(lattice.decode(Path(child['saved_path']).read_bytes())['agent_status'], 'cancelled')
        self.assertEqual(len([item for item in self.client.get('/api/jobs').json() if item.get('kind') == 'agent']), 1)
