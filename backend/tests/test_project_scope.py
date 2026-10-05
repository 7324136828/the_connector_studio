"""Project-only public sessions and durable five-entry recent-project pointers."""
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import patch

from fastapi.testclient import TestClient

from backend.app.config import Settings
from backend.app.main import create_app
from backend.app.services import lattice
from backend.app.services.workspace import Workspace


class ProjectScopeTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='studio-project-scope-test-')
        self.root = Path(self.directory.name).resolve()
        self.settings = Settings(data_dir=self.root / 'data', workspace_root=self.root / 'projects', browser_roots=[self.root])
        self.app = create_app(self.settings)
        self.context = TestClient(self.app)
        self.client = self.context.__enter__()

    def tearDown(self):
        self.context.__exit__(None, None, None)
        for path in self.app.state.pipeline.temp.root.glob('job-*'):
            self.app.state.pipeline.temp.purge(path)
        self.directory.cleanup()

    def project(self, name='Project'):
        response = self.client.post('/api/projects', json={'name': name})
        self.assertEqual(response.status_code, 201, response.text)
        return response.json()

    def session(self, project, title='Scoped conversation'):
        response = self.client.post('/api/sessions', json={'title': title, 'project_id': project['id']})
        self.assertEqual(response.status_code, 201, response.text)
        return response.json()

    def recents(self):
        response = self.client.get('/api/projects')
        self.assertEqual(response.status_code, 200, response.text)
        return response.json()

    def test_public_creation_requires_existing_project_folder_without_creating_orphan_records(self):
        for body in ({'title': 'No project'}, {'title': 'Null project', 'project_id': None}, {'title': 'Empty project', 'project_id': ''}, {'title': 'Unknown project', 'project_id': 'unknown'}):
            response = self.client.post('/api/sessions', json=body)
            self.assertIn(response.status_code, (404, 422), response.text)
        self.assertEqual(self.app.state.store.list('session'), [])
        project = self.project()
        folder = Path(project['path'])
        moved = self.root / 'Temporarily moved project'
        self.assertTrue(folder.resolve().is_relative_to(self.root))
        self.assertTrue(moved.resolve().is_relative_to(self.root))
        folder.rename(moved)
        response = self.client.post('/api/sessions', json={'title': 'Missing folder', 'project_id': project['id']})
        self.assertEqual(response.status_code, 404, response.text)
        self.assertEqual(self.app.state.store.list('session'), [])
        self.assertFalse(folder.exists())

    def test_legacy_orphans_are_preserved_readable_hidden_and_blocked_until_explicit_project_adoption(self):
        files = self.app.state.session_files
        orphan = files.new('Legacy conversation')
        original = files.save(orphan['id'])
        before = Path(original['saved_path']).read_bytes()
        url = '/api/sessions/' + orphan['id']
        self.assertEqual(self.client.get(url).status_code, 200)
        self.assertEqual(self.client.get(url + '/download').status_code, 200)
        self.assertEqual(self.client.get('/api/sessions').json(), [])
        self.assertEqual(self.client.get('/api/sessions', params={'include_agents': True}).json(), [])
        for path, body in (('/messages', {'text': 'Cannot send yet', 'model': 'test-config'}), ('/save', None), ('/export', None), ('/open', None)):
            response = self.client.post(url + path, json=body)
            self.assertEqual(response.status_code, 409, response.text)
        self.assertEqual(self.app.state.store.list('job'), [])
        self.assertEqual(Path(original['saved_path']).read_bytes(), before)
        invalid = self.client.post(url + '/open', json={'project_id': 'missing'})
        self.assertEqual(invalid.status_code, 404)
        self.assertIsNone(self.app.state.store.get('session', orphan['id'])['project_id'])
        project = self.project('Adoption target')
        adopted = self.client.post(url + '/open', json={'project_id': project['id']})
        self.assertEqual(adopted.status_code, 200, adopted.text)
        current = adopted.json()
        self.assertEqual(current['id'], orphan['id'])
        self.assertEqual(current['project_id'], project['id'])
        self.assertEqual(current['source_path'], original['saved_path'])
        self.assertEqual(Path(original['saved_path']).read_bytes(), before)
        self.assertEqual([item['id'] for item in self.client.get('/api/sessions').json()], [orphan['id']])
        self.assertEqual(self.client.post(url + '/save').status_code, 200)

    def test_missing_project_blocks_open_save_messages_and_export_without_recreating_folder(self):
        project = self.project()
        session = self.session(project)
        folder = Path(project['path'])
        moved = self.root / 'Moved conversation project'
        self.assertTrue(folder.resolve().is_relative_to(self.root))
        self.assertTrue(moved.resolve().is_relative_to(self.root))
        folder.rename(moved)
        url = '/api/sessions/' + session['id']
        for suffix, body in (('/open', None), ('/save', None), ('/messages', {'text': 'Cannot send', 'model': 'test-config'}), ('/export', None)):
            response = self.client.post(url + suffix, json=body)
            self.assertEqual(response.status_code, 404, response.text)
        self.assertFalse(folder.exists())
        self.assertEqual(self.app.state.store.list('job'), [])
        self.assertEqual(self.app.state.store.get('session', session['id'])['project_id'], project['id'])

    def test_upload_import_is_unsupported_but_project_explorer_file_open_preserves_source(self):
        project = self.project()
        data = lattice.encode({'title': 'Project-native conversation', 'messages': []})
        for body in (None, {'project_id': project['id']}):
            response = self.client.post('/api/sessions/import', data=body, files={'file': ('native.lattice', data)})
            self.assertEqual(response.status_code, 410, response.text)
            self.assertIn('project explorer', response.json()['detail'])
        self.assertEqual(self.app.state.store.list('session'), [])
        source = Path(project['path']) / 'files' / 'native.lattice'
        source.write_bytes(data)
        response = self.client.post('/api/projects/' + project['id'] + '/sessions/open', json={'path': 'files/native.lattice'})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json()['project_id'], project['id'])
        self.assertEqual(source.read_bytes(), data)

    def test_recents_are_five_mru_pointers_and_eviction_preserves_catalog_sessions_files_and_id(self):
        projects = [self.project('Project ' + str(index)) for index in range(7)]
        session = self.session(projects[0])
        before = Path(session['saved_path']).read_bytes()
        self.app.state.store.update('project', projects[0]['id'], lambda current: current['preferences'].update({'fixture': {'enabled': True}}))
        self.assertEqual([item['id'] for item in self.recents()], [project['id'] for project in reversed(projects[2:])])
        self.assertEqual(len(self.app.state.store.list('project')), 7)
        self.assertEqual(self.app.state.store.get('session', session['id'])['project_id'], projects[0]['id'])
        self.assertEqual(Path(session['saved_path']).read_bytes(), before)
        response = self.client.post('/api/projects/open', json={'path': projects[0]['path']})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json()['id'], projects[0]['id'])
        self.assertEqual(response.json()['preferences']['fixture'], {'enabled': True})
        self.assertEqual([item['id'] for item in self.recents()], [projects[0]['id'], projects[6]['id'], projects[5]['id'], projects[4]['id'], projects[3]['id']])
        self.assertEqual(len(self.app.state.store.list('project')), 7)

    def test_tied_timestamps_never_reorder_open_events_and_recents_survive_restart(self):
        with patch('backend.app.services.workspace.now', return_value='2026-10-05T12:00:00Z'):
            first, second, third = [self.project(name) for name in ('First', 'Second', 'Third')]
            self.client.post('/api/projects/open', json={'path': first['path']}).raise_for_status()
            self.client.post('/api/projects/open', json={'path': second['path']}).raise_for_status()
        expected = [second['id'], first['id'], third['id']]
        self.assertEqual([item['id'] for item in self.recents()], expected)
        self.context.__exit__(None, None, None)
        self.app = create_app(self.settings)
        self.context = TestClient(self.app)
        self.client = self.context.__enter__()
        self.assertEqual([item['id'] for item in self.recents()], expected)

    def test_confirmed_missing_and_non_directory_paths_are_pruned_without_deleting_records(self):
        project = self.project()
        folder = Path(project['path'])
        moved = self.root / 'Pruned project data'
        self.assertTrue(folder.resolve().is_relative_to(self.root))
        self.assertTrue(moved.resolve().is_relative_to(self.root))
        folder.rename(moved)
        self.assertEqual(self.recents(), [])
        self.assertEqual(self.app.state.store.get('settings', 'recent_projects')['project_ids'], [])
        self.assertEqual(self.app.state.store.get('project', project['id'])['path'], project['path'])
        moved.rename(folder)
        self.assertEqual(self.recents(), [], 'Pruned project must not be seeded again when its folder reappears')
        reopened = self.client.post('/api/projects/open', json={'path': str(folder)})
        self.assertEqual(reopened.json()['id'], project['id'])
        folder.rename(moved)
        folder.write_text('A file replaced the project folder', encoding='utf-8')
        self.assertEqual(self.recents(), [])
        self.assertEqual(self.app.state.store.get('project', project['id'])['id'], project['id'])

    def test_permission_or_transient_stat_errors_keep_recent_project_pointers(self):
        project = self.project()
        candidate = Path(project['path'])
        original = Path.stat
        def denied(path, *args, **kwargs):
            if path == candidate:
                raise PermissionError('Temporary project access failure')
            return original(path, *args, **kwargs)
        with patch.object(Path, 'stat', denied):
            self.assertEqual([item['id'] for item in self.recents()], [project['id']])
        self.assertEqual(self.app.state.store.get('settings', 'recent_projects')['project_ids'], [project['id']])

    def test_legacy_catalog_migration_seeds_latest_existing_five_without_removing_catalog(self):
        projects = [self.project('Legacy ' + str(index)) for index in range(7)]
        with self.app.state.store.connection() as db:
            db.execute('DELETE FROM documents WHERE kind=? AND id=?', ('settings', 'recent_projects'))
        recents = self.recents()
        self.assertEqual([item['id'] for item in recents], [project['id'] for project in reversed(projects[2:])])
        self.assertEqual(len(self.app.state.store.list('project')), 7)

    def test_parallel_reopens_keep_stable_ids_and_at_most_five_unique_recent_pointers(self):
        projects = [self.project('Parallel ' + str(index)) for index in range(6)]
        workspace = self.app.state.session_files.workspace
        with ThreadPoolExecutor(max_workers=6) as workers:
            opened = list(workers.map(lambda project: workspace.open(project['path']), projects))
        self.assertEqual([item['id'] for item in opened], [item['id'] for item in projects])
        ids = [project['id'] for project in self.recents()]
        self.assertEqual(len(ids), 5)
        self.assertEqual(len(set(ids)), 5)
        self.assertEqual(len(self.app.state.store.list('project')), 6)
