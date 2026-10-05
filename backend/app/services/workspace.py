"""Work Projects and metadata-only browsing with optional filesystem restrictions."""
import os
import re
import tempfile
import threading
import stat
from datetime import datetime, timezone
from pathlib import Path
from uuid import uuid4
from fastapi import HTTPException
from ..repository import now

def valid_name(value):
    if (not value.strip() or value != value.strip() or value in ('.', '..') or re.search(r'[<>:"/\\|?*\x00-\x1f]', value) or value.endswith(('.', ' ')) or re.fullmatch(r'(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?', value)):
        raise ValueError('Use a simple file or project name without path separators')
    return value

def contained(root: Path, path: Path):
    resolved = path.resolve()
    if not resolved.is_relative_to(root.resolve()):
        raise HTTPException(403, 'Path is outside the configured workspace')
    return resolved

def atomic_write(path, data):
    descriptor, temporary = tempfile.mkstemp(prefix='.studio-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as file:
            file.write(data); file.flush(); os.fsync(file.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)

class Workspace:
    def __init__(self, settings, store):
        self.settings, self.store = settings, store
        self._recent_lock = threading.RLock()

    def _recent_available(self, project):
        try:
            return stat.S_ISDIR(Path(project['path']).stat().st_mode)
        except (FileNotFoundError, NotADirectoryError):
            return False
        except OSError:
            # Permission and transient drive errors are not evidence of deletion.
            return None

    def _recent_ids(self):
        try:
            record = self.store.get('settings', 'recent_projects')
            ids = record.get('project_ids', [])
            if not isinstance(ids, list):
                ids = []
            return list(dict.fromkeys(value for value in ids if isinstance(value, str)))[:5]
        except HTTPException as error:
            if error.status_code != 404:
                raise
        catalog = sorted(self.store.list('project'), key=lambda project: str(project.get('updated_at') or project.get('created_at') or ''), reverse=True)
        ids = [project['id'] for project in catalog if self._recent_available(project) is not False][:5]
        self.store.put('settings', {'id': 'recent_projects', 'version': 1, 'project_ids': ids})
        return ids

    def _remember_project(self, id):
        with self._recent_lock:
            ids = self._recent_ids()
            return self.store.put('settings', {'id': 'recent_projects', 'version': 1,
                                              'project_ids': [id, *(value for value in ids if value != id)][:5]})

    def recent_projects(self):
        with self._recent_lock:
            ids = self._recent_ids()
            catalog = {project['id']: project for project in self.store.list('project')}
            recent = [catalog[id] for id in ids if id in catalog and self._recent_available(catalog[id]) is not False]
            retained = [project['id'] for project in recent]
            # Persist even an empty pruned list; it must not be seeded again.
            if retained != ids:
                self.store.put('settings', {'id': 'recent_projects', 'version': 1, 'project_ids': retained})
            return recent

    def browser_path(self, value):
        candidate = Path(value)
        if not candidate.is_absolute():
            candidate = self.settings.workspace_root / candidate
        try:
            resolved = candidate.resolve()
        except RuntimeError:
            raise HTTPException(422, 'Cannot resolve a folder path containing a symlink loop')
        roots = self.settings.browser_roots
        if roots is not None and not any(resolved.is_relative_to(root) for root in roots):
            raise HTTPException(403, 'Path is outside the configured file browser roots')
        return resolved

    def default_browser_path(self):
        root = self.settings.workspace_root
        roots = self.settings.browser_roots
        return root if roots is None or any(root.is_relative_to(allowed) for allowed in roots) else roots[0]

    def discovery_roots(self):
        if self.settings.browser_roots is not None:
            return self.settings.browser_roots
        if os.name != 'nt':
            return (Path('/'),)
        # Drive discovery is a Windows API call, not a shell command. UNC shares
        # can also be reached directly by entering their absolute path.
        import ctypes
        drives = ctypes.windll.kernel32.GetLogicalDrives()
        roots = [Path(f'{chr(65 + index)}:/') for index in range(26) if drives & (1 << index)]
        if not roots:
            roots = [Path(Path.home().anchor), Path(self.settings.workspace_root.anchor)]
        return tuple(dict.fromkeys(roots))

    def browser_locations(self):
        shortcuts = []
        seen = set()
        def add(name, path):
            try:
                path = self.browser_path(path)
                if path.is_dir() and str(path) not in seen:
                    shortcuts.append({'name': name, 'path': str(path)})
                    seen.add(str(path))
            except (OSError, ValueError, HTTPException):
                pass
        home = Path.home()
        add('Home', home)
        add('Workspace', self.settings.workspace_root)
        onedrive = [Path(value) for key in ('OneDrive', 'OneDriveConsumer', 'OneDriveCommercial') if (value := os.environ.get(key))]
        try:
            if self.settings.browser_roots is None or any(home.resolve().is_relative_to(root) for root in self.settings.browser_roots):
                onedrive.extend(sorted(home.glob('OneDrive*')))
        except OSError:
            pass
        for path in onedrive:
            add('OneDrive', path)
        for name in ('Desktop', 'Documents', 'Downloads', 'Pictures', 'Music', 'Videos'):
            for base in [*onedrive, home]:
                before = len(shortcuts)
                add(name, base / name)
                if len(shortcuts) > before:
                    break
        return {'default_path': str(self.default_browser_path()), 'roots': [{'name': 'Home' if root == home.resolve() else 'Workspace' if root == self.settings.workspace_root else root.name or str(root), 'path': str(root)} for root in self.discovery_roots()], 'shortcuts': shortcuts}

    def list_directory(self, path=None, search='', offset=0, limit=200):
        folder = self.browser_path(path or self.default_browser_path())
        if not folder.is_dir():
            raise HTTPException(404, 'Folder not found')
        entries = []
        query = search.casefold()
        with os.scandir(folder) as children:
            for child in children:
                if child.name.startswith('.') or query not in child.name.casefold():
                    continue
                try:
                    child_path = self.browser_path(child.path)
                    directory = child.is_dir()
                    if not directory and not child.is_file():
                        continue
                    access = os.R_OK | (os.X_OK if directory and os.name != 'nt' else 0)
                    if not os.access(child_path, access):
                        continue
                    stat = child.stat()
                    entries.append({'name': child.name, 'path': str(child_path), 'kind': 'directory' if directory else 'file', 'size': None if directory else stat.st_size, 'modified_at': datetime.fromtimestamp(stat.st_mtime, timezone.utc).isoformat()})
                except (OSError, ValueError, HTTPException):
                    continue
        entries.sort(key=lambda entry: (entry['kind'] != 'directory', entry['name'].casefold(), entry['name']))
        try:
            parent = self.browser_path(folder.parent) if folder.parent != folder else None
        except HTTPException:
            parent = None
        return {'path': str(folder), 'parent_path': str(parent) if parent is not None else None, 'entries': entries[offset:offset + limit], 'total': len(entries), 'offset': offset, 'limit': limit}

    def create_folder(self, parent_path, name):
        valid_name(name)
        parent = self.browser_path(parent_path)
        if not parent.is_dir():
            raise HTTPException(404, 'Parent folder not found')
        path = self.browser_path(parent / name)
        try:
            path.mkdir()
        except FileExistsError:
            raise HTTPException(409, 'A file or folder with this name already exists')
        return {'path': str(path)}

    def path(self, id):
        project = self.store.get('project', id)
        path = self.browser_path(project['path'])
        if not path.is_dir():
            raise HTTPException(404, 'Project folder no longer exists')
        return path

    def memory_path(self, id):
        root = self.path(id)
        candidate = root / '.memory'
        if candidate.is_symlink() or getattr(candidate, 'is_junction', lambda: False)():
            raise HTTPException(403, 'Project memory cannot use a linked folder')
        try:
            path = contained(root, candidate)
        except RuntimeError:
            raise ValueError('Project memory contains a symlink loop')
        if path.exists() and not path.is_dir():
            raise ValueError('Project .memory must be a folder')
        return path

    def open(self, path):
        path = self.browser_path(path)
        if not path.is_dir():
            raise HTTPException(404, 'Project folder not found')
        name = path.name
        descriptor = path / 'project.connector'
        managed = descriptor.is_file()
        if managed:
            descriptor = contained(path, descriptor)
            if descriptor.stat().st_size > 16384:
                raise ValueError('Invalid project descriptor')
            lines = descriptor.read_text(encoding='utf-8').splitlines()
            if len(lines) != 3 or lines[0] != 'CONNECTOR_WORK_PROJECT_1':
                raise ValueError('Unsupported project descriptor')
            name = valid_name(lines[1])
        with self._recent_lock:
            existing = next((p for p in self.store.list('project') if p['path'] == str(path)), None)
            project = existing or {'id': str(uuid4()), 'name': name, 'path': str(path), 'managed': managed, 'created_at': now(), 'preferences': {}}
            project['updated_at'] = now()
            saved = self.store.put('project', project)
            self._remember_project(saved['id'])
            return saved

    def create(self, name, parent_path=None):
        valid_name(name)
        root = self.browser_path(parent_path or self.default_browser_path())
        if not root.is_dir():
            raise HTTPException(404, 'Parent folder not found')
        path = self.browser_path(root / name)
        try:
            path.mkdir()
        except FileExistsError:
            raise HTTPException(409, 'A project with this name already exists')
        (path / 'files').mkdir(); (path / 'sessions').mkdir()
        (path / 'project.connector').write_text(f'CONNECTOR_WORK_PROJECT_1\n{name}\nwork-project\n', encoding='utf-8')
        return self.open(path)

    def files(self, id):
        root = self.path(id)
        result = []
        def walk(folder, depth):
            if depth > 8 or len(result) >= 1000:
                return
            for path in sorted(folder.iterdir(), key=lambda p: (not p.is_dir(), p.name.lower())):
                if path.name.startswith('.') or path.name in ('node_modules', '__pycache__', '.venv') or path.is_symlink() or path.suffix == '.connector-item':
                    continue
                if not path.resolve().is_relative_to(root):
                    continue
                result.append({'path': path.relative_to(root).as_posix(), 'name': path.name, 'directory': path.is_dir(), 'depth': depth})
                if path.is_dir():
                    walk(path, depth + 1)
                if len(result) >= 1000:
                    break
        walk(root, 0)
        return result

    def resources(self, id, effort='low'):
        project = self.store.get('project', id)
        root = self.path(id)
        entries = []
        for kind, aliases in {'skill': ('skill', 'skills', '.skill', '.skills'), 'agent': ('agent', 'agents', '.agent', '.agents'), 'mcp': ('mcp', 'mcps', '.mcp', '.mcps')}.items():
            for alias in aliases:
                parent = root / alias
                if not parent.is_dir() or not parent.resolve().is_relative_to(root):
                    continue
                for child in sorted(parent.iterdir()):
                    if child.name == 'runs' and kind == 'agent':
                        continue
                    if not child.is_dir() or child.is_symlink() or not child.resolve().is_relative_to(root):
                        continue
                    key = child.relative_to(root).as_posix()
                    locked = effort != 'max' and (kind == 'agent' or kind == 'skill' and child.name in ('create_agent', 'run_agent'))
                    enabled = project['preferences'].get(key, {}).get('enabled', kind == 'skill' and child.name in ('run_python_script', 'run_batch_script', 'create_tool_from_conversation'))
                    entry = {'id': key, 'name': child.name, 'kind': kind, 'enabled': bool(enabled) and not locked, 'priority': project['preferences'].get(key, {}).get('priority', 10000), 'description': 'Saved connection definition.' if kind == 'mcp' else 'Project instructions and executable tool selection.', 'locked': locked}
                    if locked:
                        entry['lock_reason'] = 'Multi-agent skills require Max effort. Select Max, then enable the skill.'
                    entries.append(entry)
                    if len(entries) >= 1000:
                        return entries
        return sorted(entries, key=lambda r: (r['kind'], r['priority'], r['name'].lower()))

    def file(self, id, name, content):
        root = self.path(id)
        if not self.store.get('project', id)['managed']:
            raise HTTPException(409, 'Create files in a managed Work Project')
        parent = contained(root, root / 'files')
        path = contained(root, parent / valid_name(name))
        try:
            with path.open('x', encoding='utf-8') as file:
                file.write(content)
        except FileExistsError:
            raise HTTPException(409, 'File already exists')
        return {'path': path.relative_to(root).as_posix()}
