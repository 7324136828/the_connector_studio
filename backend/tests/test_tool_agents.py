"""Actual project script execution, concurrent agents, cancellation and portable traces."""
import asyncio
import ctypes
import json
import os
import tempfile
import threading
import time
import unittest
from pathlib import Path

import httpx
from fastapi.testclient import TestClient

from backend.app.config import Settings
from backend.app.main import create_app
from backend.app.services import lattice


def call(identifier, name, arguments):
    return {'id': identifier, 'type': 'function', 'function': {'name': name, 'arguments': json.dumps(arguments)}}


def process_is_running(pid):
    if os.name == 'nt':
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return False
        try:
            code = wintypes.DWORD()
            return bool(kernel.GetExitCodeProcess(handle, ctypes.byref(code))) and code.value == 259
        finally:
            kernel.CloseHandle(handle)
    try:
        proc_stat = Path('/proc') / str(pid) / 'stat'
        if proc_stat.is_file() and proc_stat.read_text().rsplit(') ', 1)[-1].split()[0] == 'Z':
            return False
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


class ToolAgentTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name).resolve()
        self.lock = threading.Lock()
        self.requests = []
        self.active_children = 0
        self.maximum_children = 0
        self.child_turns = []

        async def respond(request):
            if request.url.path == '/v1/models':
                return httpx.Response(200, json={'data': [{'id': 'test-config'}, {'id': 'test-config-alt'}]})
            body = json.loads(request.content)
            with self.lock:
                self.requests.append(body)
            messages = body['messages']
            user_index = next(index for index in range(len(messages) - 1, -1, -1) if messages[index]['role'] == 'user')
            prompt = messages[user_index]['content']
            results = [message for message in messages[user_index + 1:] if message['role'] == 'tool']
            calls = []
            if isinstance(prompt, str) and prompt.startswith('Can you continue?'):
                return httpx.Response(200, json={'choices': [{'message': {'role': 'assistant', 'content': '{"continue":"no"}'}}]})
            if prompt == 'iterative scripts':
                if not results:
                    calls = [call('python_stage', 'run_python_script', {'code': "import os,sys\nprint('python-result')\nprint(os.environ['STUDIO_ENVIRONMENT_NAME'])\nprint(sys.prefix)", 'timeout': 10})]
                elif len(results) == 1:
                    calls = [call('batch_stage', 'run_batch_script', {'script': 'python -c "print(\'batch-result\')"', 'timeout': 10})]
            elif prompt in ('parallel agents', 'serial agents', 'cancel agents') and not results:
                task_prefix = {'parallel agents': 'parallel-child', 'serial agents': 'serial-child', 'cancel agents': 'cancel-child'}[prompt]
                calls = [call('agent_left', 'run_agent', {'name': 'left', 'task': task_prefix + '-left', 'model': 'test-config-alt'}),
                         call('agent_right', 'run_agent', {'name': 'right', 'task': task_prefix + '-right', 'model': 'retired-config' if prompt == 'serial agents' else 'test-config'})]
            elif isinstance(prompt, str) and prompt.startswith(('parallel-child-', 'serial-child-')) and not results:
                with self.lock:
                    self.active_children += 1
                    self.maximum_children = max(self.maximum_children, self.active_children)
                    self.child_turns.append({'task': prompt, 'model': body['model'], 'thread': threading.current_thread().name, 'tools': body.get('tools', [])})
                try:
                    if prompt.startswith('parallel-child-'):
                        deadline = time.monotonic() + 4
                        while time.monotonic() < deadline:
                            with self.lock:
                                if self.active_children == 2:
                                    break
                            await asyncio.sleep(.01)
                    await asyncio.sleep(.15)
                finally:
                    with self.lock:
                        self.active_children -= 1
                calls = [call('child_python', 'run_python_script', {'code': 'print(' + repr(prompt) + ')', 'timeout': 10})]
            elif isinstance(prompt, str) and prompt.startswith('cancel-child-') and not results:
                code = ("import json,os,pathlib,subprocess,sys,time\n"
                        "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)'])\n"
                        "path=pathlib.Path(os.environ['STUDIO_PROJECT_DIR'])/" + repr(prompt + '.json') + "\n"
                        "path.write_text(json.dumps({'parent':os.getpid(),'child':child.pid}))\n"
                        "print('started',flush=True)\ntime.sleep(30)\n")
                calls = [call('child_long_python', 'run_python_script', {'code': code, 'timeout': 60})]
            elif prompt == 'disabled agent' and not results:
                calls = [call('disabled_agent', 'run_agent', {'name': 'blocked', 'task': 'Should never execute'})]
            elif prompt == 'disabled script' and not results:
                calls = [call('disabled_python', 'run_python_script', {'code': "import os,pathlib\npathlib.Path(os.environ['STUDIO_PROJECT_DIR'],'executed.txt').write_text('unexpected')", 'timeout': 10})]
            message = {'role': 'assistant', 'content': None if calls else 'Completed ' + str(prompt), 'reasoning_content': 'fixture-private-analysis'}
            if calls:
                message['tool_calls'] = calls
            return httpx.Response(200, json={'choices': [{'message': message}]})

        settings = Settings(data_dir=self.root / 'data', workspace_root=self.root / 'workspace', browser_roots=[self.root / 'workspace'])
        self.app = create_app(settings, httpx.MockTransport(respond))
        self.context = TestClient(self.app)
        self.client = self.context.__enter__()
        response = self.client.post('/api/projects', json={'name': 'Tool integration'})
        self.assertEqual(response.status_code, 201, response.text)
        self.project = response.json()
        response = self.client.post('/api/sessions', json={'title': 'Parent tool conversation', 'project_id': self.project['id']})
        self.assertEqual(response.status_code, 201, response.text)
        self.session = response.json()

    def tearDown(self):
        if self.context is not None:
            self.context.__exit__(None, None, None)
        for path in self.app.state.pipeline.temp.root.glob('job-*'):
            self.app.state.pipeline.temp.purge(path)
        self.directory.cleanup()

    def prepare_environment(self):
        response = self.client.post('/api/projects/' + self.project['id'] + '/environments', json={'name': 'default'})
        self.assertEqual(response.status_code, 201, response.text)
        environment = next(item for item in response.json()['environments'] if item['name'] == 'default')
        self.assertTrue(environment['ready'])
        return environment

    def limit(self, count):
        connection = self.client.get('/api/settings').json()
        response = self.client.post('/api/settings', json={'server_url': connection['server_url'], 'font_size': 14, 'max_parallel_agents': count})
        self.assertEqual(response.status_code, 200, response.text)

    def start(self, text):
        if text in ('parallel agents', 'serial agents', 'cancel agents', 'disabled agent'):
            self.client.patch('/api/sessions/' + self.session['id'], json={'effort': 'max'}).raise_for_status()
            resources = self.client.get('/api/projects/' + self.project['id'] + '/resources?effort=max').json()
            for entry in resources:
                if entry['name'] in ('run_agent', 'create_agent') and entry['kind'] == 'skill':
                    self.client.patch('/api/projects/' + self.project['id'] + '/resources', json={
                        'id': entry['id'], 'enabled': True, 'effort': 'max'}).raise_for_status()
        response = self.client.post('/api/sessions/' + self.session['id'] + '/messages', json={'text': text, 'model': 'test-config'})
        self.assertEqual(response.status_code, 202, response.text)
        return response.json()

    def wait(self, job_id, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            job = self.client.get('/api/jobs/' + job_id).json()
            if job['status'] not in ('queued', 'in_progress'):
                return job
            time.sleep(.02)
        self.fail('Tool request did not complete')

    def completed(self, text):
        job = self.wait(self.start(text)['id'])
        self.assertEqual(job['status'], 'completed', job)
        return job

    def current(self):
        response = self.client.get('/api/sessions/' + self.session['id'])
        self.assertEqual(response.status_code, 200, response.text)
        return response.json()

    def children(self):
        return [session for session in self.client.get('/api/sessions', params={'include_agents': True}).json() if session.get('parent_session_id') == self.session['id']]

    def test_iterative_python_and_batch_tools_use_project_environment_and_persist_trace(self):
        environment = self.prepare_environment()
        resources = self.client.get('/api/projects/' + self.project['id'] + '/resources').json()
        builtins = [resource for resource in resources if resource['kind'] == 'skill']
        self.assertEqual(len(builtins), 5)
        self.assertEqual({resource['name'] for resource in builtins if resource['enabled']},
                         {'run_python_script', 'run_batch_script', 'create_tool_from_conversation'})
        self.assertTrue(all(resource['locked'] for resource in builtins if resource['name'] in ('run_agent', 'create_agent')))
        job = self.completed('iterative scripts')
        session = self.current()
        trace = session['execution_traces'][0]
        self.assertEqual(trace['job_id'], job['id'])
        self.assertEqual(trace['status'], 'completed')
        self.assertEqual([step['tool_name'] for step in trace['steps']], ['run_python_script', 'run_batch_script'])
        self.assertTrue(all(step['status'] == 'completed' and step['result']['exit_code'] == 0 for step in trace['steps']))
        self.assertIn('python-result', trace['steps'][0]['result']['stdout'])
        self.assertIn(environment['path'], trace['steps'][0]['result']['stdout'])
        self.assertIn('batch-result', trace['steps'][1]['result']['stdout'])
        saved = lattice.decode(Path(session['saved_path']).read_bytes())
        self.assertEqual(saved['execution_traces'], session['execution_traces'])
        self.assertNotIn('fixture-private-analysis', json.dumps(session))
        last = self.requests[-1]
        self.assertEqual(len([message for message in last['messages'] if message['role'] == 'tool']), 2)

    def test_parallel_agents_use_distinct_worker_threads_and_selected_configurations(self):
        self.limit(2)
        self.prepare_environment()
        self.completed('parallel agents')
        self.assertEqual(self.maximum_children, 2)
        self.assertEqual(len({turn['thread'] for turn in self.child_turns}), 2)
        self.assertTrue(all(turn['thread'].startswith('studio-agent') for turn in self.child_turns))
        self.assertEqual({turn['task']: turn['model'] for turn in self.child_turns}, {'parallel-child-left': 'test-config-alt', 'parallel-child-right': 'test-config'})
        self.assertTrue(all('run_agent' not in {tool['function']['name'] for tool in turn['tools']} for turn in self.child_turns))
        children = self.children()
        self.assertEqual(len(children), 2)
        self.assertTrue(all(child['agent_status'] == 'completed' for child in children))
        self.assertTrue(all(child['execution_traces'][0]['steps'][0]['result']['exit_code'] == 0 for child in children))

    def test_concurrency_limit_one_serializes_agents_and_unavailable_model_falls_back(self):
        self.limit(1)
        self.prepare_environment()
        self.completed('serial agents')
        self.assertEqual(self.maximum_children, 1)
        self.assertEqual({turn['task']: turn['model'] for turn in self.child_turns}, {'serial-child-left': 'test-config-alt', 'serial-child-right': 'test-config'})
        jobs = [job for job in self.client.get('/api/jobs').json() if job['kind'] == 'agent']
        self.assertEqual(len(jobs), 2)
        self.assertTrue(all(job['hidden'] and job['status'] == 'completed' for job in jobs))

    def test_hidden_agent_files_are_read_only_and_portable_import_remaps_child_links(self):
        self.limit(2)
        self.prepare_environment()
        self.completed('parallel agents')
        children = self.children()
        self.assertEqual(len(children), 2)
        visible_ids = {session['id'] for session in self.client.get('/api/sessions').json()}
        self.assertTrue(all(child['id'] not in visible_ids for child in children))
        for child in children:
            self.assertTrue(child['is_agent'] and child['read_only'] and child['hidden'])
            expected = Path(self.project['path']) / '.agent/runs' / child['id'] / 'session.lattice'
            self.assertEqual(Path(child['saved_path']), expected)
            saved = lattice.decode(expected.read_bytes())
            self.assertEqual(saved['agent_task'], child['agent_task'])
            self.assertEqual(saved['messages'], [{key: message[key] for key in ('role', 'author', 'time', 'text', 'items', 'plan_steps', 'job_id') if key in message} | {'items': message.get('items', []), 'plan_steps': message.get('plan_steps', [])} for message in child['messages']])
            for body in ({'title': 'Edited title'}, {'model': 'test-config'}, {'draft': 'An edit'}):
                self.assertIn(self.client.patch('/api/sessions/' + child['id'], json=body).status_code, (403, 409))
            self.assertIn(self.client.post('/api/sessions/' + child['id'] + '/messages', json={'text': 'Edit agent', 'model': 'test-config'}).status_code, (403, 409))

        exported = self.client.get('/api/sessions/' + self.session['id'] + '/download').content
        encoded = lattice.decode(exported)
        self.assertEqual({child['id'] for child in encoded['child_sessions']}, {child['id'] for child in children})
        (Path(self.project['path']) / 'files' / 'portable.lattice').write_bytes(exported)
        imported = self.client.post('/api/projects/' + self.project['id'] + '/sessions/open', json={'path': 'files/portable.lattice'})
        self.assertEqual(imported.status_code, 200, imported.text)
        parent = imported.json()
        self.assertNotEqual(parent['id'], self.session['id'])
        imported_children = parent['child_sessions']
        self.assertEqual(len(imported_children), 2)
        original_ids = {child['id'] for child in children}
        remapped_ids = {child['id'] for child in imported_children}
        self.assertTrue(remapped_ids.isdisjoint(original_ids))
        self.assertEqual({step['agent_session_id'] for step in parent['execution_traces'][0]['steps']}, remapped_ids)
        for child_id in remapped_ids:
            child = self.client.get('/api/sessions/' + child_id).json()
            self.assertEqual(child['parent_session_id'], parent['id'])
            self.assertTrue(child['read_only'] and child['hidden'])
            self.assertTrue(Path(child['saved_path']).is_file())

    def test_disabled_tools_are_not_executed_even_when_requested_by_the_provider(self):
        resources = self.client.get('/api/projects/' + self.project['id'] + '/resources').json()
        python = next(resource for resource in resources if resource['name'] == 'run_python_script')
        response = self.client.patch('/api/projects/' + self.project['id'] + '/resources', json={'id': python['id'], 'enabled': False})
        self.assertEqual(response.status_code, 200, response.text)
        self.completed('disabled script')
        self.assertFalse((Path(self.project['path']) / 'executed.txt').exists())
        self.assertNotIn('run_python_script', {tool['function']['name'] for tool in self.requests[0]['tools']})
        step = self.current()['execution_traces'][0]['steps'][0]
        self.assertEqual(step['status'], 'failed')
        self.assertIn('disabled', step['result']['error'])

    def test_cancelling_parent_stops_agent_subprocess_trees_and_persists_cancelled_trace(self):
        self.limit(2)
        self.prepare_environment()
        job = self.start('cancel agents')
        files = [Path(self.project['path']) / (task + '.json') for task in ('cancel-child-left', 'cancel-child-right')]
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not all(path.is_file() for path in files):
            time.sleep(.02)
        self.assertTrue(all(path.is_file() for path in files), 'Both child subprocesses must start before cancellation')
        processes = [pid for path in files for pid in json.loads(path.read_text()).values()]
        self.assertTrue(all(process_is_running(pid) for pid in processes))
        response = self.client.post('/api/jobs/' + job['id'] + '/discard')
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json()['status'], 'discarded')
        self.assertFalse(any(process_is_running(pid) for pid in processes))
        children = self.children()
        self.assertEqual(len(children), 2)
        self.assertTrue(all(child['pending_job'] is None and child['agent_status'] == 'cancelled' for child in children))
        jobs = [child for child in self.client.get('/api/jobs').json() if child.get('parent_job_id') == job['id']]
        self.assertTrue(all(child['status'] == 'discarded' for child in jobs))
        self.assertEqual(self.app.state.pipeline.workers.running, 0)
        self.assertEqual(self.app.state.pipeline.workers.handles, {})
        session = self.current()
        self.assertEqual(session['draft'], 'cancel agents')
        self.assertEqual(session['messages'], [])
        trace = session['execution_traces'][0]
        self.assertEqual(trace['status'], 'cancelled')
        self.assertTrue(all(step['status'] == 'cancelled' for step in trace['steps']))
        saved = lattice.decode(Path(session['saved_path']).read_bytes())
        self.assertEqual(saved['execution_traces'], session['execution_traces'])
        self.assertTrue(all(child['agent_status'] == 'cancelled' for child in saved['child_sessions']))


    def test_disabled_existing_agent_is_not_run_or_injected_as_available(self):
        definition = Path(self.project['path']) / '.agent' / 'blocked'
        definition.mkdir(parents=True)
        (definition / 'AGENT.md').write_text('Do the requested task.', encoding='utf-8')
        response = self.client.patch('/api/projects/' + self.project['id'] + '/resources',
                                     json={'id': '.agent/blocked', 'enabled': False})
        self.assertEqual(response.status_code, 200, response.text)
        self.completed('disabled agent')
        session = self.client.get('/api/sessions/' + self.session['id']).json()
        step = session['execution_traces'][-1]['steps'][0]
        self.assertEqual(step['status'], 'failed')
        self.assertIn('disabled', step['result']['error'])
        self.assertNotIn('agent_session_id', step)
        self.assertFalse(any(job['kind'] == 'agent' for job in self.app.state.store.list('job')))
        self.assertNotIn('blocked', ''.join(message['content'] for message in self.requests[0]['messages'] if message['role'] == 'system'))

    def test_restart_marks_agent_steps_terminal_and_retains_pending_task_on_disk(self):
        pipeline, files, store = self.app.state.pipeline, self.app.state.session_files, self.app.state.store
        parent_job = pipeline.new(self.session, 'chat', 'interrupted delegation')
        child = files.new('Interrupted child', self.project['id'], persist=False)
        child = files.update(child['id'], lambda current: current.update(
            is_agent=True, hidden=True, read_only=True, parent_session_id=self.session['id'],
            agent_name='Interrupted child', agent_status='running', model='test-config'))
        child_job = pipeline.new(child, 'agent', 'Keep this task')
        pipeline.update(child_job['id'], parent_job_id=parent_job['id'])
        files.update(self.session['id'], lambda current: current.update(pending_job=parent_job['id']))
        files.update(child['id'], lambda current: current.update(pending_job=child_job['id'], messages=[
            {'role': 'user', 'author': 'Task', 'time': '2026-10-04T12:00:00+00:00', 'text': 'Keep this task', 'job_id': child_job['id']}]))
        pipeline.trace(parent_job, step={'id': 'delegation', 'tool_name': 'run_agent', 'arguments': {},
                                        'status': 'running', 'agent_session_id': child['id']})
        pipeline.trace(child_job, step={'id': 'script', 'tool_name': 'run_python_script', 'arguments': {}, 'status': 'running'})
        pipeline.persist_child(self.session['id'], child['id'])
        before = lattice.decode(Path(store.get('session', child['id'])['saved_path']).read_bytes())
        self.assertEqual(before['messages'][0]['text'], 'Keep this task')
        pipeline.recover()
        parent = store.get('session', self.session['id'])
        child = store.get('session', child['id'])
        self.assertEqual(parent['draft'], 'interrupted delegation')
        self.assertIsNone(child['pending_job'])
        self.assertEqual(child['agent_status'], 'failed')
        for current in (parent, child):
            decoded = lattice.decode(Path(current['saved_path']).read_bytes())
            self.assertEqual(decoded['execution_traces'][0]['status'], 'failed')
            self.assertEqual(decoded['execution_traces'][0]['steps'][0]['status'], 'failed')
        self.assertEqual(lattice.decode(Path(child['saved_path']).read_bytes())['messages'][0]['text'], 'Keep this task')


    def test_stopping_children_individually_preserves_parent_and_other_real_processes(self):
        self.limit(2)
        self.prepare_environment()
        job = self.start('cancel agents')
        paths = [Path(self.project['path']) / (task + '.json') for task in ('cancel-child-left', 'cancel-child-right')]
        deadline = time.monotonic() + 10
        while not all(path.exists() for path in paths) and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertTrue(all(path.exists() for path in paths))
        pids = [json.loads(path.read_text()) for path in paths]
        children = {child['agent_name']: child for child in self.children()}
        left, right = children['left'], children['right']
        result = self.client.post('/api/jobs/' + left['pending_job'] + '/discard')
        self.assertEqual(result.status_code, 200, result.text)
        self.assertEqual(self.client.get('/api/sessions/' + left['id']).json()['agent_status'], 'cancelled')
        self.assertFalse(any(process_is_running(pid) for pid in pids[0].values()))
        self.assertTrue(all(process_is_running(pid) for pid in pids[1].values()))
        self.assertEqual(self.client.get('/api/jobs/' + job['id']).json()['status'], 'in_progress')
        self.assertEqual(self.client.post('/api/jobs/' + right['pending_job'] + '/discard').status_code, 200)
        self.assertEqual(self.wait(job['id'])['status'], 'completed')
        self.assertFalse(any(process_is_running(pid) for pid in pids[1].values()))
        self.assertTrue(all(step['status'] == 'cancelled' for step in self.current()['execution_traces'][0]['steps']))
        self.assertEqual(self.app.state.pipeline.workers.running, 0)


if __name__ == '__main__':
    unittest.main()
