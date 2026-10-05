"""Exercise the supervisor's service commands and shutdown against isolated data."""
import os
import shutil
import tempfile
import time
from urllib.request import urlopen
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run import ROOT, PYTHON, launch, ready, stop, port

def check(production):
    with tempfile.TemporaryDirectory(prefix='studio-smoke-') as data:
        backend_port = port(18000)
        frontend_port = port(18500, (backend_port,))
        backend_url = 'http://127.0.0.1:' + str(backend_port)
        frontend_url = backend_url if production else 'http://127.0.0.1:' + str(frontend_port)
        env = {**os.environ, 'STUDIO_DATA_DIR': data, 'WORKSPACE_ROOT': str(Path(data) / 'projects'), 'STUDIO_SERVE_FRONTEND': '1' if production else '0', 'CORS_ORIGINS': frontend_url}
        processes = []
        try:
            backend = launch([str(PYTHON), '-m', 'backend.serve', '--port', str(backend_port)], env, ROOT, graceful=True)
            processes.append(backend)
            ready(backend, backend_url)
            if not production:
                npm = shutil.which('npm.cmd' if os.name == 'nt' else 'npm')
                processes.append(launch([npm, 'run', 'dev', '--', '--host', '127.0.0.1', '--port', str(frontend_port), '--strictPort'], {**env, 'VITE_BACKEND_URL': backend_url}, ROOT / 'frontend'))
            deadline = time.monotonic() + 20
            while True:
                try:
                    with urlopen(frontend_url, timeout=1) as response:
                        assert b'Connector Studio' in response.read()
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(.2)
            with urlopen(frontend_url + '/api/health', timeout=2) as response:
                assert b'connector-studio' in response.read()
            print(('Production' if production else 'Development') + ' UI and API ready.')
        finally:
            for process in reversed(processes):
                stop(process)
        assert all(p.poll() is not None for p in processes)
        print('All owned service processes stopped.')
if __name__ == '__main__':
    check(False)
    check(True)
