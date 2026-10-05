"""Project tools, managed Python environments, and isolated cancellable execution."""
import asyncio
import ctypes
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from contextlib import contextmanager
from pathlib import Path
from uuid import UUID, uuid4
from fastapi import HTTPException
from .workspace import contained, valid_name, atomic_write
from .project_memory import _redact

MAX_SCRIPT_CHARS = 100000
MAX_INSTRUCTION_CHARS = 32768
MAX_OUTPUT_BYTES = 128 * 1024
MAX_TIMEOUT = 120
MAX_PROMPT_CHARS = 32768
MAX_SETTINGS_BYTES = 16384
SLUG = re.compile(r'[a-z0-9][a-z0-9_-]{0,63}')
MODEL = re.compile(r'[A-Za-z0-9][A-Za-z0-9._:/@+\-]{0,199}')

BUILTIN_SKILLS = {
    'run_python_script': ('Execute Python in the selected project environment',
        'Use run_python_script for Python calculations, file processing and project tasks. '
        'Provide explicit Python code. The selected project virtual environment is used. '
        'The working directory is an isolated temporary folder. STUDIO_PROJECT_DIR points '
        'to the project; STUDIO_ENVIRONMENT_PYTHON points to the selected interpreter. '
        'Use those variables when project files or package installation are needed. '
        'Inspect stdout, stderr and exit_code, and summarize the result briefly.'),
    'run_batch_script': ('Execute shell commands in the selected project environment',
        'Use run_batch_script for operating-system commands and installing Python dependencies. '
        'On Windows the script runs as a batch file; on other systems it runs with sh. '
        'python and pip resolve to the selected managed virtual environment. '
        'Prefer python -m pip install PACKAGE for dependency installation. '
        'The working directory is temporary. STUDIO_PROJECT_DIR identifies the project. '
        'Inspect the actual exit_code, stdout and stderr before reporting success.'),
    'create_tool_from_conversation': ('Save reusable skills, agents and MCP definitions',
        'Use create_tool_from_conversation when the user asks to turn the conversation '
        'into a reusable skill, agent or MCP definition. Supply clear instructions and '
        'a short source summary, without private reasoning or credentials. '
        'Creation produces a new project definition and never replaces an existing one. '
        'Agent definitions require Max effort and the enabled create_agent skill; '
        'an optional model chooses their configuration. '
        'MCP definitions are metadata; saving one does not execute its transport command.'),
    'create_agent': ('Create a reusable project agent from the conversation',
        'Use create_agent only at Max effort when the user enabled this skill. '
        'Supply a lowercase name, concrete instructions and an optional model. '
        'The definition is saved under the project .agent folder and never overwrites '
        'an existing definition. Use run_agent separately to delegate work.'),
    'run_agent': ('Run a named project agent in a separate conversation',
        'Use run_agent only at Max effort when the user enabled this skill. '
        'The agent runs in its own session and returns a brief result summary. '
        'Use the agent definition model when configured, otherwise the current configuration. '
        'Keep delegated tasks bounded and never include private reasoning in artifacts.'),
}


class _WindowsJob:
    """Keep descendants attached even if their original parent exits."""
    def __init__(self, process):
        self.handle = None
        if os.name != 'nt':
            return
        from ctypes import wintypes
        class Basic(ctypes.Structure):
            _fields_ = [('per_process', ctypes.c_int64), ('per_job', ctypes.c_int64),
                        ('flags', wintypes.DWORD), ('minimum', ctypes.c_size_t),
                        ('maximum', ctypes.c_size_t), ('active', wintypes.DWORD),
                        ('affinity', ctypes.c_size_t), ('priority', wintypes.DWORD),
                        ('scheduling', wintypes.DWORD)]
        class IO(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint64) for name in ('read_ops', 'write_ops', 'other_ops', 'read_bytes', 'write_bytes', 'other_bytes')]
        class Extended(ctypes.Structure):
            _fields_ = [('basic', Basic), ('io', IO), ('process_memory', ctypes.c_size_t),
                        ('job_memory', ctypes.c_size_t), ('peak_process', ctypes.c_size_t),
                        ('peak_job', ctypes.c_size_t)]
        api = ctypes.WinDLL('kernel32', use_last_error=True)
        api.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        api.CreateJobObjectW.restype = wintypes.HANDLE
        api.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        api.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        api.OpenProcess.restype = wintypes.HANDLE
        api.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        api.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = api.CreateJobObjectW(None, None)
        info = Extended()
        info.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE.
        process_handle = api.OpenProcess(0x0100 | 0x0001, False, process.pid)
        if handle and process_handle and api.SetInformationJobObject(handle, 9, ctypes.byref(info), ctypes.sizeof(info)) and api.AssignProcessToJobObject(handle, process_handle):
            self.handle = handle
        elif handle:
            api.CloseHandle(handle)
        if process_handle:
            api.CloseHandle(process_handle)
        self.api = api

    def terminate(self):
        if self.handle:
            self.api.TerminateJobObject(self.handle, 1)

    def close(self):
        if self.handle:
            self.api.CloseHandle(self.handle)
            self.handle = None


class ToolRuntime:
    def __init__(self, settings, workspace):
        self.settings, self.workspace = settings, workspace
        self._locks = {}
        self._locks_guard = threading.Lock()

    def _lock(self, key):
        with self._locks_guard:
            return self._locks.setdefault(key, threading.RLock())

    @contextmanager
    def _cancellable_lock(self, key, cancelled):
        lock = self._lock(key)
        while not lock.acquire(timeout=.05):
            if cancelled.is_set():
                raise RuntimeError('Tool execution was cancelled before it started')
        try:
            if cancelled.is_set():
                raise RuntimeError('Tool execution was cancelled before it started')
            yield
        finally:
            lock.release()

    def _slug(self, name):
        if not isinstance(name, str) or not SLUG.fullmatch(name):
            raise ValueError('Use a lowercase name containing letters, numbers, underscores or hyphens')
        valid_name(name)
        return name

    def _uuid(self, value):
        try:
            return str(UUID(value))
        except (ValueError, TypeError, AttributeError):
            raise ValueError('Invalid project identifier') from None

    def _safe(self, root, path):
        candidate = Path(path)
        try:
            # Reject directory aliases and file links, including Windows junctions.
            relative = candidate.relative_to(root)
            current = root
            for part in relative.parts:
                current /= part
                if current.is_symlink() or getattr(current, 'is_junction', lambda: False)():
                    raise HTTPException(403, 'Project tool paths cannot contain linked files or folders')
            return contained(root, candidate)
        except RuntimeError:
            raise ValueError('Project tool path contains a symlink loop') from None
        except ValueError:
            raise HTTPException(403, 'Path is outside the managed project tool folder') from None

    def _read_text(self, path, maximum=192 * 1024):
        if not path.is_file():
            raise HTTPException(404, 'Project tool definition not found')
        with path.open('rb') as file:
            data = file.read(maximum + 1)
        if len(data) > maximum:
            raise ValueError('Project tool definition exceeds its size limit')
        try:
            return data.decode('utf-8')
        except UnicodeError:
            raise ValueError('Project tool definition must use UTF-8 text') from None

    def _json(self, path):
        try:
            value = json.loads(self._read_text(path, MAX_SETTINGS_BYTES))
        except (ValueError, TypeError, RecursionError):
            raise ValueError('Project tool settings have an invalid format') from None
        if not isinstance(value, dict):
            raise ValueError('Project tool settings must be an object')
        return value

    def _project_settings(self, project_id):
        root = self.workspace.path(project_id)
        directory = self._safe(root, root / '.tools')
        path = self._safe(root, directory / 'settings.json')
        if not path.exists():
            return {'version': 1, 'selected_environment': 'default'}
        value = self._json(path)
        if set(value) - {'version', 'selected_environment'} or value.get('version', 1) != 1:
            raise ValueError('Project tool settings version or fields are unsupported')
        name = self._slug(value.get('selected_environment', 'default'))
        return {'version': 1, 'selected_environment': name}

    def _environment_root(self, project_id):
        self.workspace.path(project_id)
        project_id = self._uuid(project_id)
        data = self.settings.data_dir.resolve()
        return self._safe(data, data / 'environments' / project_id)

    def _environment(self, project_id, name):
        name = self._slug(name)
        base = self._environment_root(project_id)
        path = self._safe(base, base / name)
        bin_path = self._safe(path, path / ('Scripts' if os.name == 'nt' else 'bin'))
        # POSIX venv interpreters intentionally link to their base Python binary.
        python = bin_path / ('python.exe' if os.name == 'nt' else 'python')
        marker = self._safe(path, path / '.studio-environment.json')
        ready = False
        if marker.exists():
            value = self._json(marker)
            if value != {'version': 1, 'project_id': project_id, 'name': name}:
                raise ValueError('Managed environment metadata does not match this project')
            ready = python.is_file() and (path / 'pyvenv.cfg').is_file()
        return {'name': name, 'path': str(path), 'python': str(python), 'ready': ready}

    def environments(self, project_id):
        with self._lock(('project', project_id)):
            selected = self._project_settings(project_id)['selected_environment']
            base = self._environment_root(project_id)
            names = {'default', selected}
            if base.exists():
                for path in base.iterdir():
                    if not path.name.startswith('.'):
                        self._slug(path.name)
                        self._safe(base, path)
                        if not path.is_dir():
                            raise ValueError('Managed environment directory contains an incompatible file')
                        names.add(path.name)
            return {'project_id': project_id, 'selected': selected,
                    'environments': [self._environment(project_id, name) for name in sorted(names)]}

    def _process_env(self, environment, project_path, work):
        allowed = {'PATH', 'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'PATHEXT', 'TEMP', 'TMP', 'TMPDIR',
                   'HOME', 'USERPROFILE', 'HOMEDRIVE', 'HOMEPATH', 'LOCALAPPDATA', 'APPDATA',
                   'PROGRAMDATA', 'PROGRAMFILES', 'PROGRAMFILES(X86)', 'NUMBER_OF_PROCESSORS',
                   'PROCESSOR_ARCHITECTURE', 'LANG', 'LC_ALL', 'LC_CTYPE'}
        env = {key: value for key, value in os.environ.items() if key.upper() in allowed}
        env.update(PYTHONUTF8='1', PYTHONIOENCODING='utf-8', PYTHONNOUSERSITE='1',
                   PIP_DISABLE_PIP_VERSION_CHECK='1', PIP_NO_INPUT='1', PIP_CONFIG_FILE=os.devnull,
                   STUDIO_PROJECT_DIR=str(project_path), STUDIO_CALL_DIR=str(work))
        if environment:
            env.update(VIRTUAL_ENV=environment['path'], STUDIO_ENVIRONMENT_NAME=environment['name'],
                       STUDIO_ENVIRONMENT_PYTHON=environment['python'])
            env['PATH'] = str(Path(environment['python']).parent) + os.pathsep + env.get('PATH', '')
        return env

    def _kill_tree(self, process, job):
        if os.name == 'nt':
            if job.handle:
                job.terminate()
            else:
                try:
                    subprocess.run(['taskkill.exe', '/PID', str(process.pid), '/T', '/F'],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                        creationflags=subprocess.CREATE_NO_WINDOW, timeout=5, check=False)
                except (OSError, subprocess.TimeoutExpired):
                    pass
        else:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        if process.poll() is None:
            process.kill()

    def _command(self, command, cwd, env, timeout, cancelled):
        if cancelled.is_set():
            raise RuntimeError('Tool execution was cancelled before it started')
        options = {'cwd': str(cwd), 'env': env, 'stdin': subprocess.DEVNULL,
                   'stdout': subprocess.PIPE, 'stderr': subprocess.PIPE, 'bufsize': 0}
        if os.name == 'nt':
            options['creationflags'] = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            options['start_new_session'] = True
        process = subprocess.Popen(command, **options)
        job = _WindowsJob(process)
        buffers = [bytearray(), bytearray()]
        truncated = [False, False]
        def read(stream, index):
            try:
                while True:
                    block = stream.read(8192)
                    if not block:
                        break
                    available = MAX_OUTPUT_BYTES - len(buffers[index])
                    buffers[index].extend(block[:max(0, available)])
                    if len(block) > available:
                        truncated[index] = True
            finally:
                stream.close()
        readers = [threading.Thread(target=read, args=(stream, index), daemon=True)
                   for index, stream in enumerate((process.stdout, process.stderr))]
        for reader in readers:
            reader.start()
        deadline, timed_out = time.monotonic() + timeout, False
        try:
            while process.poll() is None or any(reader.is_alive() for reader in readers):
                if cancelled.is_set() or time.monotonic() >= deadline:
                    timed_out = not cancelled.is_set()
                    self._kill_tree(process, job)
                    break
                cancelled.wait(.05)
            # A successfully finished script must not leave background children running.
            self._kill_tree(process, job)
            process.wait(timeout=5)
            for reader in readers:
                reader.join(timeout=5)
            if any(reader.is_alive() for reader in readers):
                raise RuntimeError('Tool output stream could not be closed')
            return {'ok': process.returncode == 0 and not timed_out and not cancelled.is_set(),
                    'exit_code': process.returncode, 'stdout': _redact(buffers[0].decode('utf-8', errors='replace')),
                    'stderr': _redact(buffers[1].decode('utf-8', errors='replace')),
                    'truncated': any(truncated), 'timed_out': timed_out}
        finally:
            if process.poll() is None:
                self._kill_tree(process, job)
            job.close()

    def create_environment(self, project_id, name, cancelled=None):
        name = self._slug(name)
        cancelled = cancelled or threading.Event()
        with self._cancellable_lock(('environment', project_id, name), cancelled):
            environment = self._environment(project_id, name)
            if environment['ready']:
                return environment
            target = Path(environment['path'])
            base = self._environment_root(project_id)
            base.mkdir(parents=True, exist_ok=True)
            target = self._safe(base, target)
            if target.exists():
                raise HTTPException(409, 'An incomplete or unrelated environment already exists with this name')
            target.mkdir()
            try:
                with tempfile.TemporaryDirectory(prefix='studio-environment-') as temporary:
                    work = Path(temporary)
                    result = self._command([sys.executable, '-m', 'venv', str(target)], work,
                        self._process_env(None, self.workspace.path(project_id), work), MAX_TIMEOUT, cancelled)
                if not result['ok']:
                    raise RuntimeError('Managed environment creation failed or was cancelled: ' + result['stderr'][:1000])
                marker = self._safe(target, target / '.studio-environment.json')
                atomic_write(marker, json.dumps({'version': 1, 'project_id': project_id, 'name': name}).encode())
                return self._environment(project_id, name)
            except Exception:
                # Only remove the brand-new directory created by this call, after rechecking it.
                resolved = self._safe(base, target)
                if resolved.parent != base or resolved.name != name:
                    raise ValueError('Invalid environment cleanup target')
                shutil.rmtree(resolved)
                raise

    def selected_environment(self, project_id, name=None, create=True, cancelled=None):
        selected = name or self._project_settings(project_id)['selected_environment']
        environment = self._environment(project_id, selected)
        if not environment['ready'] and create:
            return self.create_environment(project_id, selected, cancelled)
        if not environment['ready'] and not create:
            raise HTTPException(404, 'Selected managed environment is not ready')
        return environment

    def select_environment(self, project_id, name):
        name = self._slug(name)
        with self._lock(('project', project_id)):
            environment = self._environment(project_id, name)
            if not environment['ready'] and (name != 'default' or Path(environment['path']).exists()):
                raise HTTPException(404, 'Selected managed environment is not ready')
            self._project_settings(project_id)
            root = self.workspace.path(project_id)
            directory = self._safe(root, root / '.tools')
            directory.mkdir(exist_ok=True)
            path = self._safe(root, directory / 'settings.json')
            atomic_write(path, json.dumps({'version': 1, 'selected_environment': name}).encode())
            return self.environments(project_id)

    def bootstrap(self, project_id):
        project = self.workspace.store.get('project', project_id)
        if not project['managed']:
            return []
        created = []
        with self._lock(('project', project_id)):
            root = self.workspace.path(project_id)
            base = self._safe(root, root / '.skill')
            base.mkdir(exist_ok=True)
            for name, (description, instructions) in BUILTIN_SKILLS.items():
                path = self._safe(root, base / name)
                if path.exists():
                    continue
                path.mkdir()
                manifest = f'---\nname: {name}\ndescription: {json.dumps(description)}\n---\n\n{instructions}\n'
                with (path / 'SKILL.md').open('x', encoding='utf-8') as file:
                    file.write(manifest)
                created.append(name)
        return created

    def prompt(self, project_id, enabled_names=None):
        enabled = set(enabled_names) if enabled_names is not None else None
        root = self.workspace.path(project_id)
        parts, remaining = [], MAX_PROMPT_CHARS
        for resource in self.workspace.resources(project_id):
            if resource['kind'] != 'skill' or (not resource['enabled'] if enabled is None else resource['name'] not in enabled and resource['id'] not in enabled):
                continue
            directory = self._safe(root, root / resource['id'])
            path = self._safe(root, directory / 'SKILL.md')
            instructions = _redact(self._read_text(path))
            text = f"## Project skill: {resource['name']}\n{instructions}\n"
            parts.append(text[:remaining])
            remaining -= min(len(text), remaining)
            if not remaining:
                break
        return '\n'.join(parts)[:MAX_PROMPT_CHARS]

    def agent(self, project_id, name):
        root = self.workspace.path(project_id)
        matches = [resource for resource in self.workspace.resources(project_id)
                   if resource['kind'] == 'agent' and name in (resource['name'], resource['id'])]
        if len(matches) != 1:
            raise HTTPException(404 if not matches else 409, 'Choose one existing project agent')
        resource = matches[0]
        directory = self._safe(root, root / resource['id'])
        instructions = _redact(self._read_text(self._safe(root, directory / 'AGENT.md')))
        config_path = self._safe(root, directory / 'agent.json')
        model = None
        if config_path.exists():
            config = self._json(config_path)
            if set(config) - {'version', 'name', 'model'} or config.get('version', 1) != 1 or config.get('name', resource['name']) != resource['name']:
                raise ValueError('Project agent configuration is incompatible')
            model = config.get('model') or None
            if model is not None and (not isinstance(model, str) or not MODEL.fullmatch(model)):
                raise ValueError('Project agent has an invalid model identifier')
        return {'name': resource['name'], 'instructions': instructions, 'model': model}

    def _create_artifact(self, arguments, context):
        allowed = {'kind', 'name', 'instructions', 'description', 'model', 'mcp_config', 'source'}
        if set(arguments) - allowed:
            raise ValueError('Tool definition contains unsupported fields')
        kind, name = arguments.get('kind'), self._slug(arguments.get('name'))
        if kind not in ('skill', 'agent', 'mcp'):
            raise ValueError('Choose skill, agent or mcp')
        if kind == 'agent':
            if context.get('effort', 'low') != 'max':
                raise ValueError('Agent creation requires Max effort')
            enabled = context.get('multi_agent_enabled') is True or any(
                resource['kind'] == 'skill' and resource['name'] == 'create_agent' and resource['enabled']
                for resource in self.workspace.resources(context['project_id'], effort='max'))
            if not enabled:
                raise ValueError('Enable the create_agent skill at Max effort before creating agents')
        instructions = arguments.get('instructions')
        source = arguments.get('source', '')
        description = arguments.get('description', 'Reusable project ' + kind)
        if not isinstance(instructions, str) or not instructions.strip() or len(instructions) > MAX_INSTRUCTION_CHARS or not isinstance(source, str) or len(source) > 8000 or not isinstance(description, str) or len(description) > 300:
            raise ValueError('Provide bounded instructions, description and a brief source summary')
        model = arguments.get('model') or None
        if model is not None and (not isinstance(model, str) or not MODEL.fullmatch(model) or _redact(model) != model):
            raise ValueError('Choose a valid agent configuration identifier')
        text = _redact(instructions) + ('\n\n## Source conversation summary\n' + _redact(source) if source else '') + '\n'
        if kind == 'skill':
            files = {'SKILL.md': f'---\nname: {name}\ndescription: {json.dumps(_redact(description))}\n---\n\n' + text}
        elif kind == 'agent':
            config = {'version': 1, 'name': name, **({'model': model} if model else {})}
            files = {'AGENT.md': '# ' + name + '\n\n' + text, 'agent.json': json.dumps(config, indent=2) + '\n'}
        else:
            config = arguments.get('mcp_config', {})
            if not isinstance(config, dict) or len(json.dumps(config).encode()) > 65536:
                raise ValueError('Provide a bounded MCP configuration object')
            def cleanse(value):
                if isinstance(value, dict):
                    cleaned = {}
                    for key, item in value.items():
                        sensitive = re.search(r'(?i)(api[_-]?key|token|secret|password|authorization)', key)
                        reference = isinstance(item, str) and re.fullmatch(r'(?:Bearer )?(?:\$\{[A-Za-z_][A-Za-z0-9_]*\}|%[A-Za-z_][A-Za-z0-9_]*%)', item)
                        cleaned[key] = '[credential environment reference required]' if sensitive and item and not reference else cleanse(item)
                    return cleaned
                if isinstance(value, list):
                    return [cleanse(item) for item in value]
                return _redact(value) if isinstance(value, str) else value
            files = {'mcp.json': json.dumps(cleanse(config), indent=2, allow_nan=False) + '\n',
                     'README.md': '# ' + name + '\n\n' + text + '\nThis transport definition is not executed automatically.\n'}
        project_id = context['project_id']
        with self._lock(('project', project_id)):
            root = self.workspace.path(project_id)
            base = self._safe(root, root / ('.' + kind))
            base.mkdir(exist_ok=True)
            target = self._safe(root, base / name)
            if target.exists():
                raise HTTPException(409, 'A project definition with this name already exists')
            target.mkdir()
            try:
                for filename, content in files.items():
                    path = self._safe(root, target / filename)
                    with path.open('x', encoding='utf-8') as file:
                        file.write(content)
            except Exception:
                checked = self._safe(base, target)
                if checked.parent != base:
                    raise ValueError('Invalid project definition cleanup target')
                shutil.rmtree(checked)
                raise
            resource_id = target.relative_to(root).as_posix()
            if kind in ('skill', 'agent'):
                def enable(project):
                    project['preferences'].setdefault(resource_id, {})['enabled'] = True
                self.workspace.store.update('project', project_id, enable)
            return {'ok': True, 'kind': kind, 'name': name, 'path': resource_id}

    def _execute(self, name, arguments, context, cancelled):
        if name == 'create_agent':
            if 'kind' in arguments:
                raise ValueError('create_agent does not accept a kind field')
            return self._create_artifact({**arguments, 'kind': 'agent'}, context)
        if name == 'create_tool_from_conversation':
            return self._create_artifact(arguments, context)
        if name not in ('run_python_script', 'run_batch_script'):
            raise ValueError('Unknown tool; run_agent is handled by the conversation orchestrator')
        script_key = 'code' if name == 'run_python_script' else 'script'
        if set(arguments) - {script_key, 'timeout'}:
            raise ValueError('Script tool contains unsupported fields')
        code = arguments.get(script_key)
        timeout = arguments.get('timeout', 60)
        if not isinstance(code, str) or not code.strip() or len(code) > MAX_SCRIPT_CHARS or isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not 1 <= timeout <= MAX_TIMEOUT:
            raise ValueError('Provide a nonempty bounded script and a timeout from 1 to 120 seconds')
        project_id = context['project_id']
        project_path = self.workspace.path(project_id)
        environment = self.selected_environment(project_id, context.get('environment'), cancelled=cancelled)
        temporary_root = Path(tempfile.gettempdir()).resolve()
        job_root = Path(context['temp_dir']).resolve()
        if not job_root.is_relative_to(temporary_root) or not job_root.is_dir():
            raise ValueError('Tool execution requires an existing system temporary job folder')
        work_parent = self._safe(job_root, job_root / 'work')
        work_parent.mkdir(exist_ok=True)
        work = self._safe(job_root, work_parent / str(uuid4()))
        work.mkdir()
        if name == 'run_python_script':
            script = work / 'tool.py'
            script.write_text(code, encoding='utf-8')
            command = [environment['python'], '-u', str(script)]
        elif os.name == 'nt':
            script = work / 'tool.bat'
            script.write_text('@echo off\nchcp 65001 >nul\n' + code, encoding='utf-8', newline='\r\n')
            command = [os.environ.get('COMSPEC', str(Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32/cmd.exe')), '/d', '/c', 'tool.bat']
        else:
            script = work / 'tool.sh'
            script.write_text(code, encoding='utf-8')
            command = ['/bin/sh', str(script)]
        env = self._process_env(environment, project_path, work)
        if name == 'run_batch_script':
            with self._cancellable_lock(('batch', project_id, environment['name']), cancelled):
                result = self._command(command, work, env, timeout, cancelled)
        else:
            result = self._command(command, work, env, timeout, cancelled)
        return {**result, 'environment': environment['name']}

    async def execute(self, name, arguments, context):
        cancelled = threading.Event()
        async def invoke():
            try:
                if not isinstance(arguments, dict) or not isinstance(context, dict) or not context.get('project_id'):
                    raise ValueError('Project tools require structured arguments and an active project')
                return await asyncio.to_thread(self._execute, name, arguments, context, cancelled)
            except HTTPException as error:
                return {'ok': False, 'error': str(error.detail)}
            except (ValueError, RuntimeError) as error:
                return {'ok': False, 'error': _redact(str(error))}
            except OSError:
                return {'ok': False, 'error': 'Tool could not access the required file or executable. Check permissions and disk space.'}
            except Exception:
                return {'ok': False, 'error': 'Tool execution could not finish. Check its arguments and retry.'}
        worker = asyncio.create_task(invoke())
        try:
            return await asyncio.shield(worker)
        except asyncio.CancelledError:
            cancelled.set()
            # Repeated parent cancellation must not release its temporary job
            # folder while the subprocess worker is still terminating children.
            while not worker.done():
                try:
                    await asyncio.shield(worker)
                except asyncio.CancelledError:
                    continue
            raise

    def definitions(self, project_id=None, effort='low'):
        agent_creation = effort == 'max' and (project_id is None or any(
            resource['kind'] == 'skill' and resource['name'] == 'create_agent' and resource['enabled']
            for resource in self.workspace.resources(project_id, effort=effort)))
        schemas = {
            'run_python_script': ({'code': {'type': 'string', 'maxLength': MAX_SCRIPT_CHARS}, 'timeout': {'type': 'number', 'minimum': 1, 'maximum': MAX_TIMEOUT}}, ['code']),
            'run_batch_script': ({'script': {'type': 'string', 'maxLength': MAX_SCRIPT_CHARS}, 'timeout': {'type': 'number', 'minimum': 1, 'maximum': MAX_TIMEOUT}}, ['script']),
            'create_tool_from_conversation': ({'kind': {'type': 'string', 'enum': ['skill', 'agent', 'mcp'] if agent_creation else ['skill', 'mcp']}, 'name': {'type': 'string', 'pattern': '^' + SLUG.pattern + '$'}, 'instructions': {'type': 'string', 'maxLength': MAX_INSTRUCTION_CHARS}, 'description': {'type': 'string', 'maxLength': 300}, 'model': {'type': 'string', 'maxLength': 200}, 'mcp_config': {'type': 'object'}, 'source': {'type': 'string', 'maxLength': 8000}}, ['kind', 'name', 'instructions']),
            'create_agent': ({'name': {'type': 'string', 'pattern': '^' + SLUG.pattern + '$'}, 'instructions': {'type': 'string', 'maxLength': MAX_INSTRUCTION_CHARS}, 'description': {'type': 'string', 'maxLength': 300}, 'model': {'type': 'string', 'maxLength': 200}, 'source': {'type': 'string', 'maxLength': 8000}}, ['name', 'instructions']),
            'run_agent': ({'name': {'type': 'string', 'maxLength': 100}, 'task': {'type': 'string', 'maxLength': 100000}, 'model': {'type': 'string', 'maxLength': 200}}, ['name', 'task']),
        }
        return [{'type': 'function', 'function': {'name': name, 'description': description,
                 'parameters': {'type': 'object', 'properties': schemas[name][0], 'required': schemas[name][1], 'additionalProperties': False}}}
                for name, (description, _) in BUILTIN_SKILLS.items() if effort == 'max' or name not in ('create_agent', 'run_agent')]
