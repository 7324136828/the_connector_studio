"""Provider tool wire format and portable execution trace/session persistence."""
import copy
import json
import struct
import tempfile
import unittest
from pathlib import Path
from uuid import uuid4

import httpx

from backend.app.config import Settings
from backend.app.services.connector import Connector, ConnectorError, MAX_ARGUMENTS
from backend.app.services import lattice

TOOLS = [{'type': 'function', 'function': {'name': 'python', 'description': 'Execute project Python', 'parameters': {'type': 'object', 'properties': {'code': {'type': 'string'}}, 'required': ['code']}}}]


class ConnectorToolTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.requests = []
        self.reply = {'role': 'assistant', 'content': 'Public reply'}
        async def respond(request):
            self.requests.append(request)
            if request.url.path == '/v1/models':
                return httpx.Response(200, json={'data': [{'id': 'test-config', 'name': 'Test configuration'}]})
            return httpx.Response(200, json={'choices': [{'message': self.reply}]})
        self.client = httpx.AsyncClient(transport=httpx.MockTransport(respond))
        self.connector = Connector(Settings(data_dir=Path(self.directory.name)), self.client)

    async def asyncTearDown(self):
        await self.client.aclose()
        self.directory.cleanup()

    def call(self, identifier='call_python_1', arguments='{"code":"print(42)"}', name='python'):
        return {'id': identifier, 'type': 'function', 'function': {'name': name, 'arguments': arguments}}

    async def turn(self, **changes):
        return await self.connector.turn('http://127.0.0.1:8301/v1', 'test-config', [{'role': 'user', 'text': 'Calculate 42'}], **changes)

    async def test_tool_turn_returns_parsed_calls_and_ignores_private_provider_reasoning(self):
        self.reply = {'role': 'assistant', 'content': None, 'tool_calls': [self.call()], 'reasoning': 'Private fixture chain', 'reasoning_content': 'Private fixture detail'}
        turn = await self.turn(tools=TOOLS)
        self.assertEqual(turn, {'text': '', 'tool_calls': [{'id': 'call_python_1', 'name': 'python', 'arguments': {'code': 'print(42)'}}]})
        payload = json.loads(self.requests[-1].content)
        self.assertEqual(payload['tools'], TOOLS)
        self.assertEqual(payload['tool_choice'], 'auto')
        self.assertFalse(payload['stream'])
        self.assertNotIn('Private fixture', json.dumps(turn))

    async def test_assistant_calls_and_tool_responses_roundtrip_in_provider_wire_format(self):
        messages = [
            {'role': 'user', 'text': 'Calculate 42'},
            {'role': 'assistant', 'content': None, 'tool_calls': [{'id': 'call_python_1', 'name': 'python', 'arguments': {'code': 'print(42)'}}], 'reasoning_content': 'Must not be transmitted'},
            {'role': 'tool', 'tool_call_id': 'call_python_1', 'text': '{"stdout":"42\\n","exit_code":0}'},
        ]
        result = await self.connector.turn('http://127.0.0.1:8301/v1', 'test-config', messages, tools=TOOLS, validate_model=False)
        self.assertEqual(result['text'], 'Public reply')
        self.assertEqual(len(self.requests), 1)
        body = json.loads(self.requests[0].content)
        self.assertEqual(body['messages'][1]['tool_calls'][0]['type'], 'function')
        self.assertEqual(json.loads(body['messages'][1]['tool_calls'][0]['function']['arguments']), {'code': 'print(42)'})
        self.assertIsNone(body['messages'][1]['content'])
        self.assertEqual(body['messages'][2]['role'], 'tool')
        self.assertEqual(body['messages'][2]['tool_call_id'], 'call_python_1')
        self.assertNotIn('reasoning_content', body['messages'][1])

    async def test_legacy_calls_are_normalized_and_standalone_complete_rejects_execution(self):
        self.reply = {'role': 'assistant', 'content': None, 'function_call': {'name': 'python', 'arguments': '{"code":"print(42)"}'}}
        result = await self.turn(tools=TOOLS)
        self.assertTrue(result['tool_calls'][0]['id'].startswith('call_legacy_'))
        self.assertEqual(result['tool_calls'][0]['arguments'], {'code': 'print(42)'})
        with self.assertRaisesRegex(ConnectorError, 'cannot execute tool calls'):
            await self.connector.complete('http://127.0.0.1:8301/v1', 'test-config', [{'role': 'user', 'text': 'No project'}])

    async def test_malformed_oversized_and_duplicate_tool_calls_raise_controlled_errors(self):
        cases = [
            [self.call(arguments='not JSON')],
            [self.call(arguments='[]')],
            [self.call(arguments='{"code":NaN}')],
            [self.call(arguments='{"code":"first","code":"duplicate"}')],
            [self.call(arguments=json.dumps({'code': 'x' * MAX_ARGUMENTS}))],
            [self.call(name='../shell')],
            [self.call(identifier='bad call id')],
            [self.call(), self.call()],
            [self.call(identifier='call_' + str(index)) for index in range(17)],
            [{'id': 'call_empty', 'function': {}}],
            {'unexpected': 'object'},
        ]
        for calls in cases:
            with self.subTest(calls_type=type(calls).__name__):
                self.reply = {'role': 'assistant', 'content': None, 'tool_calls': calls}
                with self.assertRaises(ConnectorError):
                    await self.turn(tools=TOOLS)

    async def test_model_validation_occurs_only_when_requested_and_empty_completion_is_invalid(self):
        await self.turn(tools=TOOLS)
        self.assertEqual([request.url.path for request in self.requests], ['/v1/models', '/v1/chat/completions'])
        self.requests.clear()
        await self.turn(tools=TOOLS, validate_model=False)
        self.assertEqual([request.url.path for request in self.requests], ['/v1/chat/completions'])
        self.reply = {'role': 'assistant', 'content': None, 'reasoning_content': 'Private fixture only'}
        with self.assertRaises(ConnectorError):
            await self.turn(tools=TOOLS)


class TraceSessionTests(unittest.TestCase):
    def session(self):
        return {'title': 'Execution session', 'messages': [{'role': 'user', 'author': 'You', 'time': '2026-10-04T12:00:00+00:00', 'text': 'Run project tools', 'job_id': str(uuid4())}], 'attachments': [], 'model': 'test-config'}

    def trace(self, job_id=None):
        return {'job_id': job_id or str(uuid4()), 'status': 'completed', 'created_at': '2026-10-04T12:00:00+00:00', 'completed_at': '2026-10-04T12:00:01+00:00', 'steps': [{'id': 'call_python_1', 'tool_name': 'python', 'arguments': {'code': 'print(42)'}, 'result': {'stdout': '42\n', 'stderr': '', 'exit_code': 0}, 'status': 'completed', 'started_at': '2026-10-04T12:00:00+00:00', 'completed_at': '2026-10-04T12:00:01+00:00', 'summary': 'Printed 42'}]}

    def test_plain_sessions_keep_native_v1_and_execution_traces_roundtrip_as_v2(self):
        session = self.session()
        original = lattice.encode(session)
        self.assertEqual(struct.unpack('<I', original[8:12])[0], 1)
        self.assertNotIn('execution_traces', lattice.decode(original))
        self.assertEqual(struct.unpack('<I', lattice.encode({**session, 'effort': 'low'})[8:12])[0], 1)
        trace = self.trace(session['messages'][0]['job_id'])
        session['execution_traces'] = [trace]
        data = lattice.encode(session)
        self.assertEqual(struct.unpack('<I', data[8:12])[0], 2)
        reopened = lattice.decode(data)
        self.assertEqual(reopened['execution_traces'], [trace])
        self.assertEqual(reopened['messages'][0]['job_id'], session['messages'][0]['job_id'])
        self.assertEqual(lattice.decode(lattice.encode(reopened))['execution_traces'], [trace])
        corrupted = data[:-1] + bytes([data[-1] ^ 1])
        with self.assertRaises(ValueError):
            lattice.decode(corrupted)

    def test_read_only_agent_snapshots_and_trace_links_survive_portable_session_export(self):
        parent = self.session()
        parent_id, child_id = str(uuid4()), str(uuid4())
        child = self.session()
        child.update(id=child_id, title='Analysis agent', is_agent=True, read_only=True, hidden=True, parent_session_id=parent_id, agent_name='Analyst', agent_task='Inspect project data', agent_status='completed', execution_traces=[self.trace()])
        parent['execution_traces'] = [self.trace()]
        parent['execution_traces'][0]['steps'][0].update(agent_session_id=child_id, agent_name='Analyst', model='test-config-alt')
        parent['child_sessions'] = [child]
        restored = lattice.decode(lattice.encode(parent))
        self.assertEqual(restored['child_sessions'][0]['id'], child_id)
        self.assertTrue(restored['child_sessions'][0]['read_only'])
        self.assertEqual(restored['child_sessions'][0]['agent_task'], 'Inspect project data')
        self.assertEqual(restored['child_sessions'][0]['execution_traces'], child['execution_traces'])
        self.assertEqual(restored['execution_traces'][0]['steps'][0]['agent_session_id'], child_id)

    def test_invalid_trace_fields_counts_roles_and_deep_child_nesting_are_rejected(self):
        for mutate in (
            lambda session: session['execution_traces'][0].update(reasoning_content='Private fixture'),
            lambda session: session['execution_traces'][0]['steps'][0].update(status='unrecognized'),
            lambda session: session['execution_traces'][0]['steps'][0].update(arguments=[]),
            lambda session: session['execution_traces'][0]['steps'].append(copy.deepcopy(session['execution_traces'][0]['steps'][0])),
            lambda session: session.update(execution_traces=[self.trace() for _ in range(lattice.MAX_TRACES + 1)]),
        ):
            session = self.session()
            session['execution_traces'] = [self.trace()]
            mutate(session)
            with self.assertRaises(ValueError):
                lattice.encode(session)
        parent, child = self.session(), self.session()
        child.update(id=str(uuid4()), is_agent=True, read_only=True, hidden=True)
        child['messages'][0]['role'] = 'system'
        parent['child_sessions'] = [child]
        with self.assertRaises(ValueError):
            lattice.encode(parent)
        child['messages'][0]['role'] = 'user'
        child['child_sessions'] = [copy.deepcopy(child)]
        with self.assertRaises(ValueError):
            lattice.encode(parent)

    def test_v2_import_rejects_unknown_duplicate_and_oversized_extensions_with_valid_checksum(self):
        payload = lattice.encode(self.session())[24:]
        for value in ('{"provider_reasoning":"private"}', '{"is_agent":true,"is_agent":false}', '{"is_agent":1}', '{"execution_traces":"invalid"}'):
            encoded = value.encode('utf-8')
            body = payload + struct.pack('<I', len(encoded)) + encoded
            data = lattice.MAGIC + struct.pack('<IIQ', 2, len(body), lattice.checksum(body)) + body
            with self.assertRaises(ValueError):
                lattice.decode(data)
        body = payload + struct.pack('<I', lattice.MAX_EXTENSION + 1)
        data = lattice.MAGIC + struct.pack('<IIQ', 2, len(body), lattice.checksum(body)) + body
        with self.assertRaises(ValueError):
            lattice.decode(data)


    def test_effort_public_updates_and_execution_state_roundtrip_with_child_snapshots(self):
        parent = self.session()
        update = {'id': str(uuid4()), 'job_id': str(uuid4()), 'text': 'Inspected the first input file.', 'created_at': '2026-10-04T12:00:00+00:00', 'kind': 'partial'}
        state = {'phase': 'checking', 'iteration': 2, 'context_tokens': 4321, 'summary_count': 1}
        parent.update(effort='max', live_updates=[update], execution_state=state)
        child = self.session()
        child.update(id=str(uuid4()), effort='high', live_updates=[dict(update)], execution_state=dict(state), is_agent=True, read_only=True, hidden=True)
        parent['child_sessions'] = [child]
        decoded = lattice.decode(lattice.encode(parent))
        self.assertEqual(decoded['effort'], 'max')
        self.assertEqual(decoded['live_updates'], [update])
        self.assertEqual(decoded['execution_state'], state)
        self.assertEqual(decoded['child_sessions'][0]['effort'], 'high')
        self.assertEqual(decoded['child_sessions'][0]['live_updates'], [update])
        self.assertEqual(decoded['child_sessions'][0]['execution_state'], state)

    def test_provider_retry_state_roundtrips_for_parent_and_child(self):
        state = {'phase': 'retrying', 'iteration': 3, 'retry_attempt': 2, 'retry_limit': 4, 'provider_response_timeout': 1.5}
        child = {**self.session(), 'id': str(uuid4()), 'is_agent': True, 'read_only': True, 'hidden': True, 'execution_state': dict(state)}
        decoded = lattice.decode(lattice.encode({**self.session(), 'execution_state': state, 'child_sessions': [child]}))
        self.assertEqual(decoded['execution_state'], state)
        self.assertEqual(decoded['child_sessions'][0]['execution_state'], state)
        for timeout in (-1, 0, .01, 3601, 10 ** 12):
            with self.subTest(timeout=timeout):
                special = {**state, 'provider_response_timeout': timeout}
                result = lattice.decode(lattice.encode({**self.session(), 'execution_state': special}))
                self.assertEqual(result['execution_state']['provider_response_timeout'], timeout)
        for key, values in {
            'retry_attempt': (-1, 11, True, 1.5, '1'),
            'retry_limit': (-1, 11, True, 1.5, '1'),
            'provider_response_timeout': (-2, -.5, True, '120', float('nan'), float('inf')),
        }.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    with self.assertRaises(ValueError):
                        lattice.encode({**self.session(), 'execution_state': {**state, key: value}})

    def test_public_update_model_attribution_roundtrips_without_requiring_it_on_old_updates(self):
        old = {'id': str(uuid4()), 'job_id': str(uuid4()), 'text': 'Earlier public result.', 'created_at': '2026-10-04T12:00:00+00:00', 'kind': 'partial'}
        attributed = {**old, 'id': str(uuid4()), 'text': 'Result from the selected configuration.', 'model': 'provider/model-v2:latest'}
        child = self.session()
        child.update(id=str(uuid4()), title='Attributed agent', is_agent=True, read_only=True, hidden=True, live_updates=[attributed])
        session = {**self.session(), 'live_updates': [old, attributed], 'child_sessions': [child]}
        decoded = lattice.decode(lattice.encode(session))
        self.assertEqual(decoded['live_updates'], [old, attributed])
        self.assertNotIn('model', decoded['live_updates'][0])
        self.assertEqual(decoded['child_sessions'][0]['live_updates'][0]['model'], 'provider/model-v2:latest')
        for model in (None, [], 'x' * 201, 'model with spaces', 'model\0hidden'):
            with self.subTest(model_type=type(model).__name__):
                with self.assertRaises(ValueError):
                    lattice.encode({**self.session(), 'live_updates': [{**old, 'model': model}]})

    def test_invalid_effort_live_update_and_execution_state_extensions_are_rejected(self):
        update = {'id': str(uuid4()), 'job_id': str(uuid4()), 'text': 'Public progress', 'created_at': '2026-10-04T12:00:00+00:00', 'kind': 'partial'}
        for extension in (
            {'effort': 'ultra'},
            {'effort': ['max']},
            {'live_updates': [{**update, 'reasoning_content': 'Private fixture'}]},
            {'live_updates': [{**update, 'kind': 'reasoning'}]},
            {'live_updates': [{**update, 'kind': []}]},
            {'live_updates': [{**update, 'text': 'x' * (lattice.MAX_LIVE_TEXT + 1)}]},
            {'live_updates': [dict(update), dict(update)]},
            {'live_updates': [dict(update) for _ in range(lattice.MAX_LIVE_UPDATES + 1)]},
            {'execution_state': {'phase': 'running', 'iteration': True}},
            {'execution_state': {'phase': 'reasoning', 'iteration': 1}},
            {'execution_state': {'phase': 'running', 'iteration': 1, 'context_tokens': -1}},
            {'execution_state': {'phase': 'running', 'iteration': 1, 'reasoning': 'Private fixture'}},
        ):
            with self.subTest(extension=list(extension)):
                with self.assertRaises(ValueError):
                    lattice.encode({**self.session(), **extension})



if __name__ == '__main__':
    unittest.main()
