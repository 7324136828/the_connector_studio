"""Imported active traces and portable live-child snapshots without execution."""
import io
import json
import tempfile
import time
import unittest
import zipfile
from pathlib import Path
from uuid import uuid4

import httpx
from fastapi.testclient import TestClient

from backend.app.config import Settings
from backend.app.main import create_app
from backend.app.repository import now
from backend.app.services import lattice


class TraceSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name).resolve()
        settings = Settings(data_dir=self.root / 'data', workspace_root=self.root / 'workspace', browser_roots=[self.root / 'workspace'])
        self.app = create_app(settings, httpx.MockTransport(lambda request: httpx.Response(200, json={'data': [{'id': 'test-config'}]})))
        self.context = TestClient(self.app)
        self.client = self.context.__enter__()
        response = self.client.post('/api/projects', json={'name': 'Portable snapshots'})
        self.assertEqual(response.status_code, 201, response.text)
        self.project = response.json()

    def tearDown(self):
        self.context.__exit__(None, None, None)
        for path in self.app.state.pipeline.temp.root.glob('job-*'):
            self.app.state.pipeline.temp.purge(path)
        self.directory.cleanup()

    def trace(self, job_id=None):
        return {'job_id': job_id or str(uuid4()), 'status': 'running', 'created_at': now(), 'steps': [
            {'id': str(uuid4()), 'tool_name': 'run_python_script', 'arguments': {'code': 'print(42)'}, 'status': 'running', 'started_at': now(), 'summary': 'Running Python'},
            {'id': str(uuid4()), 'tool_name': 'run_batch_script', 'arguments': {'script': 'echo queued'}, 'status': 'queued', 'started_at': now(), 'summary': 'Queued shell task'},
        ]}

    def source(self):
        return {'title': 'Active snapshot', 'draft': 'Keep my draft', 'model': 'test-config', 'messages': [{'role': 'user', 'author': 'You', 'time': now(), 'text': 'Inspect the active work'}], 'execution_traces': [self.trace()]}

    def import_snapshot(self, source):
        data = lattice.encode(source)
        name = 'snapshot-' + str(uuid4()) + '.lattice'
        (Path(self.project['path']) / 'files' / name).write_bytes(data)
        response = self.client.post('/api/projects/' + self.project['id'] + '/sessions/open', json={'path': 'files/' + name})
        self.assertEqual(response.status_code, 200, response.text)
        return response.json()

    def assert_inactive(self, session):
        self.assertIsNone(session['pending_job'])
        self.assertEqual(session['execution_traces'][0]['status'], 'cancelled')
        self.assertEqual([step['status'] for step in session['execution_traces'][0]['steps']], ['cancelled', 'cancelled'])
        persisted = lattice.decode(Path(session['saved_path']).read_bytes())
        self.assertEqual(persisted['execution_traces'], session['execution_traces'])

    def test_imported_standalone_active_tool_trace_becomes_a_terminal_snapshot(self):
        imported = self.import_snapshot(self.source())
        self.assert_inactive(imported)
        self.assertEqual(imported['draft'], 'Keep my draft')
        self.assertFalse(imported.get('read_only', False))

    def test_imported_individual_active_agent_trace_and_task_are_retained_read_only(self):
        source = self.source()
        source.update(is_agent=True, read_only=True, hidden=True, agent_name='Snapshot analyst', agent_task='Inspect the active work', agent_status='running', parent_session_id=str(uuid4()))
        imported = self.import_snapshot(source)
        self.assert_inactive(imported)
        self.assertEqual(imported['agent_status'], 'cancelled')
        self.assertTrue(imported['is_agent'] and imported['hidden'] and imported['read_only'])
        self.assertEqual(imported['messages'][0]['text'], 'Inspect the active work')
        self.assertIn(self.client.patch('/api/sessions/' + imported['id'], json={'draft': 'Edit the snapshot'}).status_code, (403, 409))

    def test_download_and_zip_export_refresh_a_live_child_trace_and_pending_parent_input(self):
        files, pipeline = self.app.state.session_files, self.app.state.pipeline
        parent = files.new('Live parent', self.project['id'])
        child = files.new('Live agent', self.project['id'], persist=False)
        child = files.update(child['id'], lambda session: session.update(is_agent=True, read_only=True, hidden=True, agent_name='Analyst', agent_task='Inspect live state', agent_status='queued', parent_session_id=parent['id'], model='test-config'))
        stale_child = dict(child)
        child_job = pipeline.new(child, 'agent', 'Inspect live state')
        trace = self.trace(child_job['id'])
        files.update(child['id'], lambda session: session.update(agent_status='running', pending_job=child_job['id'], execution_traces=[trace], messages=[{'role': 'user', 'author': 'Task', 'time': now(), 'text': 'Inspect live state', 'job_id': child_job['id']}]))
        files.autosave(child['id'])
        parent_job = pipeline.new(parent, 'chat', 'Pending parent instruction')
        files.update(parent['id'], lambda session: session.update(pending_job=parent_job['id'], draft='Newer unsent text', child_sessions=[stale_child], messages=[{'role': 'user', 'author': 'You', 'time': now(), 'text': 'Pending parent instruction', 'job_id': parent_job['id']}]))
        # Store embeds remain cached, whereas portable output must read current child state.
        self.assertEqual(self.app.state.store.get('session', parent['id'])['child_sessions'][0]['agent_status'], 'queued')
        download = self.client.get('/api/sessions/' + parent['id'] + '/download')
        self.assertEqual(download.status_code, 200, download.text)
        snapshot = lattice.decode(download.content)
        self.assertEqual(snapshot['messages'], [])
        self.assertEqual(snapshot['draft'], 'Pending parent instruction\nNewer unsent text')
        self.assertEqual(snapshot['child_sessions'][0]['agent_status'], 'running')
        self.assertEqual(snapshot['child_sessions'][0]['execution_traces'], [trace])
        self.assertEqual(snapshot['child_sessions'][0]['messages'][0]['text'], 'Inspect live state')

        response = self.client.post('/api/sessions/' + parent['id'] + '/export')
        self.assertEqual(response.status_code, 202, response.text)
        job_id = response.json()['id']
        for _ in range(100):
            job = self.client.get('/api/jobs/' + job_id).json()
            if job['status'] not in ('queued', 'in_progress'):
                break
            time.sleep(.01)
        self.assertEqual(job['status'], 'completed', job)
        archive = self.client.get('/api/jobs/' + job_id + '/download-zip')
        self.assertEqual(archive.status_code, 200, archive.text)
        with zipfile.ZipFile(io.BytesIO(archive.content)) as contents:
            native = lattice.decode(contents.read('session.lattice'))
            json_snapshot = json.loads(contents.read('session.json'))
        self.assertEqual(native['child_sessions'][0]['execution_traces'], [trace])
        self.assertEqual(json_snapshot['child_sessions'][0]['execution_traces'], [trace])
        self.assertEqual(json_snapshot['draft'], snapshot['draft'])


if __name__ == '__main__':
    unittest.main()
