"""Readable portable session filenames, exclusive publication and rename rollback."""
import hashlib
import os
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import patch
from urllib.parse import unquote

from fastapi import HTTPException
from fastapi.testclient import TestClient

from backend.app.config import Settings
from backend.app.main import create_app
from backend.app.repository import Store
from backend.app.services import lattice
from backend.app.services.session_files import SessionFiles
from backend.app.services.session_names import download_disposition, session_filename, title_stem
from backend.app.services.workspace import Workspace


class SessionNameTests(unittest.TestCase):
    def test_portable_names_preserve_unicode_and_sanitize_devices_paths_controls_and_length(self):
        cases = {'test-1': 'test-1.lattice', 'Plan.lattice': 'Plan.lattice', 'Plan.LATTICE.lattice': 'Plan.lattice',
                 '  Trailing... ': 'Trailing.lattice', 'CON': '_CON.lattice', 'nul.txt': '_nul.txt.lattice',
                 'COM1': '_COM1.lattice', 'LPT9': '_LPT9.lattice', 'COM\u00b9': '_COM\u00b9.lattice',
                 'A/B\\C:D?E*F|G"H<I>J': 'A-B-C-D-E-F-G-H-I-J.lattice', 'line\nbreak': 'line-break.lattice',
                 '...': 'Session.lattice', '\u7814\u7a76 r\u00e9sum\u00e9': '\u7814\u7a76 r\u00e9sum\u00e9.lattice'}
        for title, expected in cases.items():
            with self.subTest(title=title):
                self.assertEqual(session_filename(title), expected)
        self.assertEqual(session_filename('e\u0301'), session_filename('\u00e9'))
        for title in ('x' * 200, '\U0001f680' * 200, '\u7814' * 200):
            name = session_filename(title, 10000)
            self.assertLessEqual(len(name.encode('utf-8')), 176)
            self.assertFalse(title_stem(title).endswith((' ', '.')))
            self.assertEqual(Path(name).name, name)
        for index in (0, 10001, True):
            with self.assertRaises(ValueError):
                session_filename('Name', index)

    def test_download_disposition_has_ascii_fallback_and_unicode_extended_filename(self):
        header = download_disposition('\u7814\u7a76 r\u00e9sum\u00e9\r\n"header')
        self.assertTrue(header.isascii())
        self.assertNotIn('\r', header)
        self.assertNotIn('\n', header)
        self.assertIn('filename="', header)
        extended = header.split("filename*=UTF-8''", 1)[1]
        self.assertEqual(unquote(extended), session_filename('\u7814\u7a76 r\u00e9sum\u00e9\r\n"header'))


class SessionFilenameTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='studio-session-name-test-')
        self.root = Path(self.directory.name).resolve()
        self.settings = Settings(data_dir=self.root / 'data', workspace_root=self.root / 'projects', browser_roots=[self.root])
        self.settings.workspace_root.mkdir()
        self.store = Store(self.settings.data_dir / 'studio.db')
        self.store.initialize()
        self.workspace = Workspace(self.settings, self.store)
        self.files = SessionFiles(self.settings, self.store, self.workspace)
        self.project = self.workspace.create('Naming project')

    def tearDown(self):
        self.directory.cleanup()

    def new(self, title='test-1', project=True):
        return self.files.new(title, self.project['id'] if project else None)

    def rename(self, session, title):
        self.files.update(session['id'], lambda current: current.update(title=title))
        return self.files.save(session['id'])

    def test_project_and_standalone_save_title_names_and_rename_without_duplicate_owned_copy(self):
        for project in (True, False):
            with self.subTest(project=project):
                session = self.new(project=project)
                saved = self.files.save(session['id'])
                old = Path(saved['saved_path'])
                self.assertEqual(old.name, 'test-1.lattice')
                renamed = self.rename(session, 'Renamed task')
                target = Path(renamed['saved_path'])
                self.assertEqual(target.name, 'Renamed task.lattice')
                self.assertFalse(old.exists())
                self.assertEqual(list(target.parent.iterdir()), [target])
                self.assertEqual(lattice.decode(target.read_bytes())['title'], 'Renamed task')
                self.assertEqual(hashlib.sha256(target.read_bytes()).hexdigest(), renamed['file_revision'])

    def test_duplicate_titles_case_aliases_and_unicode_normalization_use_stable_readable_suffixes(self):
        first = self.new('test-1')
        first_bytes = Path(first['saved_path']).read_bytes()
        second = self.new('test-1')
        third = self.new('TEST-1')
        self.assertEqual(Path(second['saved_path']).name, 'test-1 (2).lattice')
        self.assertEqual(Path(third['saved_path']).name, 'TEST-1 (3).lattice')
        self.assertEqual(Path(first['saved_path']).read_bytes(), first_bytes)
        Path(first['saved_path']).unlink()
        self.assertEqual(self.files.save(second['id'])['saved_path'], second['saved_path'])
        composed = self.new('\u00e9')
        decomposed = self.new('e\u0301')
        self.assertEqual(Path(composed['saved_path']).name, '\u00e9.lattice')
        self.assertEqual(Path(decomposed['saved_path']).name, '\u00e9 (2).lattice')

    def test_rename_collision_never_overwrites_other_file(self):
        first, second = self.new('First'), self.new('Second')
        before = Path(second['saved_path']).read_bytes()
        renamed = self.rename(first, 'Second')
        self.assertEqual(Path(renamed['saved_path']).name, 'Second (2).lattice')
        self.assertFalse(Path(first['saved_path']).exists())
        self.assertEqual(Path(second['saved_path']).read_bytes(), before)

    def test_case_only_rename_preserves_requested_disk_basename(self):
        session = self.new('Case Name')
        saved = self.rename(session, 'case name')
        parent = Path(saved['saved_path']).parent
        self.assertEqual([path.name for path in parent.iterdir()], ['case name.lattice'])
        self.assertEqual(Path(saved['saved_path']).name, 'case name.lattice')
        self.assertEqual(lattice.decode(Path(saved['saved_path']).read_bytes())['title'], 'case name')

    def test_existing_uuid_canonical_files_migrate_on_save_with_revision_protection(self):
        for project in (True, False):
            with self.subTest(project=project):
                session = self.new('Migrated title', project)
                if project:
                    Path(session['saved_path']).unlink()
                parent = Path(self.project['path']) / 'sessions' if project else self.settings.data_dir / 'sessions'
                parent.mkdir(exist_ok=True)
                legacy = parent / (session['id'] + '.lattice')
                before = lattice.encode(session)
                legacy.write_bytes(before)
                self.files.update(session['id'], lambda current: current.update(saved_path=str(legacy), file_revision=hashlib.sha256(before).hexdigest()))
                saved = self.files.save(session['id'])
                self.assertEqual(Path(saved['saved_path']).name, 'Migrated title.lattice')
                self.assertFalse(legacy.exists())
                self.assertEqual(lattice.decode(Path(saved['saved_path']).read_bytes())['title'], 'Migrated title')

    def test_external_change_before_rename_preserves_old_file_and_durable_save_error(self):
        session = self.new('Original')
        original = Path(session['saved_path'])
        changed = lattice.encode({**session, 'draft': 'Externally modified bytes'})
        original.write_bytes(changed)
        self.files.update(session['id'], lambda current: current.update(title='New title'))
        with self.assertRaises(HTTPException) as error:
            self.files.save(session['id'])
        self.assertEqual(error.exception.status_code, 409)
        stored = self.store.get('session', session['id'])
        self.assertEqual(stored['saved_path'], str(original))
        self.assertIn('changed outside', stored['save_error'])
        self.assertEqual(original.read_bytes(), changed)
        self.assertFalse((original.parent / 'New title.lattice').exists())

    def test_outside_canonical_import_source_and_mappings_survive_renames(self):
        source = Path(self.project['path']) / 'files' / 'source-name.lattice'
        before = lattice.encode({'title': 'Imported title', 'messages': []})
        source.write_bytes(before)
        opened = self.files.open(self.project['id'], 'files/source-name.lattice')
        first = Path(opened['saved_path'])
        renamed = self.rename(opened, 'Renamed import')
        self.assertFalse(first.exists())
        self.assertEqual(renamed['source_path'], str(source))
        self.assertEqual(renamed['source_revision'], hashlib.sha256(before).hexdigest())
        self.assertEqual(source.read_bytes(), before)
        self.assertEqual(self.files.open(self.project['id'], 'files/source-name.lattice')['id'], opened['id'])
        standalone = self.new('Adopted standalone', False)
        saved = self.files.save(standalone['id'])
        original = Path(saved['saved_path'])
        original_bytes = original.read_bytes()
        adopted = self.files.ensure_project_file(standalone['id'], self.project['id'])
        renamed = self.rename(adopted, 'Renamed adopted session')
        self.assertEqual(original.read_bytes(), original_bytes)
        self.assertEqual(renamed['source_path'], str(original))
        self.assertEqual(self.files.open(self.project['id'], 'files/source-name.lattice')['id'], opened['id'])

    def test_failed_publication_and_failed_old_file_cleanup_roll_back_files_and_metadata(self):
        session = self.new('Original')
        original = Path(session['saved_path'])
        before = original.read_bytes()
        self.files.update(session['id'], lambda current: current.update(title='Renamed'))
        with patch.object(self.files, '_publish_new', side_effect=PermissionError('Denied')):
            with self.assertRaises(PermissionError):
                self.files.save(session['id'])
        self.assertEqual(self.store.get('session', session['id'])['saved_path'], str(original))
        self.assertEqual(original.read_bytes(), before)
        unlink = Path.unlink
        def denied(path, *args, **kwargs):
            if path == original:
                raise PermissionError('Old file cleanup denied')
            return unlink(path, *args, **kwargs)
        with patch.object(Path, 'unlink', denied):
            with self.assertRaises(PermissionError):
                self.files.save(session['id'])
        stored = self.store.get('session', session['id'])
        self.assertEqual(stored['saved_path'], str(original))
        self.assertIn('could not be saved', stored['save_error'])
        self.assertEqual(original.read_bytes(), before)
        self.assertEqual(list(original.parent.iterdir()), [original])
        saved = self.files.save(session['id'])
        self.assertEqual(Path(saved['saved_path']).name, 'Renamed.lattice')
        self.assertFalse(original.exists())

    def test_failed_metadata_commit_rolls_back_rename_and_in_place_bytes(self):
        session = self.new('Original')
        original = Path(session['saved_path'])
        before = original.read_bytes()
        update = self.files.update
        failed = [False]
        def fail_once(id, mutate):
            if not failed[0]:
                failed[0] = True
                raise OSError('Fixture SQLite commit failure')
            return update(id, mutate)
        for title in ('Original', 'Renamed'):
            with self.subTest(title=title):
                self.files.update(session['id'], lambda current: current.update(title=title, draft='New draft'))
                failed[0] = False
                with patch.object(self.files, 'update', side_effect=fail_once):
                    with self.assertRaises(OSError):
                        self.files.save(session['id'])
                stored = self.store.get('session', session['id'])
                self.assertEqual(stored['saved_path'], str(original))
                self.assertEqual(original.read_bytes(), before)
                self.assertEqual(list(original.parent.iterdir()), [original])
                self.assertIn('could not be saved', stored['save_error'])

    def test_exclusive_publication_race_uses_another_suffix_without_overwriting(self):
        session = self.new('Race title', False)
        publish = self.files._publish_new
        raced = []
        unrelated = lattice.encode({'title': 'Other application file', 'messages': []})
        def race(path, data):
            if not raced:
                path.write_bytes(unrelated)
                raced.append(path)
                raise FileExistsError('Other application created this name')
            return publish(path, data)
        with patch.object(self.files, '_publish_new', side_effect=race):
            saved = self.files.save(session['id'])
        self.assertEqual(Path(saved['saved_path']).name, 'Race title (2).lattice')
        self.assertEqual(raced[0].read_bytes(), unrelated)

    def test_concurrent_duplicate_saves_are_exclusive_and_keep_both_sessions(self):
        sessions = [self.new('Concurrent', False), self.new('Concurrent', False)]
        with ThreadPoolExecutor(max_workers=2) as workers:
            saved = list(workers.map(lambda session: self.files.save(session['id']), sessions))
        self.assertEqual({Path(item['saved_path']).name for item in saved}, {'Concurrent.lattice', 'Concurrent (2).lattice'})
        self.assertEqual(len(list(Path(saved[0]['saved_path']).parent.iterdir())), 2)

    def test_agent_run_path_is_fixed_and_existing_unowned_agent_file_is_not_replaced(self):
        session = self.new('User chosen agent title', False)
        self.files.update(session['id'], lambda current: current.update(is_agent=True, read_only=True, hidden=True))
        saved = self.files.save(session['id'])
        expected = self.settings.data_dir / '.agent' / 'runs' / session['id'] / 'session.lattice'
        self.assertEqual(Path(saved['saved_path']), expected)
        self.files.update(session['id'], lambda current: current.update(title='Another agent title'))
        self.assertEqual(self.files.save(session['id'])['saved_path'], str(expected))
        second = self.new('Another agent', False)
        self.files.update(second['id'], lambda current: current.update(is_agent=True, read_only=True, hidden=True))
        path = self.settings.data_dir / '.agent' / 'runs' / second['id'] / 'session.lattice'
        path.parent.mkdir(parents=True)
        original = lattice.encode({'title': 'Unrelated agent file', 'messages': []})
        path.write_bytes(original)
        with self.assertRaises(HTTPException):
            self.files.save(second['id'])
        self.assertEqual(path.read_bytes(), original)

    def test_linked_session_directory_is_rejected_without_writing_outside(self):
        session = self.new('Linked', False)
        parent = self.settings.data_dir / 'sessions'
        is_symlink = Path.is_symlink
        def linked(path):
            return path == parent or is_symlink(path)
        with patch.object(Path, 'is_symlink', linked):
            with self.assertRaises(HTTPException) as error:
                self.files.save(session['id'])
        self.assertEqual(error.exception.status_code, 403)
        self.assertFalse(parent.exists())
        self.assertIn('cannot be linked', self.store.get('session', session['id'])['save_error'])


class DownloadFilenameTests(unittest.TestCase):
    def test_http_download_header_uses_session_title_with_unicode_encoding(self):
        with tempfile.TemporaryDirectory(prefix='studio-download-name-test-') as directory:
            root = Path(directory)
            app = create_app(Settings(data_dir=root / 'data', workspace_root=root / 'projects', browser_roots=[root]))
            with TestClient(app) as client:
                title = '\u7814\u7a76 r\u00e9sum\u00e9'
                project = client.post('/api/projects', json={'name': 'Download project'}).json()
                session = client.post('/api/sessions', json={'title': title, 'project_id': project['id']}).json()
                response = client.get('/api/sessions/' + session['id'] + '/download')
                self.assertEqual(response.status_code, 200)
                self.assertEqual(response.headers['content-disposition'], download_disposition(title))
                self.assertEqual(lattice.decode(response.content)['title'], title)
