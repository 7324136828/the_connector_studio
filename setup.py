#!/usr/bin/env python3
"""Provision the project virtual environment and build the React application."""
import argparse
import os
import shutil
import subprocess
import sys
import venv
from script_runtime import ROOT, PYTHON, VENV

class SetupError(RuntimeError):
    pass

def run(command, cwd=ROOT):
    print('[setup] ' + ' '.join(map(str, command)), flush=True)
    subprocess.run([str(p) for p in command], cwd=cwd, check=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--with-e2e', action='store_true', help='also install browser test dependencies')
    args = parser.parse_args()
    if sys.version_info < (3, 11):
        raise SetupError('Python 3.11 or newer is required')
    node = shutil.which('node')
    npm = shutil.which('npm.cmd' if os.name == 'nt' else 'npm')
    if not node or not npm:
        raise SetupError('Install Node.js 22.12+ or 24+ with npm, then retry')
    version = subprocess.check_output([node, '--version'], text=True).strip().lstrip('v')
    major, minor, *_ = map(int, version.split('.'))
    if major < 22 or (major == 22 and minor < 12):
        raise SetupError('Node.js 22.12+ or 24+ is required')
    if not PYTHON.is_file():
        print('[setup] Creating ' + str(VENV), flush=True)
        venv.EnvBuilder(with_pip=True).create(VENV)
    run([PYTHON, '-m', 'pip', 'install', '--upgrade', 'pip'])
    run([PYTHON, '-m', 'pip', 'install', '-r', ROOT / 'backend/requirements.lock.txt'])
    for folder in ['frontend'] + (['e2e'] if args.with_e2e else []):
        location = ROOT / folder
        run([npm, 'ci' if (location / 'package-lock.json').is_file() else 'install'], location)
    run([npm, 'run', 'typecheck'], ROOT / 'frontend')
    run([npm, 'run', 'build'], ROOT / 'frontend')
    if not (ROOT / '.env').exists():
        shutil.copyfile(ROOT / '.env.example', ROOT / '.env')
    print('[setup] Ready. Start with run.bat or bash run.sh. Use --production to serve the built UI.')
    return 0

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (SetupError, subprocess.CalledProcessError, OSError) as error:
        print('[setup] ERROR: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
