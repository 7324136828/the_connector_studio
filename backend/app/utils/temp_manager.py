"""Only delete temporary folders owned by this application."""
import shutil
import tempfile
from pathlib import Path

class TempManager:
    def __init__(self, owner):
        self.root = Path(tempfile.gettempdir()).resolve() / ('connector-studio-' + owner)
        self.root.mkdir(mode=0o700, parents=True, exist_ok=True)

    def create(self):
        path = Path(tempfile.mkdtemp(prefix='job-', dir=self.root))
        for name in ('inputs', 'work', 'outputs', 'archive'):
            (path / name).mkdir()
        return path

    def validate(self, path):
        path = Path(path).resolve()
        if path.parent != self.root or not path.name.startswith('job-'):
            raise ValueError('Invalid temporary job path')
        return path

    def purge(self, path):
        path = self.validate(path)
        if path.exists():
            shutil.rmtree(path)
