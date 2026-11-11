"""Local Unix socket for a container to use the host user's Codex installation."""

import json
import os
from pathlib import Path
import signal
import socket
from socketserver import StreamRequestHandler, ThreadingUnixStreamServer
import subprocess
import sys
from threading import Event, Thread


STATE_FIELDS = ("local-projects", "project-order", "thread-project-assignments", "projectless-thread-ids")


def desktop_state(codex_home):
    try:
        return json.loads((codex_home / ".codex-global-state.json").read_text())
    except FileNotFoundError:
        return {}


def connect(path):
    connection = socket.socket(socket.AF_UNIX)
    try:
        connection.settimeout(5)
        connection.connect(path)
        return connection
    except OSError:
        connection.close()
        raise


def relay_read(path, command, limit):
    with connect(path) as connection:
        connection.sendall(command)
        with connection.makefile("rb") as reader:
            data = reader.read(limit + 1)
        if len(data) > limit:
            raise OSError("Host relay response too large")
        return data


class HostSession:
    def __init__(self, path):
        self.socket = connect(path)
        try:
            self.socket.sendall(b"C")
            self.socket.settimeout(None)
            self.stdin = self.socket.makefile("w", encoding="utf-8")
            self.stdout = self.socket.makefile("r", encoding="utf-8")
        except OSError:
            self.socket.close()
            raise
        self.finished = Event()

    def poll(self):
        return 0 if self.finished.is_set() else None

    def close(self):
        try:
            self.socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.finished.set()
        self.socket.close()
        try:
            self.stdin.close()
        except OSError:
            pass
        self.stdout.close()


class RelayHandler(StreamRequestHandler):
    def handle(self):
        self.request.settimeout(5)
        try:
            mode = self.request.recv(1)
            if mode == b"C":
                self.request.settimeout(None)
                with subprocess.Popen(
                    ["codex", "app-server", "--listen", "stdio://"],
                    stdin=self.request, stdout=self.request, stderr=subprocess.DEVNULL,
                ) as process:
                    process.wait()
            elif mode == b"S":
                state = desktop_state(self.server.codex_home)
                self.request.sendall(json.dumps({key: state[key] for key in STATE_FIELDS if key in state}).encode())
            elif mode == b"I":
                from images import Attachment, ImagePreviews

                request = json.loads(self.rfile.readline(8193))
                project = Path(request["project"]).resolve() if request["project"] else None
                if project is not None:
                    state = desktop_state(self.server.codex_home)
                    roots = (Path(root).resolve() for item in state.get("local-projects", {}).values()
                             for root in item.get("rootPaths", []))
                    if not any(project.is_relative_to(root) for root in roots):
                        project = None
                source = ImagePreviews.open_source(Attachment(request["path"]), project, self.server.codex_home)
                if source is not None:
                    self.request.sendall(source.getbuffer())
        except (OSError, ValueError, KeyError, TypeError):
            return


def main():
    path = Path(sys.argv[1])
    os.umask(0o077)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_socket():
        try:
            with connect(path):
                pass
        except ConnectionRefusedError:
            path.unlink()
        else:
            raise OSError("Codex host relay is already running")
    server = ThreadingUnixStreamServer(str(path), RelayHandler)
    server.codex_home = Path(os.environ.get("CODEX_HOME", Path.home() / ".codex"))
    signal.signal(signal.SIGTERM, lambda *_: Thread(target=server.shutdown, daemon=True).start())
    try:
        with server:
            server.serve_forever()
    finally:
        path.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
