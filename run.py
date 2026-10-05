#!/usr/bin/env python3
"""Launch Connector Studio's backend and React UI; stop the entire process trees."""
import argparse
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
from urllib.request import urlopen
from urllib.error import URLError
from script_runtime import ROOT, PYTHON, load_env

class RunError(RuntimeError):
    pass

def port(value, reserved=(), host='127.0.0.1'):
    try:
        first = int(value)
    except (TypeError, ValueError):
        raise RunError('Ports must be integers') from None
    if not 1 <= first <= 65535:
        raise RunError('Ports must be between 1 and 65535')
    for number in range(first, min(first + 100, 65536)):
        if number in reserved:
            continue
        with socket.socket() as probe:
            try:
                probe.bind((host, number))
            except OSError:
                continue
        return number
    raise RunError('No available port found within 100 ports of the configured port')

def stop(process):
    if process.poll() is not None:
        return
    if process.stdin is not None:
        try:
            process.stdin.write('stop\n')
            process.stdin.flush()
            process.wait(timeout=8)
            return
        except (OSError, subprocess.TimeoutExpired):
            pass
    if os.name == 'nt':
        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    else:
        os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        if os.name == 'nt':
            process.kill()
        else:
            os.killpg(process.pid, signal.SIGKILL)
        process.wait()

def launch(command, env, cwd, graceful=False):
    options = {'creationflags': subprocess.CREATE_NO_WINDOW} if os.name == 'nt' else {'start_new_session': True}
    return subprocess.Popen(command, env=env, cwd=cwd, stdin=subprocess.PIPE if graceful else None, text=True, **options)

def ready(process, url):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RunError('Backend exited during startup')
        try:
            with urlopen(url + '/api/health', timeout=1) as response:
                if response.status == 200:
                    return
        except (OSError, URLError):
            time.sleep(.2)
    raise RunError('Backend did not become ready within 30 seconds')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--production', action='store_true', help='serve the built React bundle from Python')
    parser.add_argument('--backend-port', type=int)
    parser.add_argument('--frontend-port', type=int)
    parser.add_argument('--check', action='store_true', help='verify service readiness then stop')
    parser.add_argument('--reload', action='store_true', help='development only; restarts cancel running requests')
    args = parser.parse_args()
    if not PYTHON.is_file():
        if subprocess.call([sys.executable, str(ROOT / 'setup.py')], cwd=ROOT):
            raise RunError('Setup failed')
    load_env()
    backend_port = port(args.backend_port if args.backend_port is not None else os.environ.get('BACKEND_PORT', '8000'))
    frontend_port = port(args.frontend_port if args.frontend_port is not None else os.environ.get('FRONTEND_PORT', '5173'), (backend_port,))
    backend_url = 'http://127.0.0.1:' + str(backend_port)
    frontend_url = backend_url if args.production else 'http://127.0.0.1:' + str(frontend_port)
    env = os.environ.copy()
    env['CORS_ORIGINS'] = ','.join({*env.get('CORS_ORIGINS', '').split(','), frontend_url, 'http://localhost:' + str(frontend_port)})
    env['STUDIO_SERVE_FRONTEND'] = '1' if args.production else '0'
    command = [str(PYTHON), '-m', 'backend.serve', '--host', '127.0.0.1', '--port', str(backend_port)]
    if args.reload and not args.production:
        command = [str(PYTHON), '-m', 'uvicorn', 'backend.app.main:app', '--host', '127.0.0.1', '--port', str(backend_port), '--reload', '--reload-dir', str(ROOT / 'backend')]
    npm = shutil.which('npm.cmd' if os.name == 'nt' else 'npm')
    if args.production and not (ROOT / 'frontend/dist/index.html').is_file():
        raise RunError('Frontend build missing; run setup first')
    if not args.production and (not npm or not (ROOT / 'frontend/node_modules').is_dir()):
        raise RunError('Frontend dependencies missing; run setup first')
    processes = []
    def interrupted(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    print('[run] Connector Studio', flush=True)
    try:
        backend = launch(command, env, ROOT, graceful=not args.reload or args.production)
        processes.append(backend)
        ready(backend, backend_url)
        if not args.production:
            frontend_env = env.copy()
            frontend_env['VITE_BACKEND_URL'] = backend_url
            processes.append(launch([npm, 'run', 'dev', '--', '--host', '127.0.0.1', '--port', str(frontend_port), '--strictPort'], frontend_env, ROOT / 'frontend'))
        if args.check:
            ready(processes[-1], frontend_url)
            print('[run] UI and API readiness verified.', flush=True)
            return 0
        print('[run] Open ' + frontend_url + '\n[run] API docs: ' + backend_url + '/docs\n[run] Press Ctrl+C to stop.', flush=True)
        while True:
            for process in processes:
                if process.poll() is not None:
                    raise RunError('A service exited with code ' + str(process.returncode))
            time.sleep(.5)
    except KeyboardInterrupt:
        print('\n[run] Stopping services...', flush=True)
        return 0
    finally:
        for process in reversed(processes):
            stop(process)

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RunError, OSError) as error:
        print('[run] ERROR: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
