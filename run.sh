#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ ! -x .venv/bin/python ] && [ ! -f .venv/Scripts/python.exe ]; then
  bash setup.sh
fi
if [ -x .venv/bin/python ]; then
  studio_python=".venv/bin/python"
elif [ -f .venv/Scripts/python.exe ]; then
  studio_python=".venv/Scripts/python.exe"
else
  echo "Setup did not create a project Python environment." >&2
  exit 1
fi
exec "$studio_python" run.py "$@"
