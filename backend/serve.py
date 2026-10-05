"""Uvicorn process with a parent-owned stdin shutdown channel on every OS."""
import argparse
import sys
import threading
import uvicorn

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8000)
    args = parser.parse_args()
    server = uvicorn.Server(uvicorn.Config('backend.app.main:app', host=args.host, port=args.port))
    def watch_parent():
        # An explicit stop or EOF after a parent crash closes jobs through lifespan.
        sys.stdin.readline()
        server.should_exit = True
    threading.Thread(target=watch_parent, daemon=True).start()
    server.run()

if __name__ == '__main__':
    main()
