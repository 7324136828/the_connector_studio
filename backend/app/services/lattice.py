"""Native v1 sessions plus a bounded v2 extension for execution and agent traces."""
import json
import math
import re
import struct
from uuid import UUID
from ..config import valid_provider_timeout

MAX_FILE = 32 * 1024 * 1024
MAX_STRING = 4 * 1024 * 1024
MAX_COUNT = 10000
MAGIC = b'LATTICE\0'
MAX_TRACES = 1000
MAX_CHILDREN = 256
MAX_EXTENSION = 16 * 1024 * 1024
MAX_LIVE_UPDATES = 1000
MAX_LIVE_TEXT = 1024 * 1024
EFFORT_LEVELS = {'low', 'medium', 'high', 'extra_high', 'max'}
EXECUTION_PHASES = {'queued', 'running', 'checking', 'summarizing', 'recovering', 'retrying', 'completed', 'failed', 'cancelled'}
LIVE_UPDATE_REQUIRED = {'id', 'job_id', 'text', 'created_at', 'kind'}
LIVE_UPDATE_KEYS = {*LIVE_UPDATE_REQUIRED, 'model'}
MODEL_IDENTIFIER = re.compile(r'[A-Za-z0-9][A-Za-z0-9._:/@+\-]{0,199}')
EXECUTION_STATE_KEYS = {'phase', 'iteration', 'context_tokens', 'summary_count', 'error', 'retry_attempt', 'retry_limit', 'provider_response_timeout'}
EXTENSION_KEYS = {'execution_traces', 'is_agent', 'read_only', 'hidden', 'parent_session_id', 'agent_name', 'agent_status', 'agent_task', 'agent_run_id', 'child_sessions', 'effort', 'live_updates', 'execution_state'}
TRACE_KEYS = {'job_id', 'status', 'created_at', 'completed_at', 'steps'}
STEP_KEYS = {'id', 'tool_name', 'arguments', 'result', 'status', 'started_at', 'completed_at', 'summary', 'agent_session_id', 'agent_name', 'model', 'error'}
CHILD_KEYS = {'id', 'title', 'is_amber', 'eyebrow', 'project_name', 'project_path', 'attachments', 'scroll_offset', 'draft', 'model', 'plan_before_edits', 'auto_run_safe_tools', 'messages', 'created_at', 'updated_at', *EXTENSION_KEYS}
TRACE_STATUSES = {'queued', 'running', 'completed', 'failed', 'cancelled'}
AGENT_STATUSES = {'', 'queued', 'running', 'completed', 'failed', 'cancelled'}
IDENTIFIER = re.compile(r'[A-Za-z0-9][A-Za-z0-9_.:\-]{0,199}')


def _plain_json(value, depth=0):
    if depth > 20:
        raise ValueError('Session extension is too deeply nested')
    if value is None or type(value) is bool or type(value) is int:
        return
    if type(value) is float:
        if not math.isfinite(value):
            raise ValueError('Invalid session extension number')
        return
    if isinstance(value, str):
        if '\0' in value or len(value.encode('utf-8')) > MAX_STRING:
            raise ValueError('Invalid session extension text')
        return
    if isinstance(value, list):
        if len(value) > MAX_COUNT:
            raise ValueError('Too many session extension entries')
        for item in value:
            _plain_json(item, depth + 1)
        return
    if isinstance(value, dict):
        if len(value) > MAX_COUNT or any(not isinstance(key, str) for key in value):
            raise ValueError('Invalid session extension object')
        for key, item in value.items():
            _plain_json(key, depth + 1)
            _plain_json(item, depth + 1)
        return
    raise ValueError('Invalid session extension value')


def _identifier(value, *, optional=False):
    if optional and value in (None, ''):
        return
    if not isinstance(value, str) or not IDENTIFIER.fullmatch(value):
        raise ValueError('Invalid session extension identifier')


def _uuid(value, *, optional=False):
    if optional and value in (None, ''):
        return
    try:
        if str(UUID(value)) != value:
            raise ValueError()
    except (ValueError, TypeError, AttributeError):
        raise ValueError('Invalid agent session identifier') from None


def _extension(value, *, child=False, messages=None):
    if not isinstance(value, dict) or set(value) - (EXTENSION_KEYS | {'message_metadata'}):
        raise ValueError('Invalid session extension fields')
    _plain_json(value)
    for key in ('is_agent', 'read_only', 'hidden'):
        if key in value and type(value[key]) is not bool:
            raise ValueError('Invalid agent session flag')
    if 'parent_session_id' in value:
        _uuid(value['parent_session_id'], optional=True)
    if 'agent_run_id' in value:
        _uuid(value['agent_run_id'], optional=True)
    if 'agent_task' in value and (not isinstance(value['agent_task'], str) or len(value['agent_task']) > 100000):
        raise ValueError('Invalid agent task')
    for key in ('agent_name', 'agent_status'):
        if key in value and (not isinstance(value[key], str) or len(value[key]) > 200):
            raise ValueError('Invalid agent session metadata')
    if value.get('agent_status', '') not in AGENT_STATUSES:
        raise ValueError('Invalid agent session status')
    if 'effort' in value and (not isinstance(value['effort'], str) or value['effort'] not in EFFORT_LEVELS):
        raise ValueError('Invalid task effort level')
    updates = value.get('live_updates', [])
    if not isinstance(updates, list) or len(updates) > MAX_LIVE_UPDATES:
        raise ValueError('Too many live updates')
    update_ids = set()
    for update in updates:
        if not isinstance(update, dict) or set(update) - LIVE_UPDATE_KEYS or not LIVE_UPDATE_REQUIRED <= set(update):
            raise ValueError('Invalid live update fields')
        _identifier(update['id'])
        _identifier(update['job_id'])
        if update['id'] in update_ids:
            raise ValueError('Duplicate live update identifier')
        update_ids.add(update['id'])
        if 'model' in update and (not isinstance(update['model'], str) or not MODEL_IDENTIFIER.fullmatch(update['model'])):
            raise ValueError('Invalid live update model')
        if not isinstance(update['text'], str) or len(update['text'].encode('utf-8')) > MAX_LIVE_TEXT or not isinstance(update['created_at'], str) or not isinstance(update['kind'], str) or update['kind'] not in ('partial', 'status'):
            raise ValueError('Invalid public live update')
    state = value.get('execution_state')
    if state is not None:
        if not isinstance(state, dict) or set(state) - EXECUTION_STATE_KEYS or not {'phase', 'iteration'} <= set(state):
            raise ValueError('Invalid execution state fields')
        if not isinstance(state['phase'], str) or state['phase'] not in EXECUTION_PHASES or type(state['iteration']) is not int or not 0 <= state['iteration'] <= 1000000:
            raise ValueError('Invalid execution state')
        for key in ('context_tokens', 'summary_count'):
            if key in state and (type(state[key]) is not int or not 0 <= state[key] <= 1000000000):
                raise ValueError('Invalid execution state count')
        for key in ('retry_attempt', 'retry_limit'):
            if key in state and (type(state[key]) is not int or not 0 <= state[key] <= 10):
                raise ValueError('Invalid execution retry count')
        if 'provider_response_timeout' in state:
            timeout = state['provider_response_timeout']
            if not valid_provider_timeout(timeout):
                raise ValueError('Invalid execution provider timeout')
        if 'error' in state and (not isinstance(state['error'], str) or len(state['error']) > 10000):
            raise ValueError('Invalid execution state error')
    traces = value.get('execution_traces', [])
    if not isinstance(traces, list) or len(traces) > MAX_TRACES:
        raise ValueError('Too many execution traces')
    trace_ids = set()
    for trace in traces:
        if not isinstance(trace, dict) or set(trace) - TRACE_KEYS or not {'job_id', 'steps', 'status'} <= set(trace):
            raise ValueError('Invalid execution trace fields')
        _identifier(trace['job_id'])
        if trace['job_id'] in trace_ids:
            raise ValueError('Duplicate execution trace identifier')
        trace_ids.add(trace['job_id'])
        if not isinstance(trace['status'], str) or trace['status'] not in TRACE_STATUSES or not isinstance(trace['steps'], list) or len(trace['steps']) > MAX_TRACES:
            raise ValueError('Invalid execution trace')
        for key in ('created_at', 'completed_at'):
            if key in trace and trace[key] is not None and not isinstance(trace[key], str):
                raise ValueError('Invalid execution trace time')
        step_ids = set()
        for step in trace['steps']:
            if not isinstance(step, dict) or set(step) - STEP_KEYS or not {'id', 'tool_name', 'arguments', 'status'} <= set(step):
                raise ValueError('Invalid execution step fields')
            _identifier(step['id'])
            if step['id'] in step_ids:
                raise ValueError('Duplicate execution step identifier')
            step_ids.add(step['id'])
            if not isinstance(step['tool_name'], str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_\-]{0,63}', step['tool_name']) or not isinstance(step['arguments'], dict) or not isinstance(step['status'], str) or step['status'] not in TRACE_STATUSES:
                raise ValueError('Invalid execution step')
            if 'agent_session_id' in step:
                _identifier(step['agent_session_id'], optional=True)
            for key in ('started_at', 'completed_at', 'summary', 'agent_name', 'model', 'error'):
                if key in step and step[key] is not None and not isinstance(step[key], str):
                    raise ValueError('Invalid execution step text')
    metadata = value.get('message_metadata', [])
    if not isinstance(metadata, list) or len(metadata) > MAX_COUNT:
        raise ValueError('Invalid session message metadata')
    indices = set()
    for item in metadata:
        if not isinstance(item, dict) or set(item) != {'index', 'job_id'} or type(item['index']) is not int or item['index'] < 0 or messages is None or item['index'] >= len(messages) or item['index'] in indices:
            raise ValueError('Invalid session message metadata')
        _identifier(item['job_id'])
        indices.add(item['index'])
    children = value.get('child_sessions', [])
    if not isinstance(children, list) or len(children) > MAX_CHILDREN or child and children:
        raise ValueError('Invalid nested agent sessions')
    child_ids = set()
    for session in children:
        if not isinstance(session, dict) or set(session) - CHILD_KEYS or not {'id', 'title', 'messages'} <= set(session):
            raise ValueError('Invalid child session fields')
        _uuid(session['id'])
        if session['id'] in child_ids:
            raise ValueError('Duplicate child session identifier')
        child_ids.add(session['id'])
        if not isinstance(session['title'], str) or not session['title'] or len(session['title']) > 200 or not isinstance(session['messages'], list) or len(session['messages']) > MAX_COUNT:
            raise ValueError('Invalid child session')
        for key in ('eyebrow', 'project_name', 'project_path', 'draft', 'model', 'created_at', 'updated_at'):
            if key in session and not isinstance(session[key], str):
                raise ValueError('Invalid child session text')
        for key in ('is_amber', 'plan_before_edits', 'auto_run_safe_tools'):
            if key in session and type(session[key]) is not bool:
                raise ValueError('Invalid child session flag')
        if 'scroll_offset' in session and (type(session['scroll_offset']) not in (int, float) or not math.isfinite(session['scroll_offset']) or session['scroll_offset'] < 0):
            raise ValueError('Invalid child session scroll offset')
        if 'attachments' in session and (not isinstance(session['attachments'], list) or any(not isinstance(item, str) for item in session['attachments'])):
            raise ValueError('Invalid child session attachments')
        for message in session['messages']:
            if not isinstance(message, dict) or set(message) - {'role', 'author', 'time', 'text', 'items', 'plan_steps', 'job_id'} or message.get('role') not in ('user', 'assistant'):
                raise ValueError('Invalid child session message')
            for key in ('author', 'time', 'text'):
                if key in message and not isinstance(message[key], str):
                    raise ValueError('Invalid child session message text')
            if 'job_id' in message:
                _identifier(message['job_id'])
            if 'items' in message and (not isinstance(message['items'], list) or any(not isinstance(item, str) for item in message['items'])):
                raise ValueError('Invalid child message items')
            if 'plan_steps' in message and not isinstance(message['plan_steps'], list):
                raise ValueError('Invalid child message plan steps')
            for step in message.get('plan_steps', []):
                if not isinstance(step, dict) or set(step) != {'number', 'title', 'description'} or type(step['number']) is not int or not 1 <= step['number'] <= MAX_COUNT or not isinstance(step['title'], str) or not isinstance(step['description'], str):
                    raise ValueError('Invalid child message plan steps')
        _extension({key: session[key] for key in EXTENSION_KEYS if key in session}, child=True, messages=session['messages'])


def _snapshot(session):
    result = {key: session[key] for key in CHILD_KEYS if key in session}
    result['messages'] = [{key: message[key] for key in ('role', 'author', 'time', 'text', 'items', 'plan_steps', 'job_id') if key in message} for message in session['messages']]
    if result.get('child_sessions'):
        raise ValueError('Nested agent sessions are unsupported')
    return result


def _session_extension(session):
    extension = {key: session[key] for key in EXTENSION_KEYS if key in session and session[key] not in (None, '', False, [])}
    if extension.get('effort') == 'low':
        extension.pop('effort')
    if extension.get('child_sessions'):
        extension['child_sessions'] = [_snapshot(child) for child in extension['child_sessions']]
    metadata = [{'index': index, 'job_id': message['job_id']} for index, message in enumerate(session['messages']) if message.get('job_id')]
    # Plain sessions retain the exact native v1 format; metadata is added with traces.
    if extension and metadata:
        extension['message_metadata'] = metadata
    if extension:
        _extension(extension, messages=session['messages'])
    return extension


def _unique_fields(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate session extension field')
        result[key] = value
    return result


def _invalid_constant(value):
    raise ValueError('Non-finite session extension number')

def checksum(payload):
    value = 14695981039346656037
    for byte in payload:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value

def encode(session):
    payload = bytearray()
    def number(value):
        payload.extend(struct.pack('<I', value))
    def text(value):
        data = value.encode('utf-8')
        if b'\0' in data or len(data) > MAX_STRING:
            raise ValueError('Invalid session text')
        number(len(data)); payload.extend(data)
    def count(items):
        if len(items) > MAX_COUNT:
            raise ValueError('Too many session entries')
        number(len(items))
    if not session['title']:
        raise ValueError('Session title is required')
    text(session['title']); number(int(session.get('is_amber', False)))
    text(session.get('eyebrow', '')); text(session.get('project_name', '')); text(session.get('project_path', ''))
    number(len(session.get('attachments', [])))
    payload.extend(struct.pack('<f', session.get('scroll_offset', 0)))
    text(session.get('draft', '')); text(session.get('model', ''))
    number(int(session.get('plan_before_edits', True))); number(int(session.get('auto_run_safe_tools', True)))
    count(session.get('attachments', []))
    for path in session.get('attachments', []):
        text(path)
    count(session['messages'])
    for message in session['messages']:
        if message['role'] not in ('user', 'assistant'):
            raise ValueError('Invalid message role')
        for key in ('role', 'author', 'time', 'text'):
            text(message.get(key, ''))
        count(message.get('items', []))
        for item in message.get('items', []):
            text(item)
        count(message.get('plan_steps', []))
        for step in message.get('plan_steps', []):
            number(step['number']); text(step['title']); text(step['description'])
    extension = _session_extension(session)
    if extension:
        serialized = json.dumps(extension, ensure_ascii=False, allow_nan=False, separators=(',', ':')).encode('utf-8')
        if len(serialized) > MAX_EXTENSION:
            raise ValueError('Session execution metadata exceeds 16 MB')
        number(len(serialized)); payload.extend(serialized)
    if len(payload) + 24 > MAX_FILE:
        raise ValueError('Session exceeds 32 MB')
    return MAGIC + struct.pack('<IIQ', 2 if extension else 1, len(payload), checksum(payload)) + payload

def decode(data):
    if not 24 <= len(data) <= MAX_FILE or data[:8] != MAGIC:
        raise ValueError('Choose a valid .lattice session')
    version, size, hash_value = struct.unpack('<IIQ', data[8:24])
    if version not in (1, 2) or size != len(data) - 24 or hash_value != checksum(data[24:]):
        raise ValueError('Session version, length or checksum is invalid')
    position = 24
    def read(size):
        nonlocal position
        if position + size > len(data):
            raise ValueError('Incomplete session')
        value = data[position:position + size]; position += size
        return value
    def number():
        return struct.unpack('<I', read(4))[0]
    def count():
        value = number()
        if value > MAX_COUNT:
            raise ValueError('Too many session entries')
        return value
    def flag():
        value = number()
        if value not in (0, 1):
            raise ValueError('Invalid flag')
        return bool(value)
    def text():
        size = number()
        if size > MAX_STRING:
            raise ValueError('Session text is too large')
        value = read(size).decode('utf-8')
        if '\0' in value:
            raise ValueError('Invalid session text')
        return value
    result = {'title': text(), 'is_amber': flag(), 'eyebrow': text(), 'project_name': text(), 'project_path': text()}
    result['files_count'] = count()
    result['scroll_offset'] = struct.unpack('<f', read(4))[0]
    result.update(draft=text(), model=text(), plan_before_edits=flag(), auto_run_safe_tools=flag())
    result['attachments'] = [text() for _ in range(count())]
    result['messages'] = []
    for _ in range(count()):
        message = dict(zip(('role', 'author', 'time', 'text'), (text() for _ in range(4))))
        message['items'] = [text() for _ in range(count())]
        message['plan_steps'] = [{'number': count(), 'title': text(), 'description': text()} for _ in range(count())]
        if message['role'] not in ('user', 'assistant') or any(s['number'] < 1 for s in message['plan_steps']):
            raise ValueError('Invalid message')
        result['messages'].append(message)
    if version == 2:
        extension_size = number()
        if not 1 <= extension_size <= MAX_EXTENSION:
            raise ValueError('Invalid session execution metadata size')
        try:
            extension = json.loads(read(extension_size).decode('utf-8'), object_pairs_hook=_unique_fields, parse_constant=_invalid_constant)
            _extension(extension, messages=result['messages'])
        except (TypeError, UnicodeError, RecursionError):
            raise ValueError('Invalid session execution metadata') from None
        for item in extension.pop('message_metadata', []):
            result['messages'][item['index']]['job_id'] = item['job_id']
        result.update(extension)
    if position != len(data) or not result['title'] or not math.isfinite(result['scroll_offset']) or result['scroll_offset'] < 0:
        raise ValueError('Invalid session metadata')
    return result
