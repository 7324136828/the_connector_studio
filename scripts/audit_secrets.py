"""Source-tree credential pattern audit. Print locations and categories, never values."""
import hashlib
import json
import os
import re
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
EXCLUDE = {'.git', '.venv', 'node_modules', 'dist', 'build', '__pycache__', 'test-results', 'playwright-report'}
PATTERNS = {
    'provider token': re.compile(r'\b(?:sk-(?:proj-|ant-)?[A-Za-z0-9_-]{24,}|gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})\b'),
    'AWS access key': re.compile(r'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b'),
    'private key material': re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
    'credential in URL': re.compile(r'(?:https?|postgres(?:ql)?|mysql|mongodb)://[^\s:@/]+:[^\s@/]{8,}@'),
}
def scan():
    findings = []
    reviewed = []
    allowlist_path = ROOT / 'scripts/reviewed_secret_examples.json'
    allowlist = json.loads(allowlist_path.read_text(encoding='utf-8')) if allowlist_path.exists() else []
    files = 0
    for folder, directories, filenames in os.walk(ROOT):
        directories[:] = [d for d in directories if d not in EXCLUDE]
        for name in filenames:
            path = Path(folder) / name
            if path.name == '.env' or path.name.startswith('.env.') and path.name != '.env.example':
                continue
            try:
                text = path.read_text(encoding='utf-8')
            except (UnicodeError, OSError):
                continue
            files += 1
            for line_number, line in enumerate(text.splitlines(), 1):
                for category, pattern in PATTERNS.items():
                    if pattern.search(line):
                        finding = {'file': path.relative_to(ROOT).as_posix(), 'line': line_number, 'category': category}
                        fingerprint = hashlib.sha256(line.encode()).hexdigest()
                        known = next((item for item in allowlist if item['file'] == finding['file'] and item['line'] == line_number and item['fingerprint'] == fingerprint), None)
                        if known:
                            reviewed.append({**finding, 'reason': known['reason']})
                        else:
                            findings.append(finding)
    return {'files_scanned': files, 'findings': findings, 'reviewed_examples': reviewed}
if __name__ == '__main__':
    result = scan()
    print(json.dumps(result, indent=2))
    raise SystemExit(bool(result['findings']))
