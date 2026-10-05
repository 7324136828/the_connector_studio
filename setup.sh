#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if command -v python3 >/dev/null 2>&1; then
  exec python3 setup.py "$@"
elif command -v python >/dev/null 2>&1; then
  exec python setup.py "$@"
else
  echo "Python 3.11+ is required." >&2
  exit 1
fi
