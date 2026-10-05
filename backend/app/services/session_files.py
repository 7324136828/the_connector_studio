"""Keep project conversations in their sessions folder without changing source copies."""
import hashlib
import os
import threading
import tempfile
import re
from contextlib import contextmanager
from pathlib import Path, PurePosixPath, PureWindowsPath
from uuid import uuid4
from fastapi import HTTPException
from ..repository import now
from . import lattice
from .workspace import contained, atomic_write
from .project_memory import memory_error_detail
from .session_names import filename_key, session_filename, title_stem


class SessionFiles:
    def __init__(self, settings, store, workspace, project_memory=None):
        self.settings, self.store, self.workspace = settings, store, workspace
        self.project_memory = project_memory
        self._locks = {}
        self._locks_guard = threading.Lock()
        self._files_lock = threading.RLock()

    @contextmanager
    def lock(self, id):
        with self._locks_guard:
            lock = self._locks.setdefault(id, threading.RLock())
        with lock:
            yield

    def update(self, id, mutate):
        with self.lock(id):
            return self.store.update('session', id, mutate)

    def _inherit_default(self, session):
        if self.project_memory and session.get('project_id') and not session.get('model'):
            try:
                session['model'] = self.project_memory.preferences(session['project_id'])['default_model']
                session['memory_error'] = None
            except Exception as error:
                session['memory_error'] = memory_error_detail(error)
        return session

    def new(self, title, project_id=None, imported=None, *, persist=True):
        project = self.store.get('project', project_id) if project_id else None
        session = {'id': str(uuid4()), 'title': title, 'project_id': project_id,
                   'project_name': project['name'] if project else '', 'project_path': project['path'] if project else '',
                   'draft': '', 'model': '', 'effort': 'low', 'messages': [], 'attachments': [], 'pending_job': None,
                   'created_at': now(), 'updated_at': now(), 'saved_path': None, 'save_error': None, 'memory_error': None}
        source = dict(imported or {})
        children = source.pop('child_sessions', [])
        source.pop('id', None)
        session.update(source)
        if session.get('is_agent') or session.get('read_only'):
            session.update(is_agent=True, read_only=True, hidden=True)
            if session.get('agent_status') in ('queued', 'running'):
                session['agent_status'] = 'cancelled'
        if project:
            session.update(project_id=project_id, project_name=project['name'], project_path=project['path'])
        self._inherit_default(session)
        self.store.put('session', session)
        mapping = {}
        if children:
            snapshots = []
            for original in children:
                payload = dict(original)
                original_id = payload.pop('id')
                payload.update(is_agent=True, read_only=True, hidden=True, parent_session_id=session['id'])
                child = self.new(payload['title'], project_id, payload, persist=False)
                child = self.autosave(child['id'])
                mapping[original_id] = child['id']
                snapshots.append(child)
            session['child_sessions'] = snapshots
        for trace in session.get('execution_traces', []):
            for step in trace['steps']:
                if step.get('agent_session_id') in mapping:
                    step['agent_session_id'] = mapping[step['agent_session_id']]
                if isinstance(step.get('result'), dict) and step['result'].get('agent_session_id') in mapping:
                    step['result']['agent_session_id'] = mapping[step['result']['agent_session_id']]
                if step['status'] in ('queued', 'running'):
                    step.update(status='cancelled', summary='Imported snapshot; execution is no longer active')
            if trace['status'] in ('queued', 'running'):
                trace['status'] = 'cancelled'
        if session.get('execution_state', {}).get('phase') in ('queued', 'running', 'checking', 'summarizing', 'recovering', 'retrying'):
            session['execution_state'] = {**session['execution_state'], 'phase': 'cancelled'}
        self.store.put('session', session)
        return self.ensure_project_file(session['id']) if project_id and persist else session

    def _inside(self, root, path):
        try:
            return contained(root, path)
        except RuntimeError:
            raise HTTPException(422, 'Cannot resolve a session path containing a symlink loop')

    def _read(self, path):
        if not path.is_file():
            raise HTTPException(404, 'Session file not found')
        with path.open('rb') as file:
            data = file.read(lattice.MAX_FILE + 1)
        return data, lattice.decode(data)

    def _matches(self, value, path):
        try:
            return bool(value) and Path(value).resolve() == path
        except (OSError, RuntimeError, ValueError):
            return False

    def _project_folder(self, project_id):
        root = self.workspace.path(project_id)
        return self._inside(root, root / 'sessions')

    def open(self, project_id, relative_path):
        normalized = relative_path.replace('\\', '/')
        if PureWindowsPath(relative_path).anchor or PurePosixPath(normalized).anchor or '..' in PurePosixPath(normalized).parts:
            raise HTTPException(403, 'Choose a session file inside this project')
        root = self.workspace.path(project_id)
        path = self._inside(root, root / normalized)
        if path.suffix.lower() != '.lattice':
            raise ValueError('Choose a .lattice session file')
        with self._files_lock:
            data, imported = self._read(path)
            revision = hashlib.sha256(data).hexdigest()
            existing = next((session for session in self.store.list('session')
                             if self._matches(session.get('saved_path'), path) or self._matches(session.get('source_path'), path)), None)
            if existing:
                with self.lock(existing['id']):
                    current = self.store.get('session', existing['id'])
                    if current.get('project_id') not in (None, project_id):
                        raise HTTPException(409, 'This session belongs to another project')
                    expected = current.get('file_revision') if self._matches(current.get('saved_path'), path) else current.get('source_revision')
                    if expected and expected != revision:
                        raise HTTPException(409, 'This session file changed outside the app. Open a copy before replacing the conversation.')
                    if self._matches(current.get('saved_path'), path):
                        self.update(current['id'], lambda session: session.update(file_revision=revision))
                    else:
                        self.update(current['id'], lambda session: session.update(source_revision=revision))
                    # Reopening retains the live draft and running job, then retries any failed save.
                    return self.ensure_project_file(current['id'], project_id)
            project_folder = self._project_folder(project_id)
            session = self.new(imported['title'], project_id, imported, persist=False)
            if path.is_relative_to(project_folder):
                self.update(session['id'], lambda current: current.update(saved_path=str(path), file_revision=revision, saved_at=now()))
            else:
                self.update(session['id'], lambda current: current.update(source_path=str(path), source_revision=revision))
            return self.ensure_project_file(session['id'], project_id)

    def ensure_project_file(self, id, project_id=None):
        with self._files_lock, self.lock(id):
            session = self.store.get('session', id)
            if project_id and session.get('project_id') not in (None, project_id):
                raise HTTPException(409, 'This session belongs to another project')
            if project_id and not session.get('project_id'):
                project = self.store.get('project', project_id)
                self.update(id, lambda current: current.update(project_id=project_id, project_name=project['name'], project_path=project['path']))
            session = self.store.get('session', id)
            if not session.get('project_id'):
                return session
            inherited = self._inherit_default(dict(session))
            if inherited != session:
                self.update(id, lambda current: current.update(model=inherited['model'], memory_error=inherited.get('memory_error')))
            try:
                return self.save(id)
            except Exception:
                # Creation/opening still returns the durable ID and visible save error.
                return self.store.get('session', id)

    def _location(self, session):
        if session.get('is_agent'):
            root = self.workspace.path(session['project_id']) if session.get('project_id') else self.settings.data_dir.resolve()
            parent = root
            for part in ('.agent', 'runs', session['id']):
                candidate = parent / part
                if candidate.is_symlink() or getattr(candidate, 'is_junction', lambda: False)():
                    raise HTTPException(403, 'Agent session folders cannot be linked')
                parent = self._inside(root, candidate)
                parent.mkdir(exist_ok=True)
            return parent
        root = self.workspace.path(session['project_id']) if session.get('project_id') else self.settings.data_dir.resolve()
        candidate = root / 'sessions'
        if candidate.is_symlink() or getattr(candidate, 'is_junction', lambda: False)():
            raise HTTPException(403, 'Session folders cannot be linked')
        parent = self._inside(root, candidate)
        parent.mkdir(parents=True, exist_ok=True)
        return parent

    def _previous(self, session, parent):
        if not session.get('saved_path'):
            return None, False
        previous = Path(session['saved_path'])
        if previous.suffix.lower() != '.lattice':
            raise ValueError('Saved session path must end in .lattice')
        if previous.is_symlink() or getattr(previous, 'is_junction', lambda: False)():
            raise HTTPException(403, 'Saved session files cannot be linked')
        try:
            previous = previous.resolve()
        except RuntimeError:
            raise HTTPException(422, 'Cannot resolve a session path containing a symlink loop')
        data_sessions = (self.settings.data_dir / 'sessions').resolve()
        project_root = self.workspace.path(session['project_id']) if session.get('project_id') else None
        if not previous.is_relative_to(parent) and not previous.is_relative_to(data_sessions) and not (project_root and previous.is_relative_to(project_root)):
            raise HTTPException(403, 'Session source is outside its project and application sessions folders')
        return previous, previous.is_relative_to(parent)

    def _target(self, session, parent=None, previous=None, excluded=()):
        parent = parent or self._location(session)
        if session.get('is_agent'):
            return parent / 'session.lattice'
        stem = title_stem(session['title'])
        preferred = 1
        if previous is not None and previous.parent == parent:
            same_title = session.get('file_title_stem')
            if same_title is None or filename_key(same_title) == filename_key(stem):
                match = re.search(r' \(([2-9]|[1-9][0-9]{1,3}|10000)\)\.lattice$', previous.name, re.IGNORECASE)
                index = int(match.group(1)) if match else 1
                if filename_key(previous.name) == filename_key(session_filename(session['title'], index)):
                    preferred = index
        occupied = {}
        for entry in parent.iterdir():
            occupied.setdefault(filename_key(entry.name), []).append(entry)
        claims = {}
        for other in self.store.list('session'):
            if other['id'] == session['id'] or not other.get('saved_path'):
                continue
            claim = Path(other['saved_path'])
            try:
                if claim.parent.resolve() == parent:
                    claims.setdefault(filename_key(claim.name), []).append(claim)
            except (OSError, RuntimeError):
                continue
        for index in [preferred, *(number for number in range(1, 10001) if number != preferred)]:
            name = session_filename(session['title'], index)
            key = filename_key(name)
            if key in excluded or key in claims:
                continue
            if any(previous is None or not self._matches(previous, entry.resolve()) for entry in occupied.get(key, [])):
                continue
            path = parent / name
            # Preserve requested filename casing after checking its resolved path.
            self._inside(parent, path)
            return path
        raise HTTPException(409, 'Too many sessions share this filename; choose another title')

    def _publish_new(self, path, data):
        descriptor, temporary = tempfile.mkstemp(prefix='.studio-', dir=path.parent)
        staged = Path(temporary)
        published = False
        try:
            with os.fdopen(descriptor, 'wb') as file:
                file.write(data)
                file.flush()
                os.fsync(file.fileno())
            if os.name == 'nt':
                # Windows rename refuses an existing target, including case aliases.
                os.rename(staged, path)
            else:
                # Same-directory links publish complete bytes exclusively on POSIX.
                os.link(staged, path)
            published = True
        finally:
            try:
                staged.unlink(missing_ok=True)
            except OSError:
                if not published:
                    raise

    def _same_revision(self, path, revision):
        if not path.is_file() or path.is_symlink():
            return False
        with path.open('rb') as file:
            return hashlib.sha256(file.read(lattice.MAX_FILE + 1)).hexdigest() == revision

    def _restore_metadata(self, id, original):
        fields = ('saved_path', 'saved_at', 'file_revision', 'file_title_stem', 'source_path', 'source_revision', 'save_error')
        def restore(current):
            for field in fields:
                if field in original:
                    current[field] = original[field]
                else:
                    current.pop(field, None)
        return self.update(id, restore)

    def snapshot(self, id):
        with self.lock(id):
            return self._snapshot(self.store.get('session', id))

    def _snapshot(self, session):
        snapshot = dict(session)
        if session.get('child_sessions'):
            children = []
            for child in session['child_sessions']:
                try:
                    children.append(self.store.get('session', child['id']))
                except HTTPException:
                    children.append(child)
            snapshot['child_sessions'] = children
        if not session.get('pending_job') or session.get('is_agent'):
            return snapshot
        job = self.store.get('job', session['pending_job'])
        snapshot['messages'] = [message for message in session['messages'] if message.get('job_id') != job['id']]
        draft = session.get('draft', '')
        snapshot['draft'] = job['submitted_text'] + ('\n' + draft if draft else '')
        return snapshot

    def _detail(self, error):
        if isinstance(error, HTTPException):
            return str(error.detail)
        if isinstance(error, ValueError):
            return str(error)
        return 'Session could not be saved. Check folder permissions and free disk space, then retry Save.'

    def save(self, id):
        with self._files_lock, self.lock(id):
            session = self.store.get('session', id)
            path, previous, old_data, data_revision = None, None, None, None
            changed_in_place, published, committed = False, False, False
            try:
                parent = self._location(session)
                previous, owned = self._previous(session, parent)
                old_revision = session.get('file_revision')
                if previous is not None and previous.exists():
                    old_data, _ = self._read(previous)
                    revision = hashlib.sha256(old_data).hexdigest()
                    if old_revision and revision != old_revision:
                        raise HTTPException(409, 'This session file changed outside the app. Save a copy before replacing it.')
                    old_revision = revision
                data = lattice.encode(self._snapshot(session))
                data_revision = hashlib.sha256(data).hexdigest()
                excluded = set()
                for _ in range(10000):
                    path = self._target(session, parent, previous if owned else None, excluded)
                    if path.exists() and previous is not None and self._matches(previous, path.resolve()):
                        # Check again immediately before replacing an owned canonical file.
                        if old_revision and not self._same_revision(previous, old_revision):
                            raise HTTPException(409, 'This session file changed outside the app. Save a copy before replacing it.')
                        changed_in_place = True
                        atomic_write(path, data)
                        break
                    try:
                        self._publish_new(path, data)
                        published = True
                        break
                    except FileExistsError:
                        if session.get('is_agent'):
                            raise HTTPException(409, 'A file already exists at this agent session save location')
                        excluded.add(filename_key(path.name))
                else:
                    raise HTTPException(409, 'Too many files appeared at this save location; retry with another title')
                changes = dict(saved_path=str(path), saved_at=now(), file_revision=data_revision, save_error=None)
                if not session.get('is_agent'):
                    changes['file_title_stem'] = title_stem(session['title'])
                if previous is not None and not owned:
                    changes.update(source_path=str(previous), source_revision=old_revision)
                saved = self.update(id, lambda current: current.update(changes))
                committed = True
                if published and owned and previous is not None and previous.exists() and not self._matches(previous, path.resolve()):
                    # The previous file must still be the owned revision before removal.
                    if old_revision and not self._same_revision(previous, old_revision):
                        raise HTTPException(409, 'This session file changed outside the app during rename. Save a copy before replacing it.')
                    previous.unlink()
                return saved
            except Exception as error:
                # Keep the old canonical file/path on failed rename or metadata commit.
                rollback_ok = True
                if committed:
                    try:
                        self._restore_metadata(id, session)
                    except Exception:
                        rollback_ok = False
                try:
                    if rollback_ok and published and path is not None and self._same_revision(path, data_revision):
                        path.unlink()
                    elif rollback_ok and changed_in_place and previous is not None and old_data is not None and self._same_revision(path, data_revision):
                        atomic_write(previous, old_data)
                except Exception:
                    pass  # Preserve surviving files and expose the durable save error.
                self.update(id, lambda current: current.update(save_error=self._detail(error)))
                raise

    def autosave(self, id):
        session = self.store.get('session', id)
        if not session.get('saved_path') and not session.get('project_id') and not session.get('is_agent'):
            return session
        try:
            return self.save(id)
        except Exception:
            # The reply remains committed in SQLite even when its file cannot be written.
            return self.store.get('session', id)
