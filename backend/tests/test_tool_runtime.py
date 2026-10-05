"""Real interpreter, shell, cancellation, and contained-definition regressions."""
import asyncio
import ctypes
import json
import os
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path
from uuid import uuid4
from unittest.mock import patch
from fastapi import HTTPException
from backend.app.config import Settings
from backend.app.repository import Store
from backend.app.services.workspace import Workspace
from backend.app.services.tool_runtime import ToolRuntime, MAX_OUTPUT_BYTES
from backend.app.utils.temp_manager import TempManager


class ToolRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='studio-runtime-test-')
        cls.root = Path(cls.directory.name).resolve()
        cls.settings = Settings(data_dir=cls.root / 'data', workspace_root=cls.root / 'projects', browser_roots=[cls.root])
        cls.settings.workspace_root.mkdir()
        cls.store = Store(cls.settings.data_dir / 'studio.db')
        cls.store.initialize()
        cls.workspace = Workspace(cls.settings, cls.store)
        cls.project = cls.workspace.create('Runtime test')
        cls.runtime = ToolRuntime(cls.settings, cls.workspace)
        cls.environment = cls.runtime.create_environment(cls.project['id'], 'default')
        cls.temp = TempManager('runtime-test-' + uuid4().hex)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()
        cls.temp.root.rmdir()

    def setUp(self):
        self.job_directory = self.temp.create()
        self.context = {'project_id': self.project['id'], 'job_id': str(uuid4()),
                        'session_id': str(uuid4()), 'temp_dir': str(self.job_directory)}

    def tearDown(self):
        self.temp.purge(self.job_directory)

    def execute(self, name, arguments):
        return asyncio.run(self.runtime.execute(name, arguments, self.context))

    def test_python_uses_managed_interpreter_temp_cwd_and_stripped_credentials(self):
        code = 'import os,sys,json; print(json.dumps({"prefix":sys.prefix,"cwd":os.getcwd(),"project":os.environ["STUDIO_PROJECT_DIR"],"secret":os.environ.get("CONNECTOR_API_KEY"),"other":os.environ.get("ARBITRARY_SECRET")}))'
        with patch.dict(os.environ, {'CONNECTOR_API_KEY': 'runtime-fixture-secret', 'ARBITRARY_SECRET': 'another-runtime-fixture'}):
            result = self.execute('run_python_script', {'code': code})
        self.assertTrue(result['ok'], result)
        values = json.loads(result['stdout'])
        self.assertEqual(Path(values['prefix']).resolve(), Path(self.environment['path']))
        self.assertTrue(Path(values['cwd']).is_relative_to(self.job_directory / 'work'))
        self.assertEqual(values['project'], self.project['path'])
        self.assertIsNone(values['secret'])
        self.assertIsNone(values['other'])
        self.assertEqual(result['environment'], 'default')

    def test_batch_python_and_pip_share_the_selected_environment(self):
        script = 'python -c "import sys; print(sys.prefix)"\npython -m pip --version'
        result = self.execute('run_batch_script', {'script': script})
        self.assertTrue(result['ok'], result)
        self.assertIn(self.environment['path'], result['stdout'])
        self.assertIn('pip ', result['stdout'])
        self.assertNotEqual(result['exit_code'], None)

    def test_bounded_output_nonzero_exit_and_timeout(self):
        output = self.execute('run_python_script', {'code': 'print("x"*300000)'})
        self.assertTrue(output['ok'])
        self.assertTrue(output['truncated'])
        self.assertLessEqual(len(output['stdout'].encode()), MAX_OUTPUT_BYTES)
        failure = self.execute('run_python_script', {'code': 'import sys; print("execution failed",file=sys.stderr); sys.exit(7)'})
        self.assertFalse(failure['ok'])
        self.assertEqual(failure['exit_code'], 7)
        self.assertIn('execution failed', failure['stderr'])
        started = time.monotonic()
        timeout = self.execute('run_python_script', {'code': 'import time; time.sleep(20)', 'timeout': 1})
        self.assertTrue(timeout['timed_out'], timeout)
        self.assertFalse(timeout['ok'])
        self.assertLess(time.monotonic() - started, 6)

    def test_async_cancellation_terminates_python_and_its_child(self):
        ready = Path(self.project['path']) / 'child-ready.txt'
        child_code = f'import os,time; from pathlib import Path; Path({str(ready)!r}).write_text(str(os.getpid())); time.sleep(30)'
        code = f'import subprocess,sys,time; subprocess.Popen([sys.executable,"-c",{child_code!r}]); time.sleep(30)'
        async def run():
            task = asyncio.create_task(self.runtime.execute('run_python_script', {'code': code, 'timeout': 60}, self.context))
            try:
                deadline = time.monotonic() + 10
                while not ready.exists() and time.monotonic() < deadline:
                    await asyncio.sleep(.02)
                self.assertTrue(ready.exists(), 'Child process did not signal readiness')
                pid = int(ready.read_text())
                task.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await task
                if os.name == 'nt':
                    from ctypes import wintypes
                    api = ctypes.WinDLL('kernel32', use_last_error=True)
                    api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
                    api.OpenProcess.restype = wintypes.HANDLE
                    api.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
                    api.CloseHandle.argtypes = [wintypes.HANDLE]
                    handle = api.OpenProcess(0x1000, False, pid)
                    if handle:
                        status = wintypes.DWORD()
                        api.GetExitCodeProcess(handle, ctypes.byref(status))
                        api.CloseHandle(handle)
                        self.assertNotEqual(status.value, 259, 'Tool child survived cancellation')
                else:
                    status = subprocess.run(['ps', '-p', str(pid), '-o', 'stat='], capture_output=True, text=True, check=False).stdout.strip()
                    self.assertTrue(not status or status.startswith('Z'), 'Tool child survived cancellation')
            finally:
                if not task.done():
                    task.cancel()
                    await asyncio.gather(task, return_exceptions=True)
                ready.unlink(missing_ok=True)
        asyncio.run(run())

    def test_repeated_cancellation_waits_for_real_process_termination(self):
        ready = self.job_directory / 'process-ready.txt'
        code = f'import os,time; from pathlib import Path; Path({str(ready)!r}).write_text(str(os.getpid())); time.sleep(30)'
        entered, release = threading.Event(), threading.Event()
        processes = []
        original = self.runtime._kill_tree

        def terminate(process, job):
            if not entered.is_set():
                processes.append(process)
                entered.set()
                if not release.wait(5):
                    raise RuntimeError('Test did not release subprocess termination')
            return original(process, job)

        async def run():
            task = asyncio.create_task(self.runtime.execute('run_python_script', {'code': code, 'timeout': 60}, self.context))
            try:
                deadline = time.monotonic() + 10
                while not ready.exists() and time.monotonic() < deadline:
                    await asyncio.sleep(.02)
                self.assertTrue(ready.exists(), 'Real process did not signal readiness')
                task.cancel()
                self.assertTrue(await asyncio.to_thread(entered.wait, 3), 'Cancellation did not start process termination')
                self.assertIsNone(processes[0].poll(), 'Process exited before the gated cleanup')
                task.cancel()
                # Run a queued callback after the second cancel is delivered.
                delivered = asyncio.get_running_loop().create_future()
                asyncio.get_running_loop().call_soon(delivered.set_result, None)
                await delivered
                self.assertFalse(task.done(), 'Repeated cancellation escaped while the subprocess was still running')
                self.assertIsNone(processes[0].poll())
                release.set()
                with self.assertRaises(asyncio.CancelledError):
                    await asyncio.wait_for(task, 5)
                self.assertIsNotNone(processes[0].poll(), 'Cancellation returned before subprocess termination')
            finally:
                release.set()
                if not task.done():
                    task.cancel()
                await asyncio.gather(task, return_exceptions=True)

        with patch.object(self.runtime, '_kill_tree', side_effect=terminate):
            asyncio.run(run())

    def test_new_project_bootstrap_and_created_skill_instructions_are_real(self):
        created = self.runtime.bootstrap(self.project['id'])
        self.assertEqual(set(created), {'run_python_script', 'run_batch_script', 'create_tool_from_conversation', 'create_agent', 'run_agent'})
        self.assertEqual(self.runtime.bootstrap(self.project['id']), [])
        result = self.execute('create_tool_from_conversation', {'kind': 'skill', 'name': 'created-skill', 'instructions': 'Always print the inspected input row count.', 'source': 'User requested a reusable data check.'})
        self.assertTrue(result['ok'], result)
        manifest = Path(self.project['path']) / result['path'] / 'SKILL.md'
        self.assertIn('inspected input row count', manifest.read_text(encoding='utf-8'))
        self.assertIn('inspected input row count', self.runtime.prompt(self.project['id']))
        before = manifest.read_bytes()
        duplicate = self.execute('create_tool_from_conversation', {'kind': 'skill', 'name': 'created-skill', 'instructions': 'Replace the existing instructions.'})
        self.assertFalse(duplicate['ok'])
        self.assertEqual(manifest.read_bytes(), before)

    def test_agent_and_mcp_artifacts_are_contained_definitions(self):
        self.context.update(effort='max', multi_agent_enabled=True)
        result = self.execute('create_tool_from_conversation', {'kind': 'agent', 'name': 'created-agent', 'instructions': 'Inspect CSV input and summarize its columns.', 'model': 'test-config-alt'})
        self.assertTrue(result['ok'], result)
        agent = self.runtime.agent(self.project['id'], 'created-agent')
        self.assertEqual(agent['model'], 'test-config-alt')
        self.assertTrue(next(resource for resource in self.workspace.resources(self.project['id'], effort='max') if resource['name'] == 'created-agent')['enabled'])
        locked_agent = next(resource for resource in self.workspace.resources(self.project['id']) if resource['name'] == 'created-agent')
        self.assertTrue(locked_agent['locked'])
        self.assertFalse(locked_agent['enabled'])
        self.assertIn('CSV input', agent['instructions'])
        result = self.execute('create_tool_from_conversation', {'kind': 'mcp', 'name': 'created-mcp', 'instructions': 'Provide a metadata-only transport definition.', 'mcp_config': {'command': 'python', 'env': {'MCP_API_KEY': 'fixture-credential', 'TOKEN_REF': '${TOKEN_REF}'}}})
        self.assertTrue(result['ok'], result)
        config = json.loads((Path(self.project['path']) / result['path'] / 'mcp.json').read_text())
        self.assertEqual(config['command'], 'python')
        self.assertFalse(next(resource for resource in self.workspace.resources(self.project['id']) if resource['name'] == 'created-mcp')['enabled'])
        self.assertEqual(config['env']['MCP_API_KEY'], '[credential environment reference required]')
        self.assertEqual(config['env']['TOKEN_REF'], '${TOKEN_REF}')
        invalid = self.execute('create_tool_from_conversation', {'kind': 'skill', 'name': '../escape', 'instructions': 'Invalid target'})
        self.assertFalse(invalid['ok'])
        self.assertFalse((Path(self.project['path']).parent / 'escape').exists())

    def test_environment_settings_are_persistent_and_do_not_select_unmanaged_paths(self):
        selected = self.runtime.select_environment(self.project['id'], 'default')
        self.assertEqual(selected['selected'], 'default')
        restarted = ToolRuntime(self.settings, self.workspace)
        self.assertEqual(restarted.environments(self.project['id'])['selected'], 'default')
        self.assertTrue(restarted.selected_environment(self.project['id'], create=False)['ready'])
        with self.assertRaises(ValueError):
            self.runtime.create_environment(self.project['id'], '../outside')
        with self.assertRaises(HTTPException):
            self.runtime.select_environment(self.project['id'], 'missing')

    def test_lazy_default_can_be_selected_after_a_custom_environment(self):
        project = self.workspace.create('Lazy environment selection')
        custom = self.runtime.create_environment(project['id'], 'custom')
        self.assertTrue(custom['ready'])
        self.runtime.select_environment(project['id'], 'custom')
        default = self.runtime._environment(project['id'], 'default')
        self.assertFalse(Path(default['path']).exists())
        selected = self.runtime.select_environment(project['id'], 'default')
        self.assertEqual(selected['selected'], 'default')
        self.assertFalse(Path(default['path']).exists(), 'Selecting lazy default must not create it')
        self.assertFalse(next(item for item in selected['environments'] if item['name'] == 'default')['ready'])
        with self.assertRaises(HTTPException):
            self.runtime.select_environment(project['id'], 'unadvertised')

    def test_queued_batch_cancellation_does_not_launch_another_environment_writer(self):
        entered, release, second_entered = threading.Event(), threading.Event(), threading.Event()
        original = self.runtime._command
        def command(arguments, cwd, env, timeout, cancelled):
            script = Path(cwd) / ('tool.bat' if os.name == 'nt' else 'tool.sh')
            text = script.read_text(encoding='utf-8')
            if 'first-batch-marker' in text:
                entered.set()
                if not release.wait(5):
                    raise RuntimeError('Test did not release the first batch')
            if 'second-batch-marker' in text:
                second_entered.set()
            return original(arguments, cwd, env, timeout, cancelled)
        async def run():
            first = asyncio.create_task(self.runtime.execute('run_batch_script', {'script': 'echo first-batch-marker'}, self.context))
            second = None
            try:
                self.assertTrue(await asyncio.to_thread(entered.wait, 3))
                second = asyncio.create_task(self.runtime.execute('run_batch_script', {'script': 'echo second-batch-marker'}, self.context))
                deadline = time.monotonic() + 3
                prepared = False
                while time.monotonic() < deadline:
                    prepared = any('second-batch-marker' in path.read_text(encoding='utf-8') for path in (self.job_directory / 'work').glob('*/tool.*'))
                    if prepared:
                        break
                    await asyncio.sleep(.01)
                self.assertTrue(prepared, 'Second batch did not stage its script')
                started = time.monotonic()
                second.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await asyncio.wait_for(second, 2)
                self.assertLess(time.monotonic() - started, 2)
                self.assertFalse(second_entered.is_set(), 'Queued batch ran before the environment lock was released')
                release.set()
                result = await first
                self.assertTrue(result['ok'], result)
                self.assertIn('first-batch-marker', result['stdout'])
            finally:
                release.set()
                tasks = [task for task in (first, second) if task is not None]
                for task in tasks:
                    if not task.done():
                        task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
        with patch.object(self.runtime, '_command', side_effect=command):
            asyncio.run(run())

    def test_definition_escape_resolution_is_rejected_without_writing_outside(self):
        project_path = Path(self.project['path'])
        outside = self.root / 'outside-definition'
        original = Path.resolve
        def resolve(path, *args, **kwargs):
            if path == project_path / '.skill':
                return outside
            return original(path, *args, **kwargs)
        with patch.object(Path, 'resolve', resolve):
            result = self.execute('create_tool_from_conversation', {'kind': 'skill', 'name': 'blocked-link', 'instructions': 'This definition must stay contained.'})
        self.assertFalse(result['ok'], result)
        self.assertFalse(outside.exists())

    def test_malformed_tool_arguments_return_errors(self):
        self.assertFalse(self.execute('run_python_script', {'code': 'print(1)', 'timeout': 999})['ok'])
        self.assertFalse(self.execute('run_python_script', {'missing': 'code'})['ok'])
        self.assertFalse(self.execute('not-a-tool', {})['ok'])
        self.assertEqual(len(self.runtime.definitions()), 3)
        self.assertEqual(len(self.runtime.definitions(effort='max')), 5)


    def test_multi_agent_skills_are_opt_in_and_locked_below_max(self):
        project = self.workspace.create('Effort gating')
        self.runtime.bootstrap(project['id'])
        resources = {item['name']: item for item in self.workspace.resources(project['id'])}
        for name in ('run_agent', 'create_agent'):
            self.assertFalse(resources[name]['enabled'])
            self.assertTrue(resources[name]['locked'])
            self.assertIn('Max effort', resources[name]['lock_reason'])
        maximum = {item['name']: item for item in self.workspace.resources(project['id'], effort='max')}
        self.assertFalse(maximum['run_agent']['enabled'])
        self.assertFalse(maximum['create_agent']['locked'])
        def enable(document):
            for name in ('run_agent', 'create_agent'):
                document['preferences'].setdefault('.skill/' + name, {})['enabled'] = True
        self.store.update('project', project['id'], enable)
        maximum = {item['name']: item for item in self.workspace.resources(project['id'], effort='max')}
        self.assertTrue(maximum['run_agent']['enabled'])
        self.assertTrue(maximum['create_agent']['enabled'])
        low = {item['name']: item for item in self.workspace.resources(project['id'], effort='low')}
        self.assertFalse(low['run_agent']['enabled'])
        self.assertTrue(self.store.get('project', project['id'])['preferences']['.skill/run_agent']['enabled'])
        names = {item['function']['name'] for item in self.runtime.definitions(effort='high')}
        self.assertNotIn('run_agent', names)
        self.assertNotIn('create_agent', names)
        low_schema = next(item['function']['parameters'] for item in self.runtime.definitions() if item['function']['name'] == 'create_tool_from_conversation')
        self.assertEqual(low_schema['properties']['kind']['enum'], ['skill', 'mcp'])
        high_schema = next(item['function']['parameters'] for item in self.runtime.definitions(project['id'], effort='max') if item['function']['name'] == 'create_tool_from_conversation')
        self.assertIn('agent', high_schema['properties']['kind']['enum'])
        prompt = self.runtime.prompt(project['id'], enabled_names={'run_python_script'})
        self.assertNotIn('Project skill: create_agent', prompt)
        self.assertNotIn('Project skill: run_agent', prompt)

    def test_create_agent_alias_and_generic_creation_cannot_bypass_effort_or_opt_in(self):
        project = self.workspace.create('Agent alias gating')
        self.runtime.bootstrap(project['id'])
        context = {**self.context, 'project_id': project['id']}
        arguments = {'name': 'new-agent', 'instructions': 'Inspect the project and provide a public summary.'}
        async def invoke(name, args):
            return await self.runtime.execute(name, args, context)
        denied = asyncio.run(invoke('create_agent', arguments))
        self.assertFalse(denied['ok'])
        self.assertIn('Max effort', denied['error'])
        context['effort'] = 'max'
        denied = asyncio.run(invoke('create_tool_from_conversation', {**arguments, 'kind': 'agent'}))
        self.assertFalse(denied['ok'])
        self.assertIn('Enable the create_agent', denied['error'])
        def enable(document):
            document['preferences'].setdefault('.skill/create_agent', {})['enabled'] = True
        self.store.update('project', project['id'], enable)
        context['effort'] = 'medium'
        denied = asyncio.run(invoke('create_tool_from_conversation', {**arguments, 'kind': 'agent'}))
        self.assertFalse(denied['ok'])
        self.assertFalse((Path(project['path']) / '.agent').exists())
        context['effort'] = 'max'
        created = asyncio.run(invoke('create_agent', {**arguments, 'model': 'test-config-alt'}))
        self.assertTrue(created['ok'], created)
        self.assertEqual(created['kind'], 'agent')
        self.assertEqual(created['path'], '.agent/new-agent')
        self.assertEqual(self.runtime.agent(project['id'], 'new-agent')['model'], 'test-config-alt')
        denied = asyncio.run(invoke('create_agent', {**arguments, 'kind': 'agent'}))
        self.assertFalse(denied['ok'])
