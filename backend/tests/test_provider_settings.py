"""Persisted connection timeout settings and legacy-policy normalization."""
import math
import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch

import httpx
from fastapi.testclient import TestClient
from pydantic import ValidationError

from backend.app.config import Settings
from backend.app.main import create_app
from backend.app.schemas.studio import ConnectionInput
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

    def restart(self):
        self.context.__exit__(None, None, None)
        self.app = create_app(self.settings, self.transport)
        self.context = TestClient(self.app)
        self.client = self.context.__enter__()

    def test_new_defaults_are_persisted_and_saved_fractional_policy_survives_restart(self):
        current = self.client.get('/api/settings').json()
        self.assertEqual(current['provider_response_timeout'], 120)
        self.assertEqual(current['provider_timeout_retries'], 2)
        stored = self.app.state.store.get('settings', 'connection')
        self.assertEqual(stored['provider_response_timeout'], 120)
        self.assertEqual(stored['provider_timeout_retries'], 2)
        saved = self.client.post('/api/settings', json={'server_url': 'http://127.0.0.1:8301', 'font_size': 17, 'max_parallel_agents': 6, 'provider_response_timeout': 1.5, 'provider_timeout_retries': 10})
        self.assertEqual(saved.status_code, 200, saved.text)
        self.restart()
        restored = self.client.get('/api/settings').json()
        self.assertEqual(restored['provider_response_timeout'], 1.5)
        self.assertEqual(restored['provider_timeout_retries'], 10)
        self.assertEqual(restored['font_size'], 17)
        self.assertEqual(restored['max_parallel_agents'], 6)
        boundary = self.client.post('/api/settings', json={'server_url': restored['server_url'], 'provider_response_timeout': 3600, 'provider_timeout_retries': 0})
        self.assertEqual(boundary.status_code, 200, boundary.text)
        self.assertEqual(boundary.json()['provider_timeout_retries'], 0)

    def test_legacy_missing_or_invalid_policy_is_normalized_without_changing_other_connection_fields(self):
        legacy = {'id': 'connection', 'server_url': 'http://localhost:8301/v1', 'font_size': 18, 'max_parallel_agents': 7, 'legacy_metadata': 'retained'}
        self.app.state.store.put('settings', legacy)
        self.restart()
        normalized = self.client.get('/api/settings').json()
        self.assertEqual(normalized['provider_response_timeout'], 120)
        self.assertEqual(normalized['provider_timeout_retries'], 2)
        for key, value in legacy.items():
            self.assertEqual(normalized[key], value)
        self.app.state.store.put('settings', {**legacy, 'provider_response_timeout': False, 'provider_timeout_retries': 2.5})
        normalized = self.client.get('/api/settings').json()
        self.assertEqual(normalized['provider_response_timeout'], 120)
        self.assertEqual(normalized['provider_timeout_retries'], 2)
        stored = self.app.state.store.get('settings', 'connection')
        self.assertEqual(stored['provider_response_timeout'], 120)
        self.assertEqual(stored['provider_timeout_retries'], 2)
        self.assertEqual(stored['legacy_metadata'], 'retained')

    def test_save_and_test_validate_numeric_timeout_and_strict_integer_retries_before_any_request(self):
        before = self.app.state.store.get('settings', 'connection')
        invalid = [('provider_response_timeout', value) for value in (0, 3601, 10 ** 400, True, '120', None)]
        invalid += [('provider_timeout_retries', value) for value in (-1, 11, True, 1.5, '2', None)]
        with patch.object(self.app.state.connector, 'models', new=AsyncMock()) as models:
            for key, value in invalid:
                for endpoint in ('/api/settings', '/api/settings/test'):
                    with self.subTest(endpoint=endpoint, key=key, value=value):
                        response = self.client.post(endpoint, json={'server_url': before['server_url'], key: value})
                        self.assertEqual(response.status_code, 422, response.text)
            for value in ('NaN', 'Infinity', '-Infinity'):
                for endpoint in ('/api/settings', '/api/settings/test'):
                    response = self.client.post(endpoint, content='{"server_url":"http://127.0.0.1:8301","provider_response_timeout":' + value + '}', headers={'Content-Type': 'application/json'})
                    self.assertEqual(response.status_code, 422, response.text)
            models.assert_not_awaited()
        self.assertEqual(self.app.state.store.get('settings', 'connection'), before)
        for value in (float('nan'), float('inf'), -float('inf')):
            with self.subTest(value=value):
                with self.assertRaises(ValidationError):
                    ConnectionInput(server_url=before['server_url'], provider_response_timeout=value)

    def test_model_discovery_uses_saved_timeout_and_test_uses_unsaved_timeout_without_persisting(self):
        self.client.post('/api/settings', json={'server_url': 'http://127.0.0.1:8301', 'provider_response_timeout': 12.5, 'provider_timeout_retries': 4}).raise_for_status()
        before = self.app.state.store.get('settings', 'connection')
        with patch.object(self.app.state.connector, 'models', new=AsyncMock(return_value=[{'id': 'test-config', 'name': 'Test'}])) as models:
            response = self.client.get('/api/models')
            self.assertEqual(response.status_code, 200, response.text)
            models.assert_awaited_once_with(before['server_url'], response_timeout=12.5)
            models.reset_mock()
            response = self.client.post('/api/settings/test', json={'server_url': 'http://localhost:9000', 'provider_response_timeout': 2.75, 'provider_timeout_retries': 9})
            self.assertEqual(response.status_code, 200, response.text)
            models.assert_awaited_once_with('http://localhost:9000/v1', response_timeout=2.75)
        self.assertEqual(self.app.state.store.get('settings', 'connection'), before)

    def test_connector_discovery_applies_saved_and_unsaved_timeouts_once_without_retrying(self):
        self.client.post('/api/settings', json={'server_url': 'http://127.0.0.1:8301', 'provider_response_timeout': 12.5, 'provider_timeout_retries': 4}).raise_for_status()
        self.client.get('/api/models').raise_for_status()
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.requests[0].extensions['timeout']['read'], 12.5)
        self.client.post('/api/settings/test', json={'server_url': 'http://localhost:9000', 'provider_response_timeout': 2.75, 'provider_timeout_retries': 9}).raise_for_status()
        self.assertEqual(len(self.requests), 2)
        self.assertEqual(self.requests[1].extensions['timeout']['read'], 2.75)
        self.assertEqual(str(self.requests[1].url), 'http://localhost:9000/v1/models')
        self.assertEqual(self.client.get('/api/settings').json()['provider_response_timeout'], 12.5)

    def test_provider_discovery_errors_return_controlled_502_and_policy_change_does_not_cancel_existing_work(self):
        with patch.object(self.app.state.connector, 'models', new=AsyncMock(side_effect=ConnectorTimeout('Connector request timed out.'))):
            self.assertEqual(self.client.get('/api/models').status_code, 502)
            self.assertEqual(self.client.post('/api/settings/test', json={'server_url': 'http://127.0.0.1:8301', 'provider_response_timeout': 1}).status_code, 502)
        pipeline = self.app.state.pipeline
        with patch.object(pipeline, 'tasks', {'active-fixture': None}), patch.object(pipeline, 'discard', new=AsyncMock()) as discard:
            response = self.client.post('/api/settings', json={'server_url': 'http://127.0.0.1:8301', 'provider_response_timeout': 60, 'provider_timeout_retries': 5})
            self.assertEqual(response.status_code, 200, response.text)
            discard.assert_not_awaited()
        self.assertEqual(self.client.get('/api/settings').json()['provider_timeout_retries'], 5)

    def test_opened_retrying_native_snapshot_is_cancelled_with_public_retry_details_preserved(self):
        project = self.client.post('/api/projects', json={'name': 'Retry snapshot project'}).json()
        state = {'phase': 'retrying', 'iteration': 2, 'retry_attempt': 1, 'retry_limit': 3, 'provider_response_timeout': 1.5}
        source = Path(project['path']) / 'files' / 'retrying.lattice'
        source.write_bytes(lattice.encode({'title': 'Retrying snapshot', 'messages': [], 'execution_state': state}))
        response = self.client.post('/api/projects/' + project['id'] + '/sessions/open', json={'path': 'files/retrying.lattice'})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json()['execution_state'], {**state, 'phase': 'cancelled'})
        saved = lattice.decode(Path(response.json()['saved_path']).read_bytes())
        self.assertEqual(saved['execution_state'], {**state, 'phase': 'cancelled'})


class ProviderPolicyConfigTests(unittest.TestCase):
    def test_config_defaults_and_invalid_runtime_defaults_are_bounded(self):
        with tempfile.TemporaryDirectory(prefix='studio-provider-config-test-') as directory:
            settings = Settings(data_dir=Path(directory), provider_response_timeout=4.5, provider_timeout_retries=3)
            self.assertEqual(settings.provider_policy({'provider_response_timeout': 'invalid', 'provider_timeout_retries': True}), {'provider_response_timeout': 4.5, 'provider_timeout_retries': 3})
            for kwargs in ({'provider_response_timeout': 0}, {'provider_response_timeout': 3601}, {'provider_response_timeout': 10 ** 400}, {'provider_response_timeout': math.inf}, {'provider_response_timeout': True}, {'provider_timeout_retries': -1}, {'provider_timeout_retries': 11}, {'provider_timeout_retries': False}, {'provider_timeout_retries': 1.5}):
                with self.subTest(kwargs=kwargs):
                    with self.assertRaises(ValueError):
                        Settings(data_dir=Path(directory), **kwargs)
