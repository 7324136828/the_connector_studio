"""Connector Studio: local-first FastAPI application and React production host."""
import asyncio
import os
import re
from contextlib import asynccontextmanager
import httpx
from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import FileResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles
from fastapi.middleware.cors import CORSMiddleware
from fastapi.middleware.trustedhost import TrustedHostMiddleware
from .config import Settings, ROOT
from .repository import Store, now
from .schemas.studio import ProjectInput, OpenProject, SessionInput, SessionPatch, MessageInput, ConnectionInput, ResourcePatch, FileInput, FolderInput, FilesystemRoots, FilesystemListing, OpenSessionFile, SessionOpenInput, EnvironmentInput, Effort
from .services.workspace import Workspace
from .services.session_files import SessionFiles
from .services.project_memory import ProjectMemory, memory_error_detail
from .services.connector import Connector, ConnectorError
from .services.pipeline import Pipeline
from .services.tool_runtime import ToolRuntime
from .services import lattice
from .services.session_names import download_disposition

_LOOPBACK_ORIGIN = re.compile(r'https?://(?:localhost|127\.0\.0\.1|\[::1\])(?::([0-9]{1,5}))?', re.IGNORECASE)


def allowed_browser_origin(origin, configured_origins):
    """Accept configured origins or exact loopback hosts at valid HTTP(S) ports."""
    match = _LOOPBACK_ORIGIN.fullmatch(origin)
    loopback = match is not None and (match.group(1) is None or 1 <= int(match.group(1)) <= 65535)
    return origin in configured_origins or loopback


class LocalCORSMiddleware(CORSMiddleware):
    def is_allowed_origin(self, origin):
        return allowed_browser_origin(origin, self.allow_origins)


def create_app(settings=None, transport=None):
    settings = settings or Settings()
    store = Store(settings.data_dir / 'studio.db')
    workspace = Workspace(settings, store)
    project_memory = ProjectMemory(workspace)
    session_files = SessionFiles(settings, store, workspace, project_memory=project_memory)
    tool_runtime = ToolRuntime(settings, workspace)

    @asynccontextmanager
    async def lifespan(application):
        store.initialize()
        for project in store.list('project'):
            if project.get('managed'):
                try:
                    tool_runtime.bootstrap(project['id'])
                except (OSError, ValueError, HTTPException):
                    pass  # Existing read-only or missing projects remain browsable.
        settings.workspace_root.mkdir(parents=True, exist_ok=True)
        try:
            settings.normalize_url(store.get('settings', 'connection')['server_url'])
        except HTTPException:
            store.put('settings', {'id': 'connection', 'server_url': settings.connector_url, 'font_size': 14})
        async with httpx.AsyncClient(timeout=httpx.Timeout(settings.request_timeout, connect=10), transport=transport, trust_env=False, follow_redirects=False) as client:
            pipeline = Pipeline(settings, store, Connector(settings, client), session_files, project_memory=project_memory, tool_runtime=tool_runtime, transport=transport)
            application.state.pipeline = pipeline
            application.state.connector = pipeline.connector
            pipeline.recover()
            async def sweep():
                while True:
                    await asyncio.sleep(30)
                    pipeline.cleanup()
            cleanup = asyncio.create_task(sweep())
            try:
                yield
            finally:
                cleanup.cancel()
                await asyncio.gather(cleanup, return_exceptions=True)
                await pipeline.close()

    app = FastAPI(title='Connector Studio API', version='1.0.0', lifespan=lifespan)
    app.state.store = store
    app.state.settings = settings
    app.state.session_files = session_files
    app.state.project_memory = project_memory
    app.state.tool_runtime = tool_runtime
    origins = [s.strip() for s in os.environ.get('CORS_ORIGINS', 'http://localhost:5173,http://127.0.0.1:5173').split(',') if s.strip() and s.strip() != '*']
    app.add_middleware(LocalCORSMiddleware, allow_origins=origins, allow_methods=['GET', 'POST', 'PATCH'], allow_headers=['Content-Type'])
    app.add_middleware(TrustedHostMiddleware, allowed_hosts=['localhost', '127.0.0.1', '[::1]', 'testserver'])

    @app.middleware('http')
    async def boundaries(request, call_next):
        origin = request.headers.get('origin')
        if request.method not in ('GET', 'HEAD', 'OPTIONS') and origin and not allowed_browser_origin(origin, origins) and origin != str(request.base_url).rstrip('/'):
            return JSONResponse({'detail': 'Origin is not allowed'}, 403)
        length = request.headers.get('content-length')
        if length and (not length.isdigit() or int(length) > lattice.MAX_FILE + 1024 * 1024):
            return JSONResponse({'detail': 'Request exceeds 33 MB'}, 413)
        response = await call_next(request)
        response.headers['X-Content-Type-Options'] = 'nosniff'
        response.headers['Referrer-Policy'] = 'no-referrer'
        response.headers['X-Frame-Options'] = 'DENY'
        if request.url.path.startswith('/api'):
            response.headers['Cache-Control'] = 'no-store'
        return response

    @app.exception_handler(ValueError)
    async def invalid(request, error):
        return JSONResponse({'detail': str(error)}, 422)

    @app.exception_handler(ConnectorError)
    async def unavailable(request, error):
        return JSONResponse({'detail': str(error)}, 502)

    @app.exception_handler(OSError)
    async def filesystem_error(request, error):
        return JSONResponse({'detail': 'Cannot access the requested file or folder. Check permissions and free disk space.'}, 409)

    @app.get('/api/health')
    def health():
        return {'status': 'ok', 'service': 'connector-studio'}

    @app.get('/api/settings')
    def get_settings():
        return {'max_parallel_agents': 4, **store.get('settings', 'connection'), 'workspace_root': str(settings.workspace_root)}

    @app.post('/api/settings/test')
    async def test_connection(body: ConnectionInput):
        return {'models': await app.state.connector.models(settings.normalize_url(body.server_url))}

    @app.post('/api/settings')
    async def save_settings(body: ConnectionInput):
        url = settings.normalize_url(body.server_url)
        if url != store.get('settings', 'connection')['server_url']:
            for id in list(app.state.pipeline.tasks):
                await app.state.pipeline.discard(id)
        result = store.put('settings', {'id': 'connection', 'server_url': url, 'font_size': body.font_size, 'max_parallel_agents': body.max_parallel_agents})
        app.state.pipeline.workers.wake()
        return result

    @app.get('/api/models')
    async def models():
        return await app.state.connector.models(store.get('settings', 'connection')['server_url'])

    @app.get('/api/projects')
    def projects():
        return workspace.recent_projects()

    @app.get('/api/filesystem/roots', response_model=FilesystemRoots)
    def filesystem_roots():
        return workspace.browser_locations()

    @app.get('/api/filesystem/list', response_model=FilesystemListing)
    def filesystem_list(path: str | None = Query(default=None, min_length=1, max_length=4096), search: str = Query(default='', max_length=200), offset: int = Query(default=0, ge=0), limit: int = Query(default=200, ge=1, le=500)):
        return workspace.list_directory(path, search, offset, limit)

    @app.post('/api/filesystem/folders', status_code=201)
    def filesystem_folder(body: FolderInput):
        return workspace.create_folder(body.parent_path, body.name)

    def project_details(project):
        try:
            preferences = project_memory.initialize(project['id'])
            return {**project, **preferences, 'memory_error': None}
        except Exception as error:
            return {**project, 'default_model': '', 'memory_error': memory_error_detail(error)}

    @app.post('/api/projects', status_code=201)
    def create_project(body: ProjectInput):
        project = workspace.create(body.name, body.parent_path)
        try:
            tool_runtime.bootstrap(project['id'])
        except (OSError, ValueError, HTTPException) as error:
            project['tool_error'] = memory_error_detail(error)
        return project_details(project)

    @app.post('/api/projects/open')
    def open_project(body: OpenProject):
        project = workspace.open(body.path)
        if project.get('managed'):
            try:
                tool_runtime.bootstrap(project['id'])
            except (OSError, ValueError, HTTPException):
                project['tool_error'] = 'Project skills could not be updated. Check folder permissions.'
        return project_details(project)

    @app.get('/api/projects/{id}/preferences')
    def project_preferences(id: str):
        store.get('project', id)
        try:
            return {'project_id': id, **project_memory.initialize(id), 'error': None}
        except Exception as error:
            return {'project_id': id, 'default_model': '', 'error': memory_error_detail(error)}

    @app.get('/api/projects/{id}/memory')
    def memory(id: str):
        store.get('project', id)
        try:
            return {**project_memory.summary(id), 'context_limit': 16000, 'error': None}
        except Exception as error:
            return {'project_id': id, 'default_model': '', 'interaction_count': 0,
                    'interactions': [], 'context_limit': 16000, 'error': memory_error_detail(error)}

    @app.get('/api/projects/{id}/environments')
    def environments(id: str):
        return tool_runtime.environments(id)

    @app.post('/api/projects/{id}/environments', status_code=201)
    async def create_environment(id: str, body: EnvironmentInput):
        await asyncio.to_thread(tool_runtime.create_environment, id, body.name)
        return tool_runtime.environments(id)

    @app.patch('/api/projects/{id}/environments')
    def select_environment(id: str, body: EnvironmentInput):
        return tool_runtime.select_environment(id, body.name)

    @app.get('/api/projects/{id}/files')
    def files(id: str):
        return workspace.files(id)

    @app.post('/api/projects/{id}/files', status_code=201)
    def create_file(id: str, body: FileInput):
        return workspace.file(id, body.name, body.content)

    @app.get('/api/projects/{id}/resources')
    def resources(id: str, effort: Effort = 'low'):
        return workspace.resources(id, effort=effort)

    @app.patch('/api/projects/{id}/resources')
    def update_resource(id: str, body: ResourcePatch):
        entries = workspace.resources(id, effort=body.effort)
        entry = next((r for r in entries if r['id'] == body.id), None)
        if not entry:
            raise HTTPException(404, 'Resource not found')
        if entry.get('locked') and body.enabled:
            raise HTTPException(409, 'Multi-agent mode requires Max effort and explicit opt-in')
        if entry['kind'] == 'mcp' and body.enabled:
            raise HTTPException(409, 'MCP transport execution is not available in the web port')
        def mutate(project):
            prefs = project['preferences']
            prefs.setdefault(body.id, {})['enabled'] = body.enabled
            agents = [r['id'] for r in entries if r['kind'] == 'agent']
            if body.direction and body.id in agents:
                index = agents.index(body.id)
                other = index + (-1 if body.direction == 'up' else 1)
                if 0 <= other < len(agents):
                    agents[index], agents[other] = agents[other], agents[index]
            for index, key in enumerate(agents):
                prefs.setdefault(key, {})['priority'] = index
        store.update('project', id, mutate)
        return workspace.resources(id, effort=body.effort)

    @app.get('/api/sessions')
    def sessions(include_agents: bool = False):
        return [session for session in store.list('session') if session.get('project_id') and (include_agents or not session.get('hidden'))]

    def new_session(body):
        workspace.path(body.project_id)
        return session_files.new(body.title, body.project_id)

    def require_session_project(session, project_id=None):
        target = project_id or session.get('project_id')
        if not target:
            raise HTTPException(409, 'Open this conversation in a project before using it')
        workspace.path(target)
        return target

    @app.post('/api/projects/{id}/sessions/open')
    def open_session_file(id: str, body: OpenSessionFile):
        return session_files.open(id, body.path)

    @app.post('/api/sessions', status_code=201)
    def create_session(body: SessionInput):
        return new_session(body)

    @app.post('/api/sessions/import')
    def import_session():
        raise HTTPException(410, 'Session file uploads are not supported. Open .lattice files through the project explorer.')

    @app.post('/api/sessions/{id}/open')
    def open_saved_session(id: str, body: SessionOpenInput | None = None):
        session = store.get('session', id)
        project_id = require_session_project(session, body.project_id if body else None)
        return session_files.ensure_project_file(id, project_id)

    @app.get('/api/sessions/{id}')
    def get_session(id: str):
        return store.get('session', id)

    def disable_multi_agent(project_id):
        if not project_id:
            return
        entries = workspace.resources(project_id, effort='max')
        def disable(project):
            for entry in entries:
                if entry['kind'] == 'skill' and entry['name'] in ('run_agent', 'create_agent'):
                    project['preferences'].setdefault(entry['id'], {})['enabled'] = False
        store.update('project', project_id, disable)

    @app.patch('/api/sessions/{id}')
    def patch_session(id: str, body: SessionPatch):
        changes = body.model_dump(exclude_none=True)
        def mutate(session):
            if session.get('read_only') or session.get('is_agent'):
                raise HTTPException(409, 'Agent execution sessions are read-only')
            if session['pending_job'] and ('model' in changes or 'effort' in changes):
                raise HTTPException(409, 'Stop the active request before changing model or effort')
            session.update(changes)
        with session_files.lock(id):
            original = store.get('session', id)
            if 'effort' in changes and (changes['effort'] != original.get('effort', 'low') or changes['effort'] != 'max'):
                if original.get('read_only') or original.get('is_agent'):
                    raise HTTPException(409, 'Agent execution sessions are read-only')
                if original['pending_job']:
                    raise HTTPException(409, 'Stop the active request before changing model or effort')
                disable_multi_agent(original.get('project_id'))
            session = session_files.update(id, mutate)
            return remember_configuration(session, changes['model']) if 'model' in changes else session

    def remember_configuration(session, model):
        if not session.get('project_id'):
            return session
        try:
            project_memory.set_default_model(session['project_id'], model)
            error = None
        except Exception as problem:
            error = memory_error_detail(problem)
        return session_files.update(session['id'], lambda current: current.update(memory_error=error))

    @app.post('/api/sessions/{id}/save')
    def save_session(id: str):
        require_session_project(store.get('session', id))
        return session_files.save(id)

    @app.get('/api/sessions/{id}/download')
    def download_session(id: str):
        session = session_files.snapshot(id)
        return Response(lattice.encode(session), media_type='application/octet-stream', headers={'Content-Disposition': download_disposition(session['title'])})

    @app.post('/api/sessions/{id}/messages', status_code=202)
    async def send_message(id: str, body: MessageInput):
        if not body.text.strip():
            raise ValueError('Enter a message')
        with session_files.lock(id):
            session = store.get('session', id)
            require_session_project(session)
            if session.get('read_only') or session.get('is_agent'):
                raise HTTPException(409, 'Agent execution sessions are read-only')
            if session['pending_job']:
                raise HTTPException(409, 'This session already has an active request')
            pipeline = app.state.pipeline
            effort = body.effort or session.get('effort', 'low')
            if effort != session.get('effort', 'low') or effort != 'max':
                disable_multi_agent(session.get('project_id'))
            selected = remember_configuration(session, body.model)
            selected = {**selected, 'effort': effort}
            job = pipeline.new({**selected, 'model': body.model}, 'chat', body.text)
            def submit(session):
                session.update(model=body.model, effort=effort, draft='', pending_job=job['id'])
                session['messages'].append({'role': 'user', 'author': 'You', 'time': now(), 'text': body.text, 'job_id': job['id']})
            session_files.update(id, submit)
            return pipeline.start(job, pipeline.chat(job, store.get('settings', 'connection')['server_url']))

    @app.post('/api/sessions/{id}/export', status_code=202)
    async def export_session(id: str):
        session = store.get('session', id)
        require_session_project(session)
        pipeline = app.state.pipeline
        job = pipeline.new(session, 'export')
        return pipeline.start(job, pipeline.export(job, session))

    @app.get('/api/jobs')
    def jobs():
        return [app.state.pipeline.public(job) for job in store.list('job')]

    @app.get('/api/jobs/{id}')
    def get_job(id: str):
        return app.state.pipeline.public(store.get('job', id))

    @app.post('/api/jobs/{id}/discard')
    async def discard(id: str):
        job = store.get('job', id)
        return await app.state.pipeline.discard(id)

    @app.get('/api/jobs/{id}/download-zip')
    def download_zip(id: str):
        job = store.get('job', id)
        if job['status'] != 'completed' or not job['download_available']:
            raise HTTPException(409, 'Export is not available; export the saved session again')
        path = app.state.pipeline.temp.validate(job['temp_dir']) / 'archive' / 'session.zip'
        if not path.is_file():
            raise HTTPException(410, 'Export expired; export the saved session again')
        return FileResponse(path, filename=f'connector-session-{id}.zip', media_type='application/zip')

    if os.environ.get('STUDIO_SERVE_FRONTEND') == '1':
        dist = ROOT / 'frontend/dist'
        if not (dist / 'index.html').is_file():
            raise RuntimeError('Frontend build missing; run setup.py first')
        @app.api_route('/api/{path:path}', methods=['GET', 'POST', 'PATCH', 'DELETE'])
        def unknown_api(path: str):
            raise HTTPException(404, 'API route not found')
        app.mount('/', StaticFiles(directory=dist, html=True), name='frontend')
    return app

app = create_app()
