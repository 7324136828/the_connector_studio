"""Deterministic loopback fixture, never contacts an AI provider."""
import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

APC_TABLE = """| Feature | APC | DPC |
|-----------------------|------------------------------------------|------------------------------------------|
| **Target** | Specific thread | CPU (system-wide) |
| **Execution Context** | Target thread\u2019s context | Current thread on that CPU |
| **IRQL** | \u2264 1 (typically passive level) | 2 (dispatch level) |
| **Trigger Condition** | Alertable wait (or special APC) | IRQL drop from higher level |
| **Typical Use Case** | Thread-specific async operations | Interrupt deferral, timers, I/O post-processing |"""
TABLE_RESPONSE = (APC_TABLE + '\n\n' + ' '.join(APC_TABLE.splitlines())
    + '\n\n| Field | Value |\n| --- | --- |\n| Literal pipe | a \\| b |'
    + '\n\n```text\n| Code | Text | | --- | --- | | a | b |\n```'
    + '\n\n`| Inline | Text | | --- | --- | | a | b |`'
    + '\n\nPipes are ordinary prose: a | b | c.')


def tool_call(identifier, name, arguments):
    return {'id': identifier, 'type': 'function', 'function': {'name': name, 'arguments': json.dumps(arguments)}}


def effort_response(body):
    """Public responses and JSON continuation decisions for the effort harness."""
    messages = body['messages']
    last = messages[-1].get('content', '')
    if not isinstance(last, str):
        return None
    users = [message.get('content', '') for message in messages if message['role'] == 'user']
    prompt = next((text for text in reversed(users) if isinstance(text, str) and not text.startswith(('Can you continue?', 'Continue the current task'))), '')
    if last.startswith('Can you continue?'):
        public = [message.get('content', '') for message in messages if message['role'] == 'assistant' and isinstance(message.get('content'), str) and not message['content'].lstrip().startswith('{')]
        keep_going = prompt == 'effort cancel' or prompt == 'effort continue' and sum(text.startswith(('Partial result:', 'Final result:')) for text in public) < 2
        return {'role': 'assistant', 'content': json.dumps({'continue': 'yes' if keep_going else 'no'})}
    if prompt not in ('effort continue', 'effort cancel'):
        return None
    if last.startswith('Continue the current task'):
        if prompt == 'effort cancel':
            time.sleep(10)
            text = 'Final result: cancelled request would have continued.'
        else:
            time.sleep(4)
            text = 'Final result: completed the remaining checks.'
    else:
        text = 'Partial result: ready to continue.' if prompt == 'effort cancel' else 'Partial result: inspected the first input.'
    return {'role': 'assistant', 'content': text, 'reasoning_content': 'fixture-private-reasoning'}


def tool_response(body):
    """Deterministic multi-turn project tools, including concurrent child agents."""
    messages = body['messages']
    user_index = next((index for index in range(len(messages) - 1, -1, -1) if messages[index]['role'] == 'user'), None)
    if user_index is None:
        return None
    prompt = messages[user_index]['content']
    if isinstance(prompt, str) and prompt.startswith('Continue the current task'):
        user_index = next((index for index in range(user_index - 1, -1, -1) if messages[index]['role'] == 'user' and isinstance(messages[index].get('content'), str) and not messages[index]['content'].startswith(('Can you continue?', 'Continue the current task'))), None)
        if user_index is None:
            return None
        prompt = messages[user_index]['content']
    if not isinstance(prompt, str):
        return None
    results = [message['content'] for message in messages[user_index + 1:] if message['role'] == 'tool']
    if prompt not in ('tool python', 'tool public progress', 'tool trace slow', 'tool trace failure', 'tool parallel agents', 'tool cancellable agents', 'tool create skill', 'tool create agent', 'tool create mcp', 'fixture agent alpha', 'fixture agent beta', 'fixture slow agent alpha'):
        return None
    if results:
        if prompt == 'tool trace failure':
            return {'role': 'assistant', 'content': None}
        if prompt == 'tool trace slow':
            text = 'Slow traced execution complete.'
        elif prompt == 'tool cancellable agents':
            text = 'Agent delegation complete; stopped agents stayed cancelled.'
        elif prompt == 'tool parallel agents':
            text = 'Parallel agent work complete. Alpha and Beta returned their results.'
        elif prompt.startswith(('fixture agent ', 'fixture slow agent ')):
            text = 'Agent completed ' + prompt + ' using ' + body['model'] + '.'
        elif prompt == 'tool python':
            text = 'Python execution complete: fixture-42.'
        elif prompt == 'tool public progress':
            text = 'Public progress task complete.'
        else:
            text = 'Created the requested project resource.'
        return {'role': 'assistant', 'content': text, 'reasoning_content': 'fixture-private-reasoning'}
    if prompt in ('tool parallel agents', 'tool cancellable agents'):
        calls = [
            tool_call('call_agent_alpha', 'run_agent', {'name': 'Alpha', 'task': 'fixture slow agent alpha' if prompt == 'tool cancellable agents' else 'fixture agent alpha', 'model': 'test-config-alt'}),
            tool_call('call_agent_beta', 'run_agent', {'name': 'Beta', 'task': 'fixture agent beta', 'model': 'test-config'}),
        ]
    elif prompt.startswith('tool create '):
        kind = prompt.removeprefix('tool create ')
        arguments = {'kind': kind, 'name': 'fixture-' + kind, 'instructions': 'Use the project conversation to report checks and explain results.', 'description': 'Created from the fixture conversation'}
        if kind == 'agent':
            arguments['model'] = 'test-config-alt'
        if kind == 'mcp':
            arguments['mcp_config'] = {'command': 'fixture-command', 'args': []}
        calls = [tool_call('call_create_' + kind, 'create_tool_from_conversation', arguments)]
    else:
        if prompt == 'tool trace failure':
            code = "print('trace-before-connector-failure')"
        elif prompt == 'tool python':
            code = "print('fixture-42')"
        elif prompt == 'tool public progress':
            code = "print('public-progress-observation')"
        else:
            code = "import time\ntime.sleep(" + ('30' if prompt in ('fixture slow agent alpha', 'tool trace slow') else '0.3') + ")\nprint(" + repr(prompt) + ")"
        calls = [tool_call('call_python_fixture', 'run_python_script', {'code': code, 'timeout': 60 if prompt in ('fixture slow agent alpha', 'tool trace slow') else 10})]
    return {'role': 'assistant', 'content': 'Public progress: checking the project inputs.' if prompt == 'tool public progress' else None, 'tool_calls': calls, 'reasoning_content': 'fixture-private-reasoning'}

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass
    def respond(self, body):
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
    def do_GET(self):
        self.respond({'data': [{'id': 'test-config', 'name': 'Test configuration'}, {'id': 'test-config-alt', 'name': 'Alternate test configuration'}]})
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        execution = effort_response(body) or tool_response(body)
        if execution is not None:
            self.respond({'choices': [{'message': execution}]})
            return
        text = body['messages'][-1]['content']
        if text == 'slow':
            time.sleep(10)
        self.respond({'choices': [{'message': {'role': 'assistant', 'content': TABLE_RESPONSE if text == 'markdown tables' else '**Mock reply** to ' + text}}]})

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8302)
    args = parser.parse_args()
    ThreadingHTTPServer(('127.0.0.1', args.port), Handler).serve_forever()
