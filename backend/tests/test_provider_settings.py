"""File-authoritative project timeout/retry policy and legacy migration."""
import math
import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch
import json

import httpx
from fastapi.testclient import TestClient
from pydantic import ValidationError

from backend.app.config import Settings
from backend.app.main import create_app
from backend.app.schemas.studio import ProjectPreferencesPatch
from backend.app.services import lattice
from backend.app.services.connector import ConnectorTimeout


class ProviderSettingsTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='studio-provider-settings-test-')
        self.root = Path(self.directory.name).resolve()
        self.settings = Settings(data_dir=self.root / 'data', workspace_root=self.root / 'projects', browser_roots=[self.root])
        self.requests = []
        def models(request):
            self.requests.append(request)
            return httpx.Response(200, json={'data': [{'id': 'test-config'}]})
        self.transport = httpx.MockTransport(models)
        self.app = create_app(self.settings, self.transport)
        self.context = TestClient(self.app)
        self.client = self.context.__enter__()

    def tearDown(self):
        self.context.__exit__(None, None, None)
        for path in self.app.state.pipeline.temp.root.glob('job-*'):
            self.app.state.pipeline.temp.purge(path)
        self.directory.cleanup()

    def restart(self, data_dir=None):
        self.context.__exit__(None, None, None)
        if data_dir:
            self.settings = Settings(data_dir=data_dir, workspace_root=self.settings.workspace_root, browser_roots=[self.root])
        self.app = create_app(self.settings, self.transport)
        self.context = TestClient(self.app)
        self.client = self.context.__enter__()

    def project(self, name='Provider policy project'):
        response = self.client.post('/api/projects', json={'name': name})
        self.assertEqual(response.status_code, 201, response.text)
        return response.json()

    def preferences(self, project):
        return self.client.get('/api/projects/' + project['id'] + '/preferences').json()

    def set_policy(self, project, **fields):
        response = self.client.patch('/api/projects/' + project['id'] + '/preferences', json=fields)
        self.assertEqual(response.status_code, 200, response.text)
        return response.json()

    def test_unlimited_defaults_are_project_scoped_and_custom_policy_survives_restart_and_portable_reopen(self):
        project, other = self.project(), self.project('Independent policy project')
        self.assertEqual(self.preferences(project)['provider_response_timeout'], -1)
        self.assertEqual(self.preferences(project)['provider_timeout_retries'], 2)
        self.set_policy(project, provider_response_timeout=100000.5, provider_timeout_retries=10)
        self.app.state.project_memory.set_default_model(project['id'], 'test-config')
        file = Path(project['path']) / '.memory/preferences.json'
        saved = json.loads(file.read_text(encoding='utf-8'))
        self.assertEqual(saved['provider_response_timeout'], 100000.5)
        self.assertEqual(saved['default_model'], 'test-config')
        self.assertEqual(self.preferences(other)['provider_response_timeout'], -1)
        self.restart()
        self.assertEqual(self.preferences(project)['provider_response_timeout'], 100000.5)
        self.restart(self.root / 'portable-data')
        reopened = self.client.post('/api/projects/open', json={'path': project['path']}).json()
        self.assertEqual(self.preferences(reopened)['provider_timeout_retries'], 10)
        self.assertEqual(self.preferences(reopened)['provider_response_timeout'], 100000.5)
        self.assertEqual(self.preferences(reopened)['default_model'], 'test-config')
        self.assertEqual(json.loads(file.read_text(encoding='utf-8')), saved)

    def test_partial_policy_updates_preserve_model_and_other_policy_fields(self):
        project = self.project()
        memory = self.app.state.project_memory
        memory.set_default_model(project['id'], 'test-config')
        self.set_policy(project, provider_response_timeout=0, provider_timeout_retries=7)
        timeout = self.set_policy(project, provider_response_timeout=0.125)
        self.assertEqual(timeout['provider_timeout_retries'], 7)
        self.assertEqual(timeout['default_model'], 'test-config')
        retries = self.set_policy(project, provider_timeout_retries=0)
        self.assertEqual(retries['provider_response_timeout'], 0.125)
        memory.set_default_model(project['id'], 'test-config-alt')
        current = self.preferences(project)
        self.assertEqual(current['default_model'], 'test-config-alt')
        self.assertEqual(current['provider_timeout_retries'], 0)
        self.assertEqual(current['provider_response_timeout'], 0.125)
        self.assertEqual(self.set_policy(project, provider_response_timeout=-1)['provider_response_timeout'], -1)

    def test_existing_catalog_migrates_former_global_policy_once_without_overwriting_project_fields(self):
        legacy, custom = self.project(), self.project('Custom existing policy')
        self.set_policy(custom, provider_response_timeout=0.5, provider_timeout_retries=9)
        for project in (legacy, custom):
            self.app.state.store.update('project', project['id'], lambda current: current.pop('provider_policy_initialized', None))
        legacy_file = Path(legacy['path']) / '.memory/preferences.json'
        legacy_file.write_text(json.dumps({'version': 1, 'default_model': 'test-config'}), encoding='utf-8')
        connection = self.app.state.store.get('settings', 'connection')
        self.app.state.store.put('settings', {**connection, 'provider_response_timeout': 45.5, 'provider_timeout_retries': 4})
        with self.app.state.store.connection() as db:
            db.execute('DELETE FROM documents WHERE kind=? AND id=?', ('settings', 'project_provider_policy_migration'))
        self.restart()
        self.assertEqual(self.preferences(legacy)['provider_response_timeout'], 45.5)
        self.assertEqual(self.preferences(legacy)['provider_timeout_retries'], 4)
        self.assertEqual(self.preferences(legacy)['default_model'], 'test-config')
        self.assertEqual(self.preferences(custom)['provider_response_timeout'], 0.5)
        self.assertEqual(self.preferences(custom)['provider_timeout_retries'], 9)
        self.assertEqual(self.preferences(self.project('New after migration'))['provider_response_timeout'], -1)
        self.client.post('/api/settings', json={'server_url': connection['server_url'], 'font_size': 17}).raise_for_status()
        self.restart()
        self.assertEqual(self.preferences(legacy)['provider_response_timeout'], 45.5)
        self.assertEqual(self.preferences(custom)['provider_response_timeout'], 0.5)

    def test_unavailable_legacy_project_keeps_migration_policy_after_global_settings_are_saved(self):
        project = self.project('Temporarily unavailable legacy project')
        folder = Path(project['path'])
        moved = self.root / 'Moved legacy folder'
        self.assertTrue(folder.resolve().is_relative_to(self.root))
        self.assertTrue(moved.resolve().is_relative_to(self.root))
        self.app.state.store.update('project', project['id'], lambda current: current.pop('provider_policy_initialized', None))
        (folder / '.memory/preferences.json').write_text(json.dumps({'version': 1, 'default_model': 'test-config'}), encoding='utf-8')
        connection = self.app.state.store.get('settings', 'connection')
        self.app.state.store.put('settings', {**connection, 'provider_response_timeout': 75, 'provider_timeout_retries': 6})
        with self.app.state.store.connection() as db:
            db.execute('DELETE FROM documents WHERE kind=? AND id=?', ('settings', 'project_provider_policy_migration'))
        folder.rename(moved)
        self.restart()
        self.client.post('/api/settings', json={'server_url': connection['server_url'], 'font_size': 16}).raise_for_status()
        moved.rename(folder)
        response = self.client.post('/api/projects/open', json={'path': project['path']})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(self.preferences(project)['provider_response_timeout'], 75)
        self.assertEqual(self.preferences(project)['provider_timeout_retries'], 6)
        self.assertEqual(self.preferences(project)['default_model'], 'test-config')

    def test_policy_validation_rejects_other_negatives_non_finite_and_non_integer_retries_without_overwrite(self):
        project = self.project()
        file = Path(project['path']) / '.memory/preferences.json'
        before = file.read_bytes()
        invalid = [('provider_response_timeout', value) for value in (-2, -0.01, 10 ** 400, True, '120', None)]
        invalid += [('provider_timeout_retries', value) for value in (-1, 11, True, 1.5, '2', None)]
        for key, value in invalid:
            with self.subTest(key=key, value=value):
                response = self.client.patch('/api/projects/' + project['id'] + '/preferences', json={key: value})
                self.assertEqual(response.status_code, 422, response.text)
        for value in ('NaN', 'Infinity', '-Infinity'):
            response = self.client.patch('/api/projects/' + project['id'] + '/preferences', content='{"provider_response_timeout":' + value + '}', headers={'Content-Type': 'application/json'})
            self.assertEqual(response.status_code, 422, response.text)
        self.assertEqual(file.read_bytes(), before)
        for value in (math.nan, math.inf, -math.inf):
            with self.assertRaises(ValidationError):
                ProjectPreferencesPatch(provider_response_timeout=value)

    def test_discovery_uses_project_policy_and_test_uses_unsaved_override_without_persisting(self):
        project = self.project()
        self.set_policy(project, provider_response_timeout=12.5, provider_timeout_retries=4)
        file = Path(project['path']) / '.memory/preferences.json'
        before = file.read_bytes()
        with patch.object(self.app.state.connector, 'models', new=AsyncMock(return_value=[])) as models:
            self.client.get('/api/models', params={'project_id': project['id']}).raise_for_status()
            models.assert_awaited_once_with('http://127.0.0.1:8301/v1', response_timeout=12.5)
            models.reset_mock()
            self.client.get('/api/models').raise_for_status()
            models.assert_awaited_once_with('http://127.0.0.1:8301/v1', response_timeout=-1)
            models.reset_mock()
            self.client.post('/api/settings/test', json={'server_url': 'http://localhost:9000', 'project_id': project['id'], 'provider_response_timeout': -1}).raise_for_status()
            models.assert_awaited_once_with('http://localhost:9000/v1', response_timeout=-1)
            models.reset_mock()
            self.client.post('/api/settings/test', json={'server_url': 'http://localhost:9000', 'project_id': project['id']}).raise_for_status()
            models.assert_awaited_once_with('http://localhost:9000/v1', response_timeout=12.5)
        self.assertEqual(file.read_bytes(), before)
        self.assertNotIn('provider_response_timeout', self.client.get('/api/settings').json())
        self.assertEqual(self.client.post('/api/settings', json={'server_url': 'http://localhost:8301', 'provider_response_timeout': 5}).status_code, 422)

    def test_connector_discovery_applies_unlimited_and_custom_timeouts_once_without_retries(self):
        self.client.get('/api/models').raise_for_status()
        self.assertEqual(len(self.requests), 1)
        self.assertTrue(all(value is None for value in self.requests[0].extensions['timeout'].values()))
        project = self.project()
        self.set_policy(project, provider_response_timeout=7200)
        self.client.get('/api/models', params={'project_id': project['id']}).raise_for_status()
        self.assertEqual(len(self.requests), 2)
        self.assertEqual(self.requests[1].extensions['timeout']['read'], 7200)
        with patch.object(self.app.state.connector, 'models', new=AsyncMock(side_effect=ConnectorTimeout('Connector request timed out.'))):
            self.assertEqual(self.client.get('/api/models').status_code, 502)
            self.assertEqual(self.client.post('/api/settings/test', json={'server_url': 'http://localhost:8301', 'project_id': project['id']}).status_code, 502)

    def test_missing_corrupt_or_unwritable_preferences_block_tasks_before_job_allocation(self):
        project = self.project()
        session = self.client.post('/api/sessions', json={'title': 'Guarded task', 'project_id': project['id']}).json()
        file = Path(project['path']) / '.memory/preferences.json'
        original = file.read_bytes()
        url = '/api/sessions/' + session['id'] + '/messages'
        before_temp = set(self.app.state.pipeline.temp.root.glob('job-*'))
        for invalid in (None, b'corrupt preferences', b'{"version":1,"default_model":"test-config"}'):
            if invalid is None:
                file.unlink()
            else:
                file.write_bytes(invalid)
            response = self.client.post(url, json={'text': 'Task must not start', 'model': 'test-config'})
            self.assertIn(response.status_code, (409, 422), response.text)
            self.assertEqual(self.app.state.store.list('job'), [])
            self.assertEqual(set(self.app.state.pipeline.temp.root.glob('job-*')), before_temp)
            self.assertIsNone(self.app.state.store.get('session', session['id'])['pending_job'])
            self.assertEqual(self.app.state.store.get('session', session['id'])['messages'], [])
            file.write_bytes(original)
        with patch('backend.app.services.project_memory.atomic_write', side_effect=PermissionError('Fixture denied preferences write')):
            response = self.client.post(url, json={'text': 'Unwritable task', 'model': 'test-config'})
            self.assertEqual(response.status_code, 409, response.text)
        self.assertEqual(self.app.state.store.list('job'), [])
        self.assertEqual(file.read_bytes(), original)
        file.unlink()
        self.restart()
        self.assertFalse(file.exists(), 'Initialized missing preference files must not silently recreate on restart')
        self.assertTrue(self.preferences(project)['error'])
        self.client.post('/api/projects/open', json={'path': project['path']}).raise_for_status()
        self.assertFalse(file.exists(), 'Reopening an initialized project must preserve a missing preference file')

    def test_opened_retrying_native_snapshot_is_cancelled_with_public_retry_details_preserved(self):
        project = self.project()
        state = {'phase': 'retrying', 'iteration': 2, 'retry_attempt': 1, 'retry_limit': 3, 'provider_response_timeout': -1}
        source = Path(project['path']) / 'files' / 'retrying.lattice'
        source.write_bytes(lattice.encode({'title': 'Retrying snapshot', 'messages': [], 'execution_state': state}))
        response = self.client.post('/api/projects/' + project['id'] + '/sessions/open', json={'path': 'files/retrying.lattice'})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json()['execution_state'], {**state, 'phase': 'cancelled'})


class ProviderPolicyConfigTests(unittest.TestCase):
    def test_runtime_defaults_accept_unlimited_zero_and_uncapped_finite_seconds(self):
        with tempfile.TemporaryDirectory(prefix='studio-provider-config-test-') as directory:
            for timeout in (-1, 0, 0.125, 7200, 1e100):
                settings = Settings(data_dir=Path(directory), provider_response_timeout=timeout, provider_timeout_retries=3)
                self.assertEqual(settings.provider_policy()['provider_response_timeout'], timeout)
            for kwargs in ({'provider_response_timeout': -2}, {'provider_response_timeout': -0.1}, {'provider_response_timeout': 10 ** 400}, {'provider_response_timeout': math.inf}, {'provider_response_timeout': True}, {'provider_timeout_retries': -1}, {'provider_timeout_retries': 11}, {'provider_timeout_retries': False}, {'provider_timeout_retries': 1.5}):
                with self.subTest(kwargs=kwargs):
                    with self.assertRaises(ValueError):
                        Settings(data_dir=Path(directory), **kwargs)
