import asyncio
import io
import hashlib
import json
import os
import struct
import threading
from concurrent.futures import ThreadPoolExecutor
import tempfile
import time
import unittest
import zipfile
from unittest.mock import patch
from pathlib import Path
from fastapi.testclient import TestClient
import httpx
from backend.app.config import Settings
from backend.app.main import create_app
from backend.app.services import lattice
from backend.app.services.session_names import session_filename
from backend.app.utils.temp_manager import TempManager

class StudioTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        workspace_root = Path(self.directory.name) / 'projects'
        self.settings = Settings(data_dir=Path(self.directory.name), workspace_root=workspace_root, browser_roots=[workspace_root])
        self.requests = []
        async def mock(request):
            self.requests.append(request)
            if request.url.path == '/v1/models':
                return httpx.Response(200, json={'data': [{'id': 'test-config', 'name': 'Test model'}]})
            body = json.loads(request.content)
            text = body['messages'][-1]['content']
            if text == 'slow':
                await asyncio.sleep(20)
            if text == 'failure':
                return httpx.Response(401, json={'error': 'secret value must not be exposed'})
            if text == 'tools':
                return httpx.Response(200, json={'choices': [{'message': {'tool_calls': [{}], 'content': None}}]})
            return httpx.Response(200, json={'choices': [{'message': {'role': 'assistant', 'content': 'Reply to ' + text}}]})
        self.app = create_app(self.settings, httpx.MockTransport(mock))
        self.context = TestClient(self.app)
        self.client = self.context.__enter__()

    def tearDown(self):
        self.context.__exit__(None, None, None)
        for path in self.app.state.pipeline.temp.root.glob('job-*'):
            self.app.state.pipeline.temp.purge(path)
        self.directory.cleanup()

    def session(self, **changes):
        if 'project_id' not in changes:
            if not hasattr(self, 'fixture_project'):
                self.fixture_project = self.client.post('/api/projects', json={'name': 'Fixture project'}).json()
            changes['project_id'] = self.fixture_project['id']
        result = self.client.post('/api/sessions', json={'title': 'Conversation', **changes})
        self.assertEqual(result.status_code, 201, result.text)
        return result.json()

    def legacy_session(self, title='Conversation'):
        return self.app.state.session_files.new(title)

    def wait(self, id):
        for _ in range(100):
            job = self.client.get('/api/jobs/' + id).json()
            if job['status'] not in ('queued', 'in_progress'):
                return job
            time.sleep(.02)
        self.fail('Job did not finish')

    def test_project_save_and_native_import(self):
        project = self.client.post('/api/projects', json={'name': 'Research'}).json()
        session = self.session(project_id=project['id'])
        self.client.patch('/api/sessions/' + session['id'], json={'draft': 'Unicode Ω draft', 'attachments': ['notes.md']})
        saved = self.client.post('/api/sessions/' + session['id'] + '/save').json()
        self.assertTrue(Path(saved['saved_path']).is_file())
        data = self.client.get('/api/sessions/' + session['id'] + '/download').content
        self.assertEqual(data[:8], b'LATTICE\0')
        self.assertEqual(struct.unpack('<I', data[8:12])[0], 1)
        native = lattice.decode(data)
        self.assertEqual(native['draft'], 'Unicode Ω draft')
        self.assertEqual(native['attachments'], ['notes.md'])
        source = Path(project['path']) / 'files' / 'native.lattice'
        source.write_bytes(data)
        response = self.client.post('/api/projects/' + project['id'] + '/sessions/open', json={'path': 'files/native.lattice'})
        self.assertEqual(response.status_code, 200, response.text)
        imported = response.json()
        self.assertEqual(imported['draft'], native['draft'])
        self.assertNotEqual(imported['id'], session['id'])
        corrupted = data[:-1] + bytes([data[-1] ^ 1])
        (source.parent / 'bad.lattice').write_bytes(corrupted)
        self.assertEqual(self.client.post('/api/projects/' + project['id'] + '/sessions/open', json={'path': 'files/bad.lattice'}).status_code, 422)
        self.assertEqual(self.client.post('/api/sessions/import', files={'file': ('native.lattice', data)}).status_code, 410)

    def test_path_boundaries_and_no_overwrite(self):
        self.assertEqual(self.client.post('/api/projects', json={'name': '../outside'}).status_code, 422)
        self.assertEqual(self.client.post('/api/projects/open', json={'path': str(Path(self.directory.name).parent)}).status_code, 403)
        project = self.client.post('/api/projects', json={'name': 'Project'}).json()
        self.assertEqual(self.client.post('/api/projects', json={'name': 'Project'}).status_code, 409)
        url = '/api/projects/' + project['id'] + '/files'
        self.assertEqual(self.client.post(url, json={'name': '../bad'}).status_code, 422)
        self.assertEqual(self.client.post(url, json={'name': 'notes.md', 'content': 'kept'}).status_code, 201)
        self.assertEqual(self.client.post(url, json={'name': 'notes.md', 'content': 'overwritten'}).status_code, 409)
        self.assertEqual((Path(project['path']) / 'files/notes.md').read_text(), 'kept')

    def test_filesystem_roots_listing_search_and_pagination(self):
        root = self.settings.workspace_root
        (root / 'Alpha').mkdir()
        (root / 'Zulu').mkdir()
        (root / 'beta.txt').write_text('metadata only', encoding='utf-8')
        (root / '.hidden').write_text('not listed', encoding='utf-8')
        with patch('backend.app.services.workspace.Path.home', return_value=root):
            locations = self.client.get('/api/filesystem/roots')
        self.assertEqual(locations.status_code, 200, locations.text)
        self.assertEqual(locations.json()['default_path'], str(root))
        self.assertEqual([entry['path'] for entry in locations.json()['roots']], [str(root)])
        response = self.client.get('/api/filesystem/list', params={'path': str(root), 'limit': 2})
        self.assertEqual(response.status_code, 200, response.text)
        first = response.json()
        self.assertIsNone(first['parent_path'])
        self.assertEqual(first['total'], 3)
        self.assertEqual([entry['name'] for entry in first['entries']], ['Alpha', 'Zulu'])
        self.assertTrue(all(entry['kind'] == 'directory' and entry['size'] is None for entry in first['entries']))
        second = self.client.get('/api/filesystem/list', params={'path': str(root), 'offset': 2, 'limit': 2}).json()
        self.assertEqual(second['entries'][0]['name'], 'beta.txt')
        self.assertEqual(second['entries'][0]['size'], len('metadata only'))
        self.assertEqual(set(second['entries'][0]), {'name', 'path', 'kind', 'size', 'modified_at'})
        self.assertEqual(second['entries'][0]['kind'], 'file')
        self.assertTrue(second['entries'][0]['modified_at'].endswith('+00:00'))
        self.assertEqual(self.client.get('/api/filesystem/list', params={'search': 'ALP'}).json()['total'], 1)
        nested = self.client.get('/api/filesystem/list', params={'path': str(root / 'Alpha')}).json()
        self.assertEqual(nested['parent_path'], str(root))
        self.assertEqual(nested['total'], 0)

    def test_parent_folder_project_creation_and_existing_open(self):
        selected_parent = Path(self.directory.name) / 'Documents'
        selected_parent.mkdir()
        self.settings.browser_roots = (*self.settings.browser_roots, selected_parent)
        result = self.client.post('/api/projects', json={'name': 'Chosen Project', 'parent_path': str(selected_parent)})
        self.assertEqual(result.status_code, 201, result.text)
        project = result.json()
        folder = selected_parent / 'Chosen Project'
        self.assertEqual(project['path'], str(folder))
        self.assertEqual((folder / 'project.connector').read_text(), 'CONNECTOR_WORK_PROJECT_1\nChosen Project\nwork-project\n')
        self.assertTrue((folder / 'files').is_dir())
        self.assertTrue((folder / 'sessions').is_dir())
        self.assertEqual(self.client.post('/api/projects/open', json={'path': str(folder)}).json()['id'], project['id'])
        self.assertEqual(self.client.post('/api/projects/' + project['id'] + '/files', json={'name': 'notes.md', 'content': 'saved'}).status_code, 201)
        saved = self.client.post('/api/sessions/' + self.session(project_id=project['id'])['id'] + '/save')
        self.assertEqual(saved.status_code, 200, saved.text)
        self.assertTrue(Path(saved.json()['saved_path']).is_relative_to(folder))
        self.assertEqual(self.client.post('/api/projects', json={'name': 'Chosen Project', 'parent_path': str(selected_parent)}).status_code, 409)
        missing = selected_parent / 'missing'
        self.assertEqual(self.client.post('/api/projects', json={'name': 'No parent', 'parent_path': str(missing)}).status_code, 404)
        self.assertFalse(missing.exists())

    def test_filesystem_create_folder_invalid_names_and_boundaries(self):
        root = self.settings.workspace_root
        response = self.client.post('/api/filesystem/folders', json={'parent_path': str(root), 'name': 'New Folder'})
        self.assertEqual(response.status_code, 201, response.text)
        self.assertEqual(response.json(), {'path': str(root / 'New Folder')})
        self.assertTrue((root / 'New Folder').is_dir())
        self.assertEqual(list((root / 'New Folder').iterdir()), [])
        self.assertEqual(self.client.post('/api/filesystem/folders', json={'parent_path': str(root), 'name': 'New Folder'}).status_code, 409)
        for name in ('../escape', '..', 'CON', 'NUL.txt', 'name.', 'trailing ', 'a/b', 'a\\b', 'bad:name', ' '):
            with self.subTest(name=name):
                self.assertEqual(self.client.post('/api/filesystem/folders', json={'parent_path': str(root), 'name': name}).status_code, 422)
                self.assertEqual(self.client.post('/api/projects', json={'parent_path': str(root), 'name': name}).status_code, 422)
        outside = root / '..'
        self.assertEqual(self.client.get('/api/filesystem/list', params={'path': str(outside)}).status_code, 403)
        self.assertEqual(self.client.post('/api/filesystem/folders', json={'parent_path': str(outside), 'name': 'Escape'}).status_code, 403)
        self.assertEqual(self.client.post('/api/projects', json={'parent_path': str(outside), 'name': 'Escape'}).status_code, 403)
        self.assertFalse((root.parent / 'Escape').exists())
        self.assertEqual(self.client.get('/api/filesystem/list', params={'path': str(root / 'missing')}).status_code, 404)
        (root / 'ordinary.txt').write_text('not a directory', encoding='utf-8')
        self.assertEqual(self.client.get('/api/filesystem/list', params={'path': str(root / 'ordinary.txt')}).status_code, 404)
        for params in ({'offset': -1}, {'limit': 0}, {'limit': 501}, {'search': 'x' * 201}, {'path': ''}):
            self.assertEqual(self.client.get('/api/filesystem/list', params=params).status_code, 422)

    def test_filesystem_symlink_escape_is_blocked_and_not_listed(self):
        root = self.settings.workspace_root
        outside = Path(self.directory.name) / 'outside'
        outside.mkdir()
        (outside / 'private.txt').write_text('not exposed', encoding='utf-8')
        link = root / 'Escape link'
        try:
            link.symlink_to(outside, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest('Creating symlinks is unavailable on this system')
        self.assertEqual(self.client.get('/api/filesystem/list', params={'path': str(link)}).status_code, 403)
        self.assertNotIn('Escape link', [entry['name'] for entry in self.client.get('/api/filesystem/list').json()['entries']])
        self.assertEqual(self.client.post('/api/projects/open', json={'path': str(link)}).status_code, 403)
        self.assertEqual(self.client.post('/api/projects', json={'parent_path': str(link), 'name': 'No escape'}).status_code, 403)
        self.assertEqual(self.client.post('/api/filesystem/folders', json={'parent_path': str(link), 'name': 'No escape'}).status_code, 403)
        self.assertFalse((outside / 'No escape').exists())

    def test_filesystem_skips_entries_when_access_is_denied(self):
        root = self.settings.workspace_root
        (root / 'Visible').mkdir()
        (root / 'Denied').mkdir()
        with patch('backend.app.services.workspace.os.access', side_effect=lambda path, mode: Path(path).name != 'Denied'):
            listing = self.client.get('/api/filesystem/list').json()
        self.assertEqual([entry['name'] for entry in listing['entries']], ['Visible'])

    def test_filesystem_handles_symlink_loop_resolution_errors(self):
        root = self.settings.workspace_root
        (root / 'Readable').mkdir()
        loop = root / 'Loop'
        loop.mkdir()
        resolve = Path.resolve
        def resolve_path(path, *args, **kwargs):
            if path.name == 'Loop':
                raise RuntimeError('Symlink loop')
            return resolve(path, *args, **kwargs)
        with patch.object(Path, 'resolve', resolve_path):
            listing = self.client.get('/api/filesystem/list')
            self.assertEqual(listing.status_code, 200, listing.text)
            self.assertEqual([entry['name'] for entry in listing.json()['entries']], ['Readable'])
            direct = self.client.get('/api/filesystem/list', params={'path': str(loop)})
            self.assertEqual(direct.status_code, 422, direct.text)

    def test_default_project_parent_uses_configured_browser_root(self):
        root = Path(self.directory.name) / 'Allowed'
        root.mkdir()
        self.settings.browser_roots = (root,)
        self.assertEqual(self.client.get('/api/filesystem/roots').json()['default_path'], str(root))
        response = self.client.post('/api/projects', json={'name': 'Custom root'})
        self.assertEqual(response.status_code, 201, response.text)
        self.assertEqual(response.json()['path'], str(root / 'Custom root'))

    def test_local_origins_work_on_other_ports_with_cors_preflight(self):
        settings = Settings(data_dir=Path(self.directory.name) / 'origin-data', workspace_root=self.settings.workspace_root, browser_roots=[self.settings.workspace_root])
        with patch.dict('os.environ', {'CORS_ORIGINS': 'http://localhost:5174'}):
            app = create_app(settings)
        with TestClient(app) as client:
            origins = ('http://127.0.0.1:5173', 'http://localhost:5181', 'http://[::1]:5173', 'https://localhost:65535', 'http://127.0.0.1')
            for index, origin in enumerate(origins):
                with self.subTest(origin=origin):
                    preflight = client.options('/api/filesystem/folders', headers={'Origin': origin, 'Access-Control-Request-Method': 'POST', 'Access-Control-Request-Headers': 'Content-Type'})
                    self.assertEqual(preflight.status_code, 200, preflight.text)
                    self.assertEqual(preflight.headers.get('access-control-allow-origin'), origin)
                    response = client.post('/api/filesystem/folders', headers={'Origin': origin}, json={'parent_path': str(self.settings.workspace_root), 'name': f'Origin-{index}'})
                    self.assertEqual(response.status_code, 201, response.text)
                    self.assertEqual(response.headers.get('access-control-allow-origin'), origin)
            for origin in ('null', 'https://evil.example', 'http://localhost.evil.example', 'http://127.0.0.1.evil.example', 'http://localhost@evil.example', 'http://localhost:99999', 'http://localhost:0', 'http://localhost/path'):
                with self.subTest(origin=origin):
                    response = client.post('/api/filesystem/folders', headers={'Origin': origin}, json={'parent_path': str(self.settings.workspace_root), 'name': 'Blocked origin'})
                    self.assertEqual(response.status_code, 403)
                    self.assertEqual(response.json()['detail'], 'Origin is not allowed')
                    self.assertIsNone(response.headers.get('access-control-allow-origin'))
                    preflight = client.options('/api/filesystem/folders', headers={'Origin': origin, 'Access-Control-Request-Method': 'POST'})
                    self.assertEqual(preflight.status_code, 400)
            self.assertFalse((self.settings.workspace_root / 'Blocked origin').exists())

    def test_unrestricted_browser_creates_projects_outside_workspace(self):
        self.settings.browser_roots = None
        external = Path(self.directory.name) / 'External documents'
        external.mkdir()
        response = self.client.post('/api/filesystem/folders', headers={'Origin': 'http://127.0.0.1:5173'}, json={'parent_path': str(external), 'name': 'Anywhere'})
        self.assertEqual(response.status_code, 201, response.text)
        parent = Path(response.json()['path'])
        listing = self.client.get('/api/filesystem/list', params={'path': str(external)})
        self.assertEqual(listing.status_code, 200, listing.text)
        self.assertEqual(listing.json()['entries'][0]['name'], 'Anywhere')
        self.assertEqual(listing.json()['parent_path'], str(external.parent))
        locations = self.client.get('/api/filesystem/roots').json()
        self.assertEqual(locations['default_path'], str(self.settings.workspace_root))
        self.assertTrue(any(Path(entry['path']).anchor == entry['path'] for entry in locations['roots']))
        project = self.client.post('/api/projects', json={'name': 'Outside workspace', 'parent_path': str(parent)})
        self.assertEqual(project.status_code, 201, project.text)
        folder = parent / 'Outside workspace'
        self.assertEqual(project.json()['path'], str(folder))
        self.assertEqual(self.client.post('/api/projects/open', json={'path': str(folder)}).status_code, 200)
        session = self.session(project_id=project.json()['id'])
        saved = self.client.post('/api/sessions/' + session['id'] + '/save')
        self.assertEqual(saved.status_code, 200, saved.text)
        self.assertTrue(Path(saved.json()['saved_path']).is_relative_to(folder / 'sessions'))

    def test_unrestricted_folder_creation_still_respects_os_permissions(self):
        self.settings.browser_roots = None
        external = Path(self.directory.name) / 'OS-protected fixture'
        external.mkdir()
        with patch('backend.app.services.workspace.Path.mkdir', side_effect=PermissionError('Permission denied')):
            response = self.client.post('/api/filesystem/folders', json={'parent_path': str(external), 'name': 'Denied'})
        self.assertEqual(response.status_code, 409, response.text)
        self.assertIn('Check permissions', response.json()['detail'])
        self.assertFalse((external / 'Denied').exists())

    def test_project_source_session_copies_into_sessions_and_reuses_record(self):
        project = self.client.post('/api/projects', json={'name': 'Native sessions'}).json()
        unrelated = self.legacy_session(title='Unrelated')
        fixture = lattice.decode(self.client.get('/api/sessions/' + unrelated['id'] + '/download').content)
        fixture.update(title='Existing conversation', draft='existing draft', project_name='Old project', project_path='/old/project')
        fixture['messages'] = [{'role': 'user', 'author': 'You', 'time': 'before', 'text': 'Previous question'}, {'role': 'assistant', 'author': 'test-config', 'time': 'before', 'text': 'Previous answer'}]
        path = Path(project['path']) / 'files' / 'existing.lattice'
        path.write_bytes(lattice.encode(fixture))
        source_bytes = path.read_bytes()
        url = '/api/projects/' + project['id'] + '/sessions/open'
        opened = self.client.post(url, json={'path': 'files/existing.lattice'})
        self.assertEqual(opened.status_code, 200, opened.text)
        session = opened.json()
        canonical = Path(project['path']) / 'sessions' / session_filename(session['title'])
        self.assertEqual(session['saved_path'], str(canonical))
        self.assertEqual(session['source_path'], str(path))
        self.assertEqual(session['project_id'], project['id'])
        self.assertEqual(session['project_path'], project['path'])
        self.assertEqual(session['messages'][1]['text'], 'Previous answer')
        self.assertEqual(self.client.post(url, json={'path': 'files/existing.lattice'}).json()['id'], session['id'])
        self.client.patch('/api/sessions/' + session['id'], json={'title': 'Updated title', 'draft': 'updated draft'})
        saved = self.client.post('/api/sessions/' + session['id'] + '/save')
        self.assertEqual(saved.status_code, 200, saved.text)
        old_canonical = canonical
        canonical = canonical.parent / 'Updated title.lattice'
        self.assertEqual(saved.json()['saved_path'], str(canonical))
        self.assertFalse(old_canonical.exists())
        written = lattice.decode(canonical.read_bytes())
        self.assertEqual((written['title'], written['draft']), ('Updated title', 'updated draft'))
        self.assertEqual(path.read_bytes(), source_bytes)
        self.assertEqual(list((Path(project['path']) / 'sessions').iterdir()), [canonical])
        self.assertIsNone(self.client.get('/api/sessions/' + unrelated['id']).json()['project_id'])
        self.assertEqual(self.client.get('/api/sessions/' + unrelated['id']).json()['messages'], [])
        rejected = self.client.post('/api/sessions/import', files={'file': ('copy.lattice', path.read_bytes())})
        self.assertEqual(rejected.status_code, 410)

    def test_completed_reply_autosaves_file_backed_session(self):
        project = self.client.post('/api/projects', json={'name': 'Autosave'}).json()
        session = self.session(project_id=project['id'])
        saved = self.client.post('/api/sessions/' + session['id'] + '/save').json()
        path = Path(saved['saved_path'])
        job = self.client.post('/api/sessions/' + session['id'] + '/messages', json={'text': 'hello', 'model': 'test-config'}).json()
        self.assertEqual(self.wait(job['id'])['status'], 'completed')
        written = lattice.decode(path.read_bytes())
        self.assertEqual([message['text'] for message in written['messages']], ['hello', 'Reply to hello'])
        current = self.client.get('/api/sessions/' + session['id']).json()
        self.assertEqual(current['saved_path'], str(path))
        self.assertIsNone(current['save_error'])
        reopened = self.client.post('/api/projects/' + project['id'] + '/sessions/open', json={'path': path.relative_to(project['path']).as_posix()}).json()
        self.assertEqual(reopened['id'], session['id'])
        self.assertEqual(len(self.client.get('/api/sessions').json()), 1)

    def test_unmanaged_project_source_session_autosaves_canonical_copy(self):
        self.settings.browser_roots = None
        root = Path(self.directory.name) / 'Existing ordinary folder'
        root.mkdir()
        seed = self.legacy_session(title='Native file')
        path = root / 'original.lattice'
        path.write_bytes(lattice.encode(seed))
        project = self.client.post('/api/projects/open', json={'path': str(root)}).json()
        self.assertFalse(project['managed'])
        opened = self.client.post('/api/projects/' + project['id'] + '/sessions/open', json={'path': 'original.lattice'})
        self.assertEqual(opened.status_code, 200, opened.text)
        session = opened.json()
        canonical = root / 'sessions' / session_filename(session['title'])
        job = self.client.post('/api/sessions/' + session['id'] + '/messages', json={'text': 'hello', 'model': 'test-config'}).json()
        self.assertEqual(self.wait(job['id'])['status'], 'completed')
        self.assertEqual([message['text'] for message in lattice.decode(canonical.read_bytes())['messages']], ['hello', 'Reply to hello'])
        self.assertEqual(lattice.decode(path.read_bytes())['messages'], [])
        self.assertEqual(self.client.get('/api/sessions/' + session['id']).json()['saved_path'], str(canonical))
        self.assertEqual(set(root.iterdir()), {path, root / 'sessions', root / '.memory'})
        self.assertIsNone(self.client.get('/api/sessions/' + seed['id']).json()['saved_path'])

    def test_session_file_open_rejects_missing_corrupt_large_and_escaping_paths(self):
        project = self.client.post('/api/projects', json={'name': 'Validation'}).json()
        root = Path(project['path'])
        url = '/api/projects/' + project['id'] + '/sessions/open'
        seed = lattice.encode(self.session())
        corrupt = seed[:-1] + bytes([seed[-1] ^ 1])
        (root / 'corrupt.lattice').write_bytes(corrupt)
        (root / 'notes.txt').write_text('ordinary file', encoding='utf-8')
        with (root / 'large.lattice').open('wb') as file:
            file.truncate(lattice.MAX_FILE + 1)
        cases = [('missing.lattice', 404), ('corrupt.lattice', 422), ('large.lattice', 422), ('notes.txt', 422), ('../escape.lattice', 403), (str(root / 'corrupt.lattice'), 403), ('files/../../escape.lattice', 403)]
        for path, status in cases:
            with self.subTest(path=path):
                response = self.client.post(url, json={'path': path})
                self.assertEqual(response.status_code, status, response.text)
        self.assertEqual(len(self.client.get('/api/sessions').json()), 1)

    def test_autosave_failure_keeps_completed_conversation_and_can_retry(self):
        session = self.session()
        saved = self.client.post('/api/sessions/' + session['id'] + '/save').json()
        path = Path(saved['saved_path'])
        before = path.read_bytes()
        with patch('backend.app.services.session_files.atomic_write', side_effect=PermissionError('Permission denied')):
            job = self.client.post('/api/sessions/' + session['id'] + '/messages', json={'text': 'hello', 'model': 'test-config'}).json()
            finished = self.wait(job['id'])
            self.assertEqual(finished['status'], 'completed')
            self.assertIn('could not be saved', finished['error_message'])
        current = self.client.get('/api/sessions/' + session['id']).json()
        self.assertEqual([message['text'] for message in current['messages']], ['hello', 'Reply to hello'])
        self.assertIsNone(current['pending_job'])
        self.assertIn('could not be saved', current['save_error'])
        self.assertEqual(path.read_bytes(), before)
        retry = self.client.post('/api/sessions/' + session['id'] + '/save')
        self.assertEqual(retry.status_code, 200, retry.text)
        self.assertIsNone(retry.json()['save_error'])
        self.assertEqual(len(lattice.decode(path.read_bytes())['messages']), 2)

    def test_session_save_never_overwrites_collision_or_external_changes(self):
        session = self.session()
        collision = Path(self.fixture_project['path']) / 'sessions' / session_filename(session['title'])
        Path(session['saved_path']).unlink()
        self.app.state.session_files.update(session['id'], lambda current: current.update(saved_path=None, file_revision=None, file_title_stem=None))
        unrelated = lattice.encode({**session, 'title': 'Unrelated on disk'})
        collision.write_bytes(unrelated)
        response = self.client.post('/api/sessions/' + session['id'] + '/save')
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(Path(response.json()['saved_path']).name, 'Conversation (2).lattice')
        self.assertEqual(collision.read_bytes(), unrelated)
        collision.unlink()
        saved = self.client.post('/api/sessions/' + session['id'] + '/save').json()
        path = Path(saved['saved_path'])
        changed = lattice.encode({**session, 'title': 'External replacement'})
        path.write_bytes(changed)
        response = self.client.post('/api/sessions/' + session['id'] + '/save')
        self.assertEqual(response.status_code, 409, response.text)
        self.assertEqual(path.read_bytes(), changed)
        self.assertIn('changed outside', self.client.get('/api/sessions/' + session['id']).json()['save_error'])

    def test_save_during_response_and_cancel_preserves_committed_file(self):
        session = self.session()
        url = '/api/sessions/' + session['id']
        path = Path(self.client.post(url + '/save').json()['saved_path'])
        job = self.client.post(url + '/messages', json={'text': 'slow', 'model': 'test-config'}).json()
        self.client.patch(url, json={'draft': 'newer draft'})
        response = self.client.post(url + '/save')
        self.assertEqual(response.status_code, 200, response.text)
        partial = lattice.decode(path.read_bytes())
        self.assertEqual(partial['messages'], [])
        self.assertEqual(partial['draft'], 'slow\nnewer draft')
        self.assertEqual(len(self.client.get(url).json()['messages']), 1)
        self.client.post('/api/jobs/' + job['id'] + '/discard')
        restored = lattice.decode(path.read_bytes())
        self.assertEqual(restored['messages'], [])
        self.assertEqual(restored['draft'], 'slow\nnewer draft')
        self.assertEqual(self.client.get(url).json()['saved_path'], str(path))

    def test_session_file_writes_serialize_draft_updates_and_recovery(self):
        session = self.session()
        url = '/api/sessions/' + session['id']
        path = Path(self.client.post(url + '/save').json()['saved_path'])
        entered, release = threading.Event(), threading.Event()
        from backend.app.services.workspace import atomic_write
        def blocked_write(target, data):
            entered.set()
            if not release.wait(3):
                raise TimeoutError('Test did not release the file write')
            atomic_write(target, data)
        with ThreadPoolExecutor(max_workers=2) as workers:
            with patch('backend.app.services.session_files.atomic_write', side_effect=blocked_write):
                saving = workers.submit(self.client.post, url + '/save')
                self.assertTrue(entered.wait(2))
                patching = workers.submit(self.client.patch, url, json={'draft': 'new draft'})
                time.sleep(.04)
                self.assertFalse(patching.done(), 'Draft must wait until this file write commits')
                release.set()
                self.assertEqual(saving.result(timeout=3).status_code, 200)
                self.assertEqual(patching.result(timeout=3).status_code, 200)
        self.client.post(url + '/save')
        self.assertEqual(lattice.decode(path.read_bytes())['draft'], 'new draft')
        store, pipeline = self.app.state.store, self.app.state.pipeline
        job = pipeline.new(store.get('session', session['id']), 'chat', 'interrupted')
        self.app.state.session_files.update(session['id'], lambda current: current.update(
            pending_job=job['id'], messages=[{'role': 'user', 'author': 'You', 'time': 'now', 'text': 'interrupted', 'job_id': job['id']}]))
        pipeline.recover()
        restored = lattice.decode(path.read_bytes())
        self.assertEqual(restored['draft'], 'interrupted\nnew draft')
        self.assertEqual(restored['messages'], [])
        self.assertEqual(store.get('job', job['id'])['status'], 'failed')

    def test_new_project_sessions_are_saved_immediately_including_ordinary_folders(self):
        managed = self.client.post('/api/projects', json={'name': 'Immediate sessions'}).json()
        ordinary_path = self.settings.workspace_root / 'Ordinary folder'
        ordinary_path.mkdir()
        ordinary = self.client.post('/api/projects/open', json={'path': str(ordinary_path)}).json()
        for project in (managed, ordinary):
            with self.subTest(managed=project['managed']):
                session = self.session(project_id=project['id'])
                path = Path(project['path']) / 'sessions' / session_filename(session['title'])
                self.assertEqual(session['saved_path'], str(path))
                self.assertIsNone(session['save_error'])
                persisted = lattice.decode(path.read_bytes())
                self.assertEqual(persisted['title'], 'Conversation')
                self.assertEqual(persisted['project_path'], project['path'])
                self.assertEqual(persisted['messages'], [])

    def test_project_native_file_open_creates_saved_copy_and_upload_import_is_unsupported(self):
        project = self.client.post('/api/projects', json={'name': 'Imported project'}).json()
        seed = self.legacy_session(title='Import source')
        payload = lattice.encode({**seed, 'project_name': 'Embedded old project', 'project_path': '/not-a-source-file', 'draft': 'Imported draft'})
        source = Path(project['path']) / 'files' / 'imported.lattice'
        source.write_bytes(payload)
        response = self.client.post('/api/projects/' + project['id'] + '/sessions/open', json={'path': 'files/imported.lattice'})
        self.assertEqual(response.status_code, 200, response.text)
        imported = response.json()
        self.assertEqual(imported['project_id'], project['id'])
        self.assertEqual(imported['project_name'], project['name'])
        self.assertEqual(imported['source_path'], str(source))
        canonical = Path(project['path']) / 'sessions' / session_filename(imported['title'])
        self.assertEqual(imported['saved_path'], str(canonical))
        self.assertEqual(lattice.decode(canonical.read_bytes())['draft'], 'Imported draft')
        self.assertEqual(source.read_bytes(), payload)
        rejected = self.client.post('/api/sessions/import', files={'file': ('imported.lattice', payload)})
        self.assertEqual(rejected.status_code, 410)
        self.assertEqual(len(self.client.get('/api/sessions').json()), 1)

    def test_session_open_adopts_unassociated_records_and_never_transfers_other_project(self):
        first = self.client.post('/api/projects', json={'name': 'Adopt target'}).json()
        second = self.client.post('/api/projects', json={'name': 'Other target'}).json()
        session = self.legacy_session(title='Standalone')
        original = Path(self.app.state.session_files.save(session['id'])['saved_path'])
        source_bytes = original.read_bytes()
        self.app.state.session_files.update(session['id'], lambda current: current.update(draft='Current unsaved draft'))
        response = self.client.post('/api/sessions/' + session['id'] + '/open', json={'project_id': first['id']})
        self.assertEqual(response.status_code, 200, response.text)
        adopted = response.json()
        canonical = Path(first['path']) / 'sessions' / session_filename(session['title'])
        self.assertEqual(adopted['id'], session['id'])
        self.assertEqual(adopted['saved_path'], str(canonical))
        self.assertEqual(adopted['source_path'], str(original))
        self.assertEqual(lattice.decode(canonical.read_bytes())['draft'], 'Current unsaved draft')
        self.assertEqual(original.read_bytes(), source_bytes)
        self.assertEqual(self.client.post('/api/sessions/' + session['id'] + '/open').json()['id'], session['id'])
        rejected = self.client.post('/api/sessions/' + session['id'] + '/open', json={'project_id': second['id']})
        self.assertEqual(rejected.status_code, 409)
        self.assertEqual(self.client.get('/api/sessions/' + session['id']).json()['project_id'], first['id'])
        self.assertEqual(list((Path(second['path']) / 'sessions').iterdir()), [])

    def test_open_legacy_project_file_migrates_live_record_without_changing_source(self):
        project = self.client.post('/api/projects', json={'name': 'Legacy project'}).json()
        session = self.legacy_session(title='Legacy live record')
        source = Path(project['path']) / 'files' / 'old.lattice'
        source.write_bytes(lattice.encode(session))
        source_bytes = source.read_bytes()
        self.app.state.session_files.update(session['id'], lambda current: current.update(
            project_id=project['id'], project_name=project['name'], project_path=project['path'],
            saved_path=str(source), file_revision=hashlib.sha256(source_bytes).hexdigest(), draft='New live draft'))
        opened = self.client.post('/api/sessions/' + session['id'] + '/open').json()
        canonical = Path(project['path']) / 'sessions' / session_filename(session['title'])
        self.assertEqual(opened['saved_path'], str(canonical))
        self.assertEqual(source.read_bytes(), source_bytes)
        self.assertEqual(lattice.decode(canonical.read_bytes())['draft'], 'New live draft')
        self.client.patch('/api/sessions/' + session['id'], json={'draft': 'Even newer live draft'})
        reopened = self.client.post('/api/projects/' + project['id'] + '/sessions/open', json={'path': 'files/old.lattice'}).json()
        self.assertEqual(reopened['id'], session['id'])
        self.assertEqual(reopened['draft'], 'Even newer live draft')
        self.assertEqual(lattice.decode(canonical.read_bytes())['draft'], 'Even newer live draft')
        self.assertEqual(len(self.client.get('/api/sessions').json()), 1)

    def test_legacy_copy_uses_saved_file_revision_after_prior_source_edits(self):
        project = self.client.post('/api/projects', json={'name': 'Legacy revisions'}).json()
        session = self.legacy_session(title='Legacy edited session')
        source = Path(project['path']) / 'files' / 'edited.lattice'
        original = lattice.encode(session)
        edited = lattice.encode({**session, 'draft': 'Last file edit'})
        source.write_bytes(edited)
        self.app.state.session_files.update(session['id'], lambda current: current.update(
            project_id=project['id'], project_name=project['name'], project_path=project['path'],
            saved_path=str(source), source_path=str(source),
            file_revision=hashlib.sha256(edited).hexdigest(), source_revision=hashlib.sha256(original).hexdigest(),
            draft='Current live draft'))
        opened = self.client.post('/api/sessions/' + session['id'] + '/open').json()
        self.assertIsNone(opened['save_error'])
        self.assertEqual(source.read_bytes(), edited)
        self.assertEqual(lattice.decode(Path(opened['saved_path']).read_bytes())['draft'], 'Current live draft')
        reopened = self.client.post('/api/projects/' + project['id'] + '/sessions/open', json={'path': 'files/edited.lattice'})
        self.assertEqual(reopened.status_code, 200, reopened.text)
        self.assertEqual(reopened.json()['id'], session['id'])

    def test_existing_sessions_folder_file_uses_title_name(self):
        project = self.client.post('/api/projects', json={'name': 'Named file'}).json()
        seed = self.session(title='Existing named session')
        path = Path(project['path']) / 'sessions' / 'review.lattice'
        path.write_bytes(lattice.encode(seed))
        opened = self.client.post('/api/projects/' + project['id'] + '/sessions/open', json={'path': 'sessions/review.lattice'}).json()
        previous = path
        path = path.parent / session_filename(seed['title'])
        self.assertEqual(opened['saved_path'], str(path))
        self.assertFalse(previous.exists())
        self.assertNotIn('source_path', opened)
        self.client.patch('/api/sessions/' + opened['id'], json={'draft': 'Changed named file'})
        reopened = self.client.post('/api/sessions/' + opened['id'] + '/open').json()
        self.assertEqual(reopened['saved_path'], str(path))
        self.assertEqual(lattice.decode(path.read_bytes())['draft'], 'Changed named file')
        self.assertEqual(list(path.parent.iterdir()), [path])

    def test_project_initial_save_failure_returns_id_and_retries_without_duplicate(self):
        project = self.client.post('/api/projects', json={'name': 'Initial failure'}).json()
        with patch('backend.app.services.session_files.Path.mkdir', side_effect=PermissionError('Denied')):
            session = self.session(project_id=project['id'])
        self.assertIsNone(session['saved_path'])
        self.assertIn('could not be saved', session['save_error'])
        retried = self.client.post('/api/sessions/' + session['id'] + '/open').json()
        self.assertEqual(retried['id'], session['id'])
        self.assertIsNone(retried['save_error'])
        self.assertTrue(Path(retried['saved_path']).is_file())
        self.assertEqual(len(self.client.get('/api/sessions').json()), 1)

    def test_failed_source_copy_deduplicates_reopens_and_leaves_source_intact(self):
        project = self.client.post('/api/projects', json={'name': 'Source failure'}).json()
        source = Path(project['path']) / 'files' / 'source.lattice'
        source.write_bytes(lattice.encode({'title': 'Copy source', 'messages': []}))
        before = source.read_bytes()
        url = '/api/projects/' + project['id'] + '/sessions/open'
        with patch('backend.app.services.session_files.Path.mkdir', side_effect=PermissionError('Denied')):
            first = self.client.post(url, json={'path': 'files/source.lattice'}).json()
            second = self.client.post(url, json={'path': 'files/source.lattice'}).json()
        self.assertEqual(first['id'], second['id'])
        self.assertIsNone(first['saved_path'])
        self.assertIn('could not be saved', first['save_error'])
        retried = self.client.post(url, json={'path': 'files/source.lattice'}).json()
        self.assertEqual(retried['id'], first['id'])
        self.assertIsNone(retried['save_error'])
        self.assertTrue(Path(retried['saved_path']).is_file())
        self.assertEqual(source.read_bytes(), before)
        self.assertEqual(len(self.client.get('/api/sessions').json()), 1)

    def test_canonical_collision_and_external_source_changes_are_not_overwritten(self):
        project = self.client.post('/api/projects', json={'name': 'Copy conflicts'}).json()
        collision_id = '00000000-0000-4000-8000-000000000001'
        collision = Path(project['path']) / 'sessions' / 'Wanted session.lattice'
        collision_bytes = lattice.encode({'title': 'Unrelated file', 'messages': []})
        collision.write_bytes(collision_bytes)
        with patch('backend.app.services.session_files.uuid4', return_value=collision_id):
            saved = self.session(title='Wanted session', project_id=project['id'])
        self.assertEqual(Path(saved['saved_path']).name, 'Wanted session (2).lattice')
        self.assertIsNone(saved['save_error'])
        self.assertEqual(collision.read_bytes(), collision_bytes)
        retry = self.client.post('/api/sessions/' + saved['id'] + '/open').json()
        self.assertEqual(retry['id'], collision_id)
        self.assertEqual(retry['saved_path'], saved['saved_path'])
        self.assertIsNone(retry['save_error'])
        self.assertEqual(collision.read_bytes(), collision_bytes)
        source = Path(project['path']) / 'files' / 'source.lattice'
        source.write_bytes(lattice.encode({'title': 'Source', 'messages': []}))
        url = '/api/projects/' + project['id'] + '/sessions/open'
        opened = self.client.post(url, json={'path': 'files/source.lattice'}).json()
        canonical = Path(opened['saved_path'])
        before = canonical.read_bytes()
        source.write_bytes(lattice.encode({'title': 'Externally changed source', 'messages': []}))
        rejected = self.client.post(url, json={'path': 'files/source.lattice'})
        self.assertEqual(rejected.status_code, 409)
        self.assertEqual(canonical.read_bytes(), before)
        changed_destination = lattice.encode({'title': 'Externally changed destination', 'messages': []})
        canonical.write_bytes(changed_destination)
        failed_save = self.client.post('/api/sessions/' + opened['id'] + '/save')
        self.assertEqual(failed_save.status_code, 409)
        self.assertEqual(canonical.read_bytes(), changed_destination)

    def test_source_dedup_rejects_other_project_association(self):
        project = self.client.post('/api/projects', json={'name': 'Association owner'}).json()
        source = Path(project['path']) / 'files' / 'source.lattice'
        source.write_bytes(lattice.encode({'title': 'Owned source', 'messages': []}))
        opened = self.client.post('/api/projects/' + project['id'] + '/sessions/open', json={'path': 'files/source.lattice'}).json()
        nested = self.client.post('/api/projects/open', json={'path': str(source.parent)}).json()
        rejected = self.client.post('/api/projects/' + nested['id'] + '/sessions/open', json={'path': 'source.lattice'})
        self.assertEqual(rejected.status_code, 409)
        self.assertEqual(self.client.get('/api/sessions/' + opened['id']).json()['project_id'], project['id'])
        self.assertFalse((source.parent / 'sessions').exists())

    def test_completion_ownership_and_zip(self):
        session, other = self.session(), self.session(title='Other')
        response = self.client.post('/api/sessions/' + session['id'] + '/messages', json={'text': 'hello', 'model': 'test-config'})
        self.assertEqual(response.status_code, 202, response.text)
        job = self.wait(response.json()['id'])
        self.assertEqual(job['status'], 'completed')
        self.assertEqual(self.client.get('/api/sessions/' + other['id']).json()['messages'], [])
        messages = self.client.get('/api/sessions/' + session['id']).json()['messages']
        self.assertEqual([m['text'] for m in messages], ['hello', 'Reply to hello'])
        archive = self.client.get('/api/jobs/' + job['id'] + '/download-zip')
        self.assertEqual(archive.status_code, 200)
        with zipfile.ZipFile(io.BytesIO(archive.content)) as zip:
            self.assertEqual(set(zip.namelist()), {'session.lattice', 'conversation.md', 'session.json', 'logs.txt'})
            self.assertEqual(lattice.decode(zip.read('session.lattice'))['messages'][1]['text'], 'Reply to hello')
        self.assertNotIn('temp_dir', job)
        stored = self.app.state.store.get('job', job['id'])
        self.assertTrue(Path(stored['temp_dir']).is_relative_to(Path(tempfile.gettempdir()).resolve()))

    def test_cancel_restores_draft_and_preserves_new_text(self):
        session = self.session()
        url = '/api/sessions/' + session['id']
        job = self.client.post(url + '/messages', json={'text': 'slow', 'model': 'test-config'}).json()
        duplicate = self.client.post(url + '/messages', json={'text': 'duplicate', 'model': 'test-config'})
        self.assertEqual(duplicate.status_code, 409)
        self.client.patch(url, json={'draft': 'newer draft'})
        result = self.client.post('/api/jobs/' + job['id'] + '/discard')
        self.assertEqual(result.json()['status'], 'discarded')
        restored = self.client.get(url).json()
        self.assertEqual(restored['draft'], 'slow\nnewer draft')
        self.assertEqual(restored['messages'], [])
        self.assertIsNone(restored['pending_job'])
        stored = self.app.state.store.get('job', job['id'])
        self.assertFalse(Path(stored['temp_dir']).exists())

    def test_failures_and_tools_are_not_executed(self):
        for text in ('failure', 'tools'):
            session = self.session()
            job = self.client.post('/api/sessions/' + session['id'] + '/messages', json={'text': text, 'model': 'test-config'}).json()
            finished = self.wait(job['id'])
            self.assertEqual(finished['status'], 'failed')
            self.assertNotIn('secret value', finished['error_message'])
            self.assertEqual(self.client.get('/api/sessions/' + session['id']).json()['draft'], text)

    def test_settings_test_does_not_save_and_origin_rejected(self):
        before = self.client.get('/api/settings').json()
        self.assertEqual(self.client.post('/api/settings/test', json={'server_url': 'http://localhost:9000', 'font_size': 16}).status_code, 200)
        self.assertEqual(self.client.get('/api/settings').json(), before)
        self.assertEqual(self.client.post('/api/settings', json={'server_url': 'http://example.com', 'font_size': 14}).status_code, 422)
        self.assertEqual(self.client.post('/api/sessions', json={}, headers={'Origin': 'https://evil.example'}).status_code, 403)

    def test_resource_preferences_and_order(self):
        project = self.client.post('/api/projects', json={'name': 'Resources'}).json()
        path = Path(project['path'])
        for folder in ('skills/a', 'agents/a', 'agents/b', '.mcp/local'):
            (path / folder).mkdir(parents=True)
        url = '/api/projects/' + project['id'] + '/resources'
        entries = self.client.get(url).json()
        self.assertEqual(len(entries), 9)
        builtins = {'run_python_script', 'run_batch_script', 'create_tool_from_conversation'}
        self.assertEqual({r['name'] for r in entries if r['enabled']}, builtins)
        self.assertTrue(all(not r['enabled'] for r in entries if r['name'] not in builtins))
        changed = self.client.patch(url, json={'id': 'agents/b', 'enabled': True, 'direction': 'up', 'effort': 'max'}).json()
        self.assertEqual([r['name'] for r in changed if r['kind'] == 'agent'], ['b', 'a'])
        self.assertTrue(next(r for r in changed if r['id'] == 'agents/b')['enabled'])
        self.assertEqual(self.client.patch(url, json={'id': '.mcp/local', 'enabled': True}).status_code, 409)

    def test_reopening_existing_project_adds_missing_agent_skill_without_replacing_custom_skill(self):
        project = self.client.post('/api/projects', json={'name': 'Older tool project'}).json()
        root = Path(project['path'])
        custom = root / '.skill/run_python_script/SKILL.md'
        custom.write_text('Custom Python instructions', encoding='utf-8')
        manifest = root / '.skill/create_agent/SKILL.md'
        manifest.unlink()
        manifest.parent.rmdir()
        response = self.client.post('/api/projects/open', json={'path': str(root)})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertTrue(manifest.is_file())
        self.assertEqual(custom.read_text(), 'Custom Python instructions')
        entries = self.client.get('/api/projects/' + project['id'] + '/resources?effort=max').json()
        self.assertFalse(next(item for item in entries if item['name'] == 'create_agent')['enabled'])
        self.assertFalse(next(item for item in entries if item['name'] == 'run_agent')['enabled'])

    def test_restart_recovery_and_ttl(self):
        session = self.session()
        store, pipeline = self.app.state.store, self.app.state.pipeline
        job = pipeline.new(session, 'chat', 'interrupted')
        store.update('session', session['id'], lambda s: s.update(pending_job=job['id'], messages=[{'role': 'user', 'author': 'You', 'time': 'now', 'text': 'interrupted', 'job_id': job['id']}]))
        pipeline.recover()
        self.assertEqual(store.get('session', session['id'])['draft'], 'interrupted')
        self.assertEqual(store.get('job', job['id'])['status'], 'failed')
        export = self.client.post('/api/sessions/' + session['id'] + '/export').json()
        self.wait(export['id'])
        store.update('job', export['id'], lambda j: j.update(completed_at='2000-01-01T00:00:00+00:00'))
        pipeline.cleanup()
        self.assertFalse(store.get('job', export['id'])['download_available'])
        self.assertEqual(self.client.get('/api/jobs/' + export['id'] + '/download-zip').status_code, 409)

    def test_temp_manager_never_deletes_unowned_path(self):
        with self.assertRaises(ValueError):
            self.app.state.pipeline.temp.purge(self.directory.name)
        self.assertTrue(Path(self.directory.name).exists())

class LatticeTests(unittest.TestCase):
    def test_fnv_reference_vectors_and_size_limits(self):
        self.assertEqual(lattice.checksum(b''), 0xcbf29ce484222325)
        self.assertEqual(lattice.checksum(b'hello'), 0xa430d84680aabd0b)
        with self.assertRaises(ValueError):
            lattice.decode(b'not a native session')
        with self.assertRaises(ValueError):
            lattice.encode({'title': 'x', 'messages': [], 'attachments': [''] * 10001})

    def test_file_browser_defaults_to_os_account_access(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            with patch.dict('os.environ'):
                os.environ.pop('FILE_BROWSER_ROOTS', None)
                self.assertIsNone(Settings(data_dir=root).browser_roots)
                for value in ('', '  ', '*', ' * '):
                    os.environ['FILE_BROWSER_ROOTS'] = value
                    self.assertIsNone(Settings(data_dir=root).browser_roots)

    def test_file_browser_roots_configuration(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            with patch.dict('os.environ', {'FILE_BROWSER_ROOTS': str(root) + os.pathsep + str(root / 'Projects')}):
                settings = Settings(data_dir=root)
            self.assertEqual(settings.browser_roots, (root, root / 'Projects'))
            for roots in ([], [Path('relative')]):
                with self.assertRaises(ValueError):
                    Settings(data_dir=root, browser_roots=roots)

if __name__ == '__main__':
    unittest.main()
