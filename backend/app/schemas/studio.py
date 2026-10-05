from typing import Literal
from pydantic import BaseModel, ConfigDict, Field

Effort = Literal['low', 'medium', 'high', 'extra_high', 'max']

class StrictModel(BaseModel):
    model_config = ConfigDict(extra='forbid')

class ProjectInput(StrictModel):
    name: str = Field(min_length=1, max_length=100)
    parent_path: str | None = Field(default=None, min_length=1, max_length=4096)

class FolderInput(StrictModel):
    parent_path: str = Field(min_length=1, max_length=4096)
    name: str = Field(min_length=1, max_length=100)

class FilesystemLocation(StrictModel):
    name: str
    path: str

class FilesystemRoots(StrictModel):
    default_path: str
    roots: list[FilesystemLocation]
    shortcuts: list[FilesystemLocation]

class FilesystemEntry(StrictModel):
    name: str
    path: str
    kind: Literal['directory', 'file']
    size: int | None
    modified_at: str | None

class FilesystemListing(StrictModel):
    path: str
    parent_path: str | None
    entries: list[FilesystemEntry]
    total: int
    offset: int
    limit: int

class OpenProject(StrictModel):
    path: str = Field(min_length=1, max_length=4096)

class OpenSessionFile(StrictModel):
    path: str = Field(min_length=1, max_length=4096)

class SessionOpenInput(StrictModel):
    project_id: str | None = Field(default=None, min_length=1, max_length=200)

class SessionInput(StrictModel):
    title: str = Field(default='New session', min_length=1, max_length=200)
    project_id: str = Field(min_length=1, max_length=200)

class SessionPatch(StrictModel):
    effort: Effort | None = None
    title: str | None = Field(default=None, min_length=1, max_length=200)
    draft: str | None = Field(default=None, max_length=100000)
    model: str | None = Field(default=None, max_length=200)
    attachments: list[str] | None = Field(default=None, max_length=100)

class MessageInput(StrictModel):
    effort: Effort | None = None
    text: str = Field(min_length=1, max_length=100000)
    model: str = Field(min_length=1, max_length=200)

class EnvironmentInput(StrictModel):
    name: str = Field(min_length=1, max_length=64)

class ConnectionInput(StrictModel):
    server_url: str = Field(min_length=1, max_length=2048)
    font_size: int = Field(default=14, ge=11, le=24)
    max_parallel_agents: int = Field(default=4, ge=1, le=16)
    provider_response_timeout: float = Field(default=120, ge=1, le=3600, strict=True, allow_inf_nan=False)
    provider_timeout_retries: int = Field(default=2, ge=0, le=10, strict=True)

class ResourcePatch(StrictModel):
    effort: Effort = 'low'
    id: str
    enabled: bool
    direction: Literal['up', 'down'] | None = None

class FileInput(StrictModel):
    name: str = Field(min_length=1, max_length=100)
    content: str = Field(default='', max_length=1000000)

class ProjectMemoryPreferences(StrictModel):
    version: Literal[1] = 1
    default_model: str = Field(default='', max_length=200, pattern=r'^(?:[A-Za-z0-9][A-Za-z0-9._:/@+\-]{0,199})?$')

class ProjectMemoryInteraction(StrictModel):
    version: Literal[1] = 1
    project_id: str = Field(min_length=1, max_length=200)
    session_id: str = Field(min_length=1, max_length=200)
    job_id: str = Field(min_length=1, max_length=200)
    model: str = Field(min_length=1, max_length=200, pattern=r'^[A-Za-z0-9][A-Za-z0-9._:/@+\-]{0,199}$')
    completed_at: str = Field(min_length=1, max_length=100)
    user_text: str = Field(max_length=100000)
    assistant_text: str = Field(max_length=8 * 1024 * 1024)
