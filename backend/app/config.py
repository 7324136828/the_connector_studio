"""Runtime configuration; user data defaults outside the source checkout."""
from dataclasses import dataclass, field
import os
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]

def data_home() -> Path:
    if os.name == 'nt':
        return Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData/Local')) / 'ConnectorStudioWeb'
    return Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local/share')) / 'connector-studio'

@dataclass
class Settings:
    data_dir: Path = field(default_factory=lambda: Path(os.environ.get('STUDIO_DATA_DIR', data_home())).resolve())
    workspace_root: Path | None = None
    browser_roots: tuple[Path, ...] | list[Path] | None = None
    connector_url: str = field(default_factory=lambda: os.environ.get('CONNECTOR_URL', 'http://127.0.0.1:8301'))
    allowed_hosts: tuple[str, ...] = field(default_factory=lambda: tuple(os.environ.get('CONNECTOR_ALLOWED_HOSTS', 'localhost,127.0.0.1,::1').split(',')))
    job_ttl: int = field(default_factory=lambda: int(os.environ.get('JOB_TTL_SECONDS', '86400')))
    max_jobs: int = 4
    request_timeout: float = 120

    def __post_init__(self):
        self.data_dir = self.data_dir.resolve()
        self.workspace_root = Path(self.workspace_root or os.environ.get('WORKSPACE_ROOT', self.data_dir / 'projects')).resolve()
        if self.browser_roots is None:
            configured = os.environ.get('FILE_BROWSER_ROOTS')
            roots = None if configured is None or configured.strip() in ('', '*') else [Path(value.strip()) for value in configured.split(os.pathsep) if value.strip()]
        else:
            roots = [Path(value) for value in self.browser_roots]
        if roots is not None and (not roots or any(not root.is_absolute() for root in roots)):
            raise ValueError('FILE_BROWSER_ROOTS must contain absolute folder paths')
        self.browser_roots = None if roots is None else tuple(dict.fromkeys(root.resolve() for root in roots))
        if self.job_ttl < 60:
            raise ValueError('JOB_TTL_SECONDS must be at least 60')
        self.connector_url = self.normalize_url(self.connector_url)

    def normalize_url(self, value: str) -> str:
        url = urlsplit(value.strip())
        if (url.scheme not in ('http', 'https') or not url.hostname or url.username or url.password or url.query or url.fragment or url.path.rstrip('/') not in ('', '/v1')):
            raise ValueError('Use an HTTP(S) Connector root URL or /v1 URL without credentials')
        if url.port is not None and not 1 <= url.port <= 65535:
            raise ValueError('Connector port must be between 1 and 65535')
        if any(character.isspace() for character in value) or '\\' in value:
            raise ValueError('Connector URL contains invalid characters')
        if url.hostname not in self.allowed_hosts:
            raise ValueError('Connector host is not in CONNECTOR_ALLOWED_HOSTS')
        return f'{url.scheme}://{url.netloc}/v1'
