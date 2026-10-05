"""Bounded, cancellable port of ConnectorClient.cpp's text API."""
import asyncio
from contextlib import nullcontext
import json
import re
from uuid import uuid4
import httpx

MAX_ARGUMENTS = 128 * 1024
MAX_TOOL_CALLS = 16
NAME_PATTERN = re.compile(r'[A-Za-z_][A-Za-z0-9_\-]{0,63}')
CALL_PATTERN = re.compile(r'[A-Za-z0-9][A-Za-z0-9_.:\-]{0,199}')


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate JSON field')
        result[key] = value
    return result


def _invalid_constant(value):
    raise ValueError('Non-finite JSON number')


def _arguments(value, *, parsed=False):
    try:
        if parsed and isinstance(value, dict):
            serialized = json.dumps(value, ensure_ascii=False, allow_nan=False)
            result = value
        elif isinstance(value, str):
            serialized = value
            result = json.loads(value, object_pairs_hook=_object, parse_constant=_invalid_constant)
        else:
            raise ValueError()
        if len(serialized.encode('utf-8')) > MAX_ARGUMENTS or not isinstance(result, dict):
            raise ValueError()
        return result
    except (ValueError, TypeError, UnicodeError, RecursionError):
        raise ConnectorError('Connector returned invalid or oversized tool arguments') from None


def _calls(value, *, parsed=False):
    if not isinstance(value, list) or len(value) > MAX_TOOL_CALLS:
        raise ConnectorError('Connector returned an invalid tool call list')
    result, identifiers = [], set()
    for call in value:
        if not isinstance(call, dict):
            raise ConnectorError('Connector returned an invalid tool call')
        function = call.get('function') if 'function' in call else call if parsed else None
        if not isinstance(function, dict) or call.get('type', 'function') != 'function':
            raise ConnectorError('Connector returned an invalid tool call')
        identifier, name = call.get('id'), function.get('name')
        if not isinstance(identifier, str) or not CALL_PATTERN.fullmatch(identifier) or identifier in identifiers or not isinstance(name, str) or not NAME_PATTERN.fullmatch(name):
            raise ConnectorError('Connector returned an invalid tool identifier or name')
        identifiers.add(identifier)
        result.append({'id': identifier, 'name': name, 'arguments': _arguments(function.get('arguments'), parsed=parsed)})
    return result


def _provider_calls(calls):
    return [{'id': call['id'], 'type': 'function', 'function': {'name': call['name'], 'arguments': json.dumps(call['arguments'], ensure_ascii=False, allow_nan=False)}} for call in calls]


def _messages(messages):
    if not isinstance(messages, list) or len(messages) > 10000:
        raise ConnectorError('Conversation contains too many messages')
    prepared = []
    for message in messages:
        if not isinstance(message, dict) or message.get('role') not in ('system', 'developer', 'user', 'assistant', 'tool'):
            raise ConnectorError('Conversation contains an invalid message role')
        role = message['role']
        content = message.get('content', message.get('text'))
        calls = _calls(message['tool_calls'], parsed=True) if message.get('tool_calls') is not None else []
        if (calls and role != 'assistant') or (not isinstance(content, str) and not (content is None and role == 'assistant' and calls)):
            raise ConnectorError('Conversation contains an invalid message')
        entry = {'role': role, 'content': content}
        if calls:
            entry['tool_calls'] = _provider_calls(calls)
        if role == 'tool':
            identifier = message.get('tool_call_id')
            if not isinstance(identifier, str) or not CALL_PATTERN.fullmatch(identifier):
                raise ConnectorError('Conversation contains an invalid tool result identifier')
            entry['tool_call_id'] = identifier
        prepared.append(entry)
    return prepared


def _tools(tools):
    if not isinstance(tools, list) or not 1 <= len(tools) <= 64:
        raise ConnectorError('Tool definitions are invalid')
    names = set()
    try:
        if len(json.dumps(tools, ensure_ascii=False, allow_nan=False).encode('utf-8')) > 1024 * 1024:
            raise ValueError()
        for tool in tools:
            function = tool.get('function') if isinstance(tool, dict) and tool.get('type') == 'function' else None
            name = function.get('name') if isinstance(function, dict) else None
            if not isinstance(name, str) or not NAME_PATTERN.fullmatch(name) or name in names or not isinstance(function.get('parameters', {}), dict):
                raise ValueError()
            names.add(name)
        return tools
    except (ValueError, TypeError, UnicodeError, RecursionError):
        raise ConnectorError('Tool definitions are invalid or oversized') from None

class ConnectorError(RuntimeError):
    pass


class ConnectorTimeout(ConnectorError):
    """A provider request missed its response deadline; safe to retry its payload."""


class ProviderTimeoutExhausted(ConnectorTimeout):
    """The effort-specific timeout attempt budget is exhausted; never recover again."""
    def __init__(self, timeout, attempts):
        self.timeout, self.attempts = timeout, attempts
        super().__init__(f'No provider response within {timeout:g}s after {attempts} attempt(s). Task stopped.')

class Connector:
    def __init__(self, settings, client):
        self.settings, self.client = settings, client

    async def request(self, url, method, endpoint, body=None, *, response_timeout=None):
        timeout = self.settings.provider_response_timeout if response_timeout is None else response_timeout
        if timeout == 0:
            raise ConnectorTimeout('No provider response within 0s.')
        try:
            # Unlimited requests disable every HTTPX deadline, including connect,
            # and create no asyncio deadline. They remain externally cancellable.
            # Finite deadlines use the task snapshot rather than client defaults.
            deadline = nullcontext() if timeout == -1 else asyncio.timeout(timeout)
            request_timeout = None if timeout == -1 else httpx.Timeout(timeout, connect=min(10, timeout))
            async with deadline:
                async with self.client.stream(method, url + endpoint, json=body,
                    timeout=request_timeout, headers={
                    'Authorization': 'Bearer ' + self.api_key
                } if self.api_key else {}) as response:
                    if response.status_code >= 400:
                        raise ConnectorError(f'Connector returned HTTP {response.status_code}. Check the server configuration.')
                    data = bytearray()
                    async for chunk in response.aiter_bytes():
                        data.extend(chunk)
                        if len(data) > 8 * 1024 * 1024:
                            raise ConnectorError('Connector response exceeds 8 MB')
            return json.loads(data)
        except (httpx.TimeoutException, TimeoutError):
            raise ConnectorTimeout(f'No provider response within {timeout:g}s.') from None
        except httpx.HTTPError:
            raise ConnectorError('Cannot reach Connector. Start the server and check Connection settings.') from None
        except (ValueError, TypeError, RecursionError):
            raise ConnectorError('Connector returned invalid JSON') from None

    @property
    def api_key(self):
        import os
        return os.environ.get('CONNECTOR_API_KEY', '')

    async def models(self, url, *, response_timeout=None):
        data = await self.request(url, 'GET', '/models', response_timeout=response_timeout)
        if not isinstance(data, dict) or not isinstance(data.get('data'), list) or len(data['data']) > 1000:
            raise ConnectorError('Connector returned an invalid model list')
        models = {}
        for item in data['data']:
            id = item.get('id') if isinstance(item, dict) else None
            if not isinstance(id, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._:/@+\-]{0,199}', id):
                raise ConnectorError('Connector returned an invalid model identifier')
            name = item.get('name', id)
            models[id] = {'id': id, 'name': name if isinstance(name, str) and len(name) <= 200 else id}
        return list(models.values())

    async def turn(self, url, model, messages, tools=None, validate_model=True, *, response_timeout=None):
        if validate_model:
            models = await self.models(url, response_timeout=response_timeout)
            if model not in {m['id'] for m in models}:
                raise ConnectorError('Select an available Connector configuration')
        elif not isinstance(model, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._:/@+\-]{0,199}', model):
            raise ConnectorError('Select a valid Connector configuration')
        body = {'model': model, 'stream': False, 'messages': _messages(messages)}
        if tools:
            body.update(tools=_tools(tools), tool_choice='auto')
        try:
            oversized = len(json.dumps(body, ensure_ascii=False, allow_nan=False).encode()) > 16 * 1024 * 1024
        except (ValueError, TypeError, UnicodeError, RecursionError):
            raise ConnectorError('Conversation contains invalid text or tool data') from None
        if oversized:
            raise ConnectorError('Conversation exceeds 16 MB. Start a new session.')
        data = await self.request(url, 'POST', '/chat/completions', body, response_timeout=response_timeout)
        try:
            message = data['choices'][0]['message']
            if not isinstance(message, dict) or message.get('role', 'assistant') != 'assistant':
                raise ValueError()
            calls = _calls(message['tool_calls']) if message.get('tool_calls') is not None else []
            if message.get('function_call') is not None:
                if calls:
                    raise ValueError()
                calls = _calls([{'id': 'call_legacy_' + uuid4().hex, 'function': message['function_call']}])
            text = message.get('content') or message.get('refusal') or ''
            if not isinstance(text, str) or not text.strip() and not calls:
                raise ValueError()
            # Provider reasoning fields are deliberately never returned or retained.
            return {'text': text, 'tool_calls': calls}
        except (KeyError, IndexError, TypeError, ValueError):
            raise ConnectorError('Connector returned an invalid text completion') from None

    async def complete(self, url, model, messages, *, response_timeout=None):
        result = await self.turn(url, model, messages, response_timeout=response_timeout)
        if result['tool_calls']:
            raise ConnectorError('This text client cannot execute tool calls. Select a text configuration.')
        return result['text']
