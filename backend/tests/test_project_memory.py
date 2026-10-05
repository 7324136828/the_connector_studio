"""Project preference persistence and scoped, completed-exchange memory integration."""
import asyncio
import json
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import httpx
from fastapi.testclient import TestClient
from fastapi import HTTPException

from backend.app.config import Settings
from backend.app.main import create_app


class ProjectMemoryTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name).resolve()
        self.workspace = self.root / 'workspace'
        self.models = ['test-config', 'test-config-alt']
        self.completions = []

        async def connector(request):
            if request.url.path == '/v1/models':
                return httpx.Response(200, json={'data': [{'id': model, 'name': model} for model in self.models]})
            body = json.loads(request.content)
            self.completions.append(body)
            text = body['messages'][-1]['content']
            if text == 'cancel this request':
                await asyncio.sleep(20)
            if text == 'fail this request':
                return httpx.Response(500, json={'error': 'Fixture failure'})
            return httpx.Response(200, json={'choices': [{'message': {'role': 'assistant', 'content': 'Reply: ' + text}}]})

        self.transport = httpx.MockTransport(connector)
        self.start(self.root / 'data')

    def start(self, data_dir):
        settings = Settings(data_dir=data_dir, workspace_root=self.workspace, browser_roots=[self.workspace])
        self.app = create_app(settings, self.transport)
        self.context = TestClient(self.app)
        self.client = self.context.__enter__()

    def restart(self, data_dir=None):
        current_data = self.app.state.settings.data_dir
        self.context.__exit__(None, None, None)
        self.start(data_dir or current_data)

    def tearDown(self):
        self.context.__exit__(None, None, None)
        for path in self.app.state.pipeline.temp.root.glob('job-*'):
            self.app.state.pipeline.temp.purge(path)
        self.directory.cleanup()

    def project(self, name='Memory project'):
        response = self.client.post('/api/projects', json={'name': name})
        self.assertEqual(response.status_code, 201, response.text)
        return response.json()

    def session(self, project=None, title='Memory conversation'):
        if project is None:
            if not hasattr(self, 'fixture_project'):
                self.fixture_project = self.project('Independent fixture project')
            project = self.fixture_project
        response = self.client.post('/api/sessions', json={'title': title, 'project_id': project['id'] if project else None})
        self.assertEqual(response.status_code, 201, response.text)
        return response.json()

    def select(self, session, model):
        response = self.client.patch('/api/sessions/' + session['id'], json={'model': model})
        self.assertEqual(response.status_code, 200, response.text)
        return response.json()

    def wait(self, job_id):
        for _ in range(200):
            response = self.client.get('/api/jobs/' + job_id)
            self.assertEqual(response.status_code, 200, response.text)
            job = response.json()
            if job['status'] not in ('queued', 'in_progress'):
                return job
            time.sleep(.01)
        self.fail('Chat did not reach a terminal state')

    def send(self, session, text, model='test-config'):
        response = self.client.post('/api/sessions/' + session['id'] + '/messages', json={'text': text, 'model': model})
        self.assertEqual(response.status_code, 202, response.text)
        return self.wait(response.json()['id'])

    def memory(self, project):
        response = self.client.get('/api/projects/' + project['id'] + '/memory')
        self.assertEqual(response.status_code, 200, response.text)
        return response.json()

    def preferences(self, project):
        response = self.client.get('/api/projects/' + project['id'] + '/preferences')
        self.assertEqual(response.status_code, 200, response.text)
        return response.json()

    def test_configuration_selection_persists_per_project_across_restart_and_fresh_store(self):
        project = self.project()
        other = self.project('Independent project')
        session = self.session(project)
        self.assertEqual(session['model'], '')
        self.client.patch('/api/projects/' + project['id'] + '/preferences', json={'provider_response_timeout': 0.75, 'provider_timeout_retries': 8}).raise_for_status()
        self.select(session, 'test-config-alt')
        self.assertEqual(self.preferences(project)['provider_response_timeout'], 0.75)
        self.assertEqual(self.preferences(project)['provider_timeout_retries'], 8)
        self.assertEqual(self.preferences(other)['provider_response_timeout'], -1)
        self.assertEqual(self.preferences(project)['default_model'], 'test-config-alt')
        preference_file = Path(project['path']) / '.memory/preferences.json'
        self.assertEqual(json.loads(preference_file.read_text(encoding='utf-8'))['default_model'], 'test-config-alt')
        self.assertEqual(self.session(project)['model'], 'test-config-alt')
        self.assertEqual(self.session(other)['model'], '')
        self.assertEqual(self.session()['model'], '')

        self.restart()
        reopened = self.client.post('/api/projects/open', json={'path': project['path']})
        self.assertEqual(reopened.status_code, 200, reopened.text)
        self.assertEqual(self.session(reopened.json())['model'], 'test-config-alt')

        # Opening the project with another application database must read its own file.
        self.restart(self.root / 'fresh-data')
        reopened = self.client.post('/api/projects/open', json={'path': project['path']})
        self.assertEqual(reopened.status_code, 200, reopened.text)
        self.assertEqual(self.preferences(reopened.json())['default_model'], 'test-config-alt')
        self.assertEqual(self.preferences(reopened.json())['provider_response_timeout'], 0.75)
        self.assertEqual(self.preferences(reopened.json())['provider_timeout_retries'], 8)
        self.assertEqual(self.session(reopened.json())['model'], 'test-config-alt')

    def test_send_updates_its_associated_project_default_and_retired_choice_is_retained(self):
        project = self.project()
        session = self.session(project)
        completed = self.send(session, 'Use the alternate configuration', 'test-config-alt')
        self.assertEqual(completed['status'], 'completed', completed)
        self.assertEqual(self.completions[-1]['model'], 'test-config-alt')
        self.assertEqual(self.preferences(project)['default_model'], 'test-config-alt')
        self.models = ['test-config']
        next_session = self.session(project)
        self.assertEqual(next_session['model'], 'test-config-alt')
        failed = self.send(next_session, 'Unavailable choice', next_session['model'])
        self.assertEqual(failed['status'], 'failed')
        self.assertIn('available Connector configuration', failed['error_message'])
        self.assertEqual(self.client.get('/api/sessions/' + next_session['id']).json()['model'], 'test-config-alt')
        self.assertEqual(self.memory(project)['interaction_count'], 1)

    def test_completed_interactions_are_on_disk_recalled_once_and_scoped(self):
        project = self.project()
        other = self.project('Other memory project')
        first = self.session(project)
        marker = 'Remember the unique cobalt project decision'
        job = self.send(first, marker)
        self.assertEqual(job['status'], 'completed', job)
        record_path = Path(project['path']) / '.memory/interactions' / (job['id'] + '.json')
        record = json.loads(record_path.read_text(encoding='utf-8'))
        self.assertEqual(record['job_id'], job['id'])
        self.assertEqual(record['session_id'], first['id'])
        self.assertEqual(record['model'], 'test-config')
        self.assertEqual(record['user_text'], marker)
        self.assertEqual(record['assistant_text'], 'Reply: ' + marker)
        summary = self.memory(project)
        self.assertEqual(summary['interaction_count'], 1)
        self.assertEqual(summary['interactions'][0]['job_id'], job['id'])
        self.assertIsNone(summary['error'])

        second = self.session(project)
        self.send(second, 'What did we decide earlier?')
        historical = [message for message in self.completions[-1]['messages'] if message['role'] == 'system' and message['content'].startswith('Previous completed exchanges from this project.')]
        self.assertEqual(len(historical), 1)
        self.assertIn(marker, historical[0]['content'])
        self.assertIn('Reply: ' + marker, historical[0]['content'])

        self.send(first, 'Continue the original conversation')
        # The first exchange appears in normal session history, not copied into memory.
        system = '\n'.join(message['content'] for message in self.completions[-1]['messages'] if message['role'] == 'system')
        self.assertNotIn(marker, system)
        self.assertEqual(sum(message['content'] == marker for message in self.completions[-1]['messages']), 1)

        self.send(self.session(other), 'A separate project message')
        self.assertNotIn(marker, json.dumps(self.completions[-1]['messages']))
        self.send(self.session(), 'An independent personal message')
        self.assertNotIn(marker, json.dumps(self.completions[-1]['messages']))
        self.assertEqual(self.memory(other)['interaction_count'], 1)

    def test_memory_recall_is_bounded_and_summary_contains_recent_eight(self):
        project = self.project()
        for index in range(10):
            session = self.session(project, title='Conversation ' + str(index))
            job = self.send(session, f'Completed exchange marker {index:02d}')
            self.assertEqual(job['status'], 'completed')
        summary = self.memory(project)
        self.assertEqual(summary['interaction_count'], 10)
        self.assertEqual(len(summary['interactions']), 8)
        self.assertEqual({record['user_text'] for record in summary['interactions']}, {f'Completed exchange marker {index:02d}' for index in range(2, 10)})
        self.send(self.session(project), 'Recall recent project work')
        context = next(message['content'] for message in self.completions[-1]['messages'] if message['role'] == 'system' and message['content'].startswith('Previous completed exchanges from this project.'))
        self.assertNotIn('Completed exchange marker 00', context)
        self.assertNotIn('Completed exchange marker 01', context)
        self.assertIn('Completed exchange marker 09', context)

        self.send(self.session(project), 'Large remembered exchange ' + 'x' * 20000)
        self.send(self.session(project), 'Check the context budget')
        contexts = [message['content'] for message in self.completions[-1]['messages'] if message['role'] == 'system' and message['content'].startswith('Previous completed exchanges from this project.')]
        self.assertEqual(len(contexts), 1)
        self.assertLessEqual(len(contexts[0]), self.memory(project)['context_limit'])

    def test_drafts_failed_requests_cancellation_and_exports_do_not_create_interactions(self):
        project = self.project()
        session = self.session(project)
        self.client.patch('/api/sessions/' + session['id'], json={'draft': 'Unsent private draft'})
        self.assertEqual(self.memory(project)['interaction_count'], 0)
        self.assertEqual(self.send(session, 'fail this request')['status'], 'failed')
        response = self.client.post('/api/sessions/' + session['id'] + '/messages', json={'text': 'cancel this request', 'model': 'test-config'})
        self.assertEqual(response.status_code, 202, response.text)
        job_id = response.json()['id']
        cancel = self.client.post('/api/jobs/' + job_id + '/discard')
        self.assertEqual(cancel.status_code, 200, cancel.text)
        self.assertEqual(cancel.json()['status'], 'discarded')
        exported = self.client.post('/api/sessions/' + session['id'] + '/export')
        self.assertEqual(exported.status_code, 202, exported.text)
        self.assertEqual(self.wait(exported.json()['id'])['status'], 'completed')
        self.assertEqual(self.memory(project)['interaction_count'], 0)
        self.assertEqual(self.client.get('/api/sessions/' + session['id']).json()['messages'], [])

    def test_memory_write_failure_preserves_completed_reply_and_reports_error(self):
        project = self.project()
        session = self.session(project)
        self.select(session, 'test-config')
        memory_folder = Path(project['path']) / '.memory'
        memory_folder.mkdir(exist_ok=True)
        # Simulate a storage failure without depending on OS account privileges.
        with patch.object(self.app.state.project_memory, 'record_interaction', side_effect=PermissionError('Fixture denied memory record write')):
            job = self.send(session, 'The reply must survive a memory write failure')
        self.assertEqual(job['status'], 'completed', job)
        saved = self.client.get('/api/sessions/' + session['id']).json()
        self.assertEqual(saved['messages'][-1]['text'], 'Reply: The reply must survive a memory write failure')
        self.assertIsNone(saved['pending_job'])
        self.assertTrue(saved['memory_error'])
        self.assertTrue(any('memory' in line.lower() for line in job['logs']))
        self.assertEqual(self.memory(project)['interaction_count'], 0)

    def test_memory_path_escape_and_symlink_loop_errors_are_controlled(self):
        project = self.project()
        self.send(self.session(project), 'A legitimate exchange')
        outside = self.root / 'outside'
        outside.mkdir()
        project_root = Path(project['path'])
        real_resolve = Path.resolve

        def escaped_resolve(path, *args, **kwargs):
            if path.is_relative_to(project_root / '.memory'):
                return outside / path.relative_to(project_root / '.memory')
            return real_resolve(path, *args, **kwargs)

        with patch.object(Path, 'resolve', escaped_resolve):
            summary = self.memory(project)
        self.assertTrue(summary['error'])
        self.assertEqual(summary['interactions'], [])
        self.assertEqual(list(outside.iterdir()), [])

        def loop_resolve(path, *args, **kwargs):
            if path.is_relative_to(project_root / '.memory'):
                raise RuntimeError('Symlink loop')
            return real_resolve(path, *args, **kwargs)

        with patch.object(Path, 'resolve', loop_resolve):
            summary = self.memory(project)
        self.assertTrue(summary['error'])
        self.assertEqual(summary['interactions'], [])

    def test_malformed_and_oversized_project_preferences_report_errors_without_overwrite(self):
        project = self.project()
        self.select(self.session(project), 'test-config-alt')
        preference_file = Path(project['path']) / '.memory/preferences.json'
        for content in ('invalid JSON fixture', 'x' * 20000):
            with self.subTest(length=len(content)):
                preference_file.write_text(content, encoding='utf-8')
                preferences = self.preferences(project)
                self.assertTrue(preferences['error'])
                self.assertEqual(preferences['default_model'], '')
                self.assertEqual(preference_file.read_text(encoding='utf-8'), content)
                reopened = self.client.post('/api/projects/open', json={'path': project['path']})
                self.assertEqual(reopened.status_code, 200, reopened.text)
                self.assertTrue(reopened.json()['memory_error'])
                self.assertEqual(preference_file.read_text(encoding='utf-8'), content)

    def test_memory_redacts_credentials_and_duplicate_records_are_idempotent(self):
        project = self.project()
        session = self.session(project)
        credential = 'fixture' + '-private-value'
        bearer = 'Fixture' + 'BearerValue987'
        private_key = '-----BEGIN ' + 'PRIVATE KEY-----\nfixture-key-material\n-----END ' + 'PRIVATE KEY-----'
        user_text = 'Remember ' + credential + ' and Bearer ' + bearer + ' and password=' + 'fixture-value' + '\n' + private_key
        with patch.dict('os.environ', {'CONNECTOR_API_KEY': credential}):
            job = self.send(session, user_text)
        self.assertEqual(job['status'], 'completed', job)
        path = Path(project['path']) / '.memory/interactions' / (job['id'] + '.json')
        serialized = path.read_text(encoding='utf-8')
        self.assertNotIn(credential, serialized)
        self.assertNotIn(bearer, serialized)
        self.assertNotIn('password=fixture-value', serialized)
        self.assertNotIn('fixture-key-material', serialized)
        self.assertIn('[redacted credential]', serialized)
        self.assertIn('[redacted private key]', serialized)
        record = json.loads(serialized)
        self.assertEqual(self.memory(project)['interactions'][0]['user_text'], record['user_text'])
        memory = self.app.state.project_memory
        memory.record_interaction(project['id'], session['id'], job['id'], record['model'], record['user_text'], record['assistant_text'], completed_at=record['completed_at'])
        self.assertEqual(self.memory(project)['interaction_count'], 1)
        with self.assertRaises(HTTPException) as conflict:
            memory.record_interaction(project['id'], session['id'], job['id'], record['model'], record['user_text'], 'A different reply', completed_at=record['completed_at'])
        self.assertEqual(conflict.exception.status_code, 409)
        self.assertEqual(path.read_text(encoding='utf-8'), serialized)


if __name__ == '__main__':
    unittest.main()
