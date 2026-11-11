"""Serve the local Codex client to the 3DS client."""

import argparse
import errno
from datetime import datetime
from contextlib import nullcontext
from ipaddress import IPv4Address, IPv4Network
import os
from pathlib import Path
import ssl
import signal
from time import sleep
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import re
from secrets import compare_digest, token_hex
from urllib.parse import parse_qs, urlsplit

from codex import CodexError, Library, MAX_REVIEW_BYTES, thread_title
from voice import MAX_AUDIO_BYTES, Transcriber, VoiceError
from lan import DEVICE_ID, DeviceStore, LanServer
from images import Attachment
from discovery import advertise


PAGE_SIZE = 9
MAX_MESSAGE_BYTES = 8192
MAX_COPY_RANGES = 256
TOKEN = re.compile(r"[a-zA-Z0-9_-]{1,96}\Z")


def clean_text(value):
    return "".join(char if char in ("\n", "\t") or char >= " " and char != "\x7f" else " "
                   for char in value.replace("\r\n", "\n").replace("\r", "\n"))


def label(value):
    return " ".join(clean_text(value).split())[:100]


def page_body(title, page, items):
    start = page * PAGE_SIZE
    lines = [f"{key}\t{label(name)}" for key, name in items[start:start + PAGE_SIZE]]
    return label(title), len(items) > start + PAGE_SIZE, "\n".join(lines).encode(), "idle", "", ""


def history_payload(messages, preview=None):
    data = bytearray()
    ranges = []
    images = []
    metadata_bytes = 0
    for role, message in messages:
        header = (("\n\n" if data else "") + role + "\n").encode()
        picture = isinstance(message, Attachment)
        metadata = ""
        inline_thumb = False
        if picture:
            result = preview(message) if preview and len(images) < 16 else None
            if result:
                token, (width, height, thumb) = result
                offset = len(data) + len(header)
                metadata = f"{offset},{token},{width},{height},{thumb}"
                inline_thumb = len(images) < 4 and (
                    len(data) + len(header) + metadata_bytes + len(metadata) + 32 <= MAX_REVIEW_BYTES
                )
                if not inline_thumb:
                    metadata = f"{offset},{token},0,0,"
            message = " \n \n图片附件" if inline_thumb else "图片附件" if metadata else "［图片暂不可预览］"
        text = clean_text(message).encode()
        cost = len(metadata) + 1 if metadata else 0
        if len(data) + len(header) + len(text) + metadata_bytes + cost > MAX_REVIEW_BYTES:
            remaining = MAX_REVIEW_BYTES - len(data) - len(header) - metadata_bytes
            if remaining > 0:
                data.extend(header + text[:remaining].decode("utf-8", errors="ignore").encode())
            data.extend("\n\n［本页超出显示容量］".encode())
            break
        start = len(data) + (2 if data else 0)
        body = len(data) + len(header)
        data.extend(header + text)
        if metadata:
            images.append(metadata)
            metadata_bytes += cost
        if not picture and len(ranges) < MAX_COPY_RANGES:
            ranges.append(f"{start},{body},{len(data)}")
    if not data:
        data.extend("这一页没有文字消息。".encode())
    index = ";".join(ranges)
    if images:
        index += "|" + ";".join(images)
    return bytes(data), index


class Handler(BaseHTTPRequestHandler):
    server_version = "3DS-Codex"

    def valid_host(self):
        host = self.headers.get("Host", "")
        address, port = self.server.server_address
        names = (address,) if isinstance(self.server, LanServer) else (address, "localhost")
        return any(host in (name, f"{name}:{port}") for name in names)

    def authorized(self):
        if not self.valid_host():
            return False
        if isinstance(self.server, LanServer):
            return getattr(self.connection, "lan_direct", False) or bool(
                self.server.devices.key(getattr(self.connection, "device_identity", None)))
        return True

    def pair(self):
        if (not isinstance(self.server, LanServer) or not self.valid_host()
                or getattr(self.connection, "device_identity", None) != "pair"
                or self.headers.get("Content-Length") != "0"):
            self.respond(403, "请使用一次性配对码连接。".encode())
            return
        try:
            credentials = self.server.enroll()
        except (OSError, ValueError):
            self.respond(503, "电脑无法保存配对，请检查设备文件。".encode())
            return
        if credentials is None:
            self.respond(409, "配对码已使用或过期。".encode())
            return
        device_id, key = credentials
        self.respond(200, f"3ds-codex/pair/1\n{device_id}\n{key}\n".encode())

    def valid_session(self):
        session = self.headers.get("X-3DS-Session", "")
        return (self.authorized() and re.fullmatch(r"[0-9a-f]{32}", session)
                and compare_digest(session, self.server.session))

    def transcribe(self):
        length = self.headers.get("Content-Length", "")
        if (not re.fullmatch(r"[0-9]{1,7}", length)
                or not 2 <= int(length) <= MAX_AUDIO_BYTES or int(length) % 2
                or self.headers.get_content_type() != "application/octet-stream"
                or self.headers.get("Transfer-Encoding")):
            self.respond(400, "录音数据格式无效。".encode())
            return
        self.connection.settimeout(12)
        try:
            pcm = self.rfile.read(int(length))
        except TimeoutError:
            self.respond(400, "录音上传超时。".encode())
            return
        if len(pcm) != int(length):
            self.respond(400, "录音上传不完整。".encode())
            return
        try:
            transcript = clean_text(self.server.transcriber.transcribe(pcm)).strip()
        except VoiceError as error:
            print(f"语音转写失败：{error}", flush=True)
            self.respond(503, str(error).encode())
            return
        data = transcript.encode()
        if not data or len(data) > MAX_MESSAGE_BYTES:
            self.respond(400, "转写为空或超出草稿容量。".encode())
            return
        self.respond(200, b"3ds-codex/voice/1\n" + data)

    def do_POST(self):
        if self.path == "/pair":
            self.pair()
            return
        if not self.valid_session():
            self.respond(409, "连接会话已更换，请重新打开对话。".encode())
            return
        if self.path == "/voice/transcribe":
            self.transcribe()
            return
        match = re.fullmatch(r"/threads/([a-zA-Z0-9_-]{1,96})/send", self.path)
        decision_route = re.fullmatch(r"/threads/([a-zA-Z0-9_-]{1,96})/approval/([0-9a-f]{32})", self.path)
        answer = re.fullmatch(r"/threads/([a-zA-Z0-9_-]{1,96})/input/([0-9a-f]{32})", self.path)
        control = re.fullmatch(r"/threads/([a-zA-Z0-9_-]{1,96})/turns/([a-zA-Z0-9_-]{1,96})/(steer|stop)", self.path)
        if not (match or decision_route or control or answer):
            self.respond(404, "这个接口不存在。".encode())
            return
        operation = self.headers.get("X-3DS-Operation", "")
        length = self.headers.get("Content-Length", "")
        if (not TOKEN.fullmatch(operation) or not re.fullmatch(r"[0-9]{1,4}", length)
                or not 1 <= int(length) <= MAX_MESSAGE_BYTES
                or self.headers.get_content_type() != "text/plain"
                or self.headers.get("Transfer-Encoding")):
            self.respond(400, "消息格式无效。".encode())
            return
        self.connection.settimeout(3)
        try:
            body = self.rfile.read(int(length))
            if len(body) != int(length):
                self.respond(400, "消息没有完整送达。".encode())
                return
            text = body.decode("utf-8")
        except (TimeoutError, UnicodeDecodeError):
            self.respond(400, "无法读取消息内容。".encode())
            return
        if not text.strip() or "\x00" in text:
            self.respond(400, "消息为空或内容无效。".encode())
            return
        library = self.server.library
        if answer:
            try:
                with library.lock:
                    resolved = library.app.answer_input(*answer.groups(), body)
            except KeyError as error:
                self.respond(404, str(error.args[0]).encode())
            except CodexError as error:
                self.respond(409, str(error).encode())
            else:
                marker = "resolved" if resolved else "unconfirmed"
                self.respond(200, f"3ds-codex/answer/1\n{marker}\n".encode())
            return
        if control:
            if control[3] == "stop" and text != "stop":
                self.respond(400, "停止请求无效。".encode())
                return
            try:
                with library.lock:
                    status = library.control_turn(*control.groups(), operation, text)
            except CodexError as error:
                self.respond(409, str(error).encode())
            else:
                self.respond(200, f"3ds-codex/control/1\n{status}\n".encode())
            return
        if decision_route:
            if text not in ("accept", "decline", "cancel"):
                self.respond(400, "审批决定无效。".encode())
                return
            try:
                with library.lock:
                    resolved = library.app.decide_approval(decision_route[1], decision_route[2], text)
            except KeyError as error:
                self.respond(404, str(error.args[0]).encode())
            except CodexError as error:
                self.respond(409, str(error).encode())
            else:
                marker = "resolved" if resolved else "unconfirmed"
                self.respond(200, f"3ds-codex/decision/1\n{marker}\n".encode())
            return
        try:
            with library.lock:
                status = library.send_message(match[1], operation, text)
        except KeyError:
            self.respond(404, "这条对话已不可用。".encode())
            return
        except CodexError as error:
            self.respond(409, str(error).encode())
            return
        self.respond(200, f"3ds-codex/send/1\n{status}\n".encode())

    def do_GET(self):
        if not self.authorized():
            self.respond(403, "请使用已配对的电脑连接。".encode())
            return

        target = urlsplit(self.path)
        if target.path == "/health":
            self.respond(200, b"3ds-codex-bridge/1\n")
            return
        image = re.fullmatch(r"/threads/([a-zA-Z0-9_-]{1,96})/images/([0-9a-f]{32})", target.path)
        if image:
            with self.server.library.lock:
                payload = self.server.library.image_previews.get(*image.groups())
            if payload is None:
                self.respond(404, "图片预览已失效，请重新打开对话。".encode())
            else:
                self.respond(200, payload, "application/octet-stream")
            return
        approval = re.fullmatch(r"/threads/([a-zA-Z0-9_-]{1,96})/approval", target.path)
        if approval:
            self.get_approval(approval[1])
            return
        prompt = re.fullmatch(r"/threads/([a-zA-Z0-9_-]{1,96})/input", target.path)
        if prompt:
            self.get_input(prompt[1])
            return
        query = parse_qs(target.query)
        page = query.get("page", ["0"])[0]
        if not re.fullmatch(r"[0-9]{1,6}", page):
            self.respond(400, "页码无效。".encode())
            return

        library = self.server.library
        try:
            with library.lock:
                title, more, data, status, active_turn, ranges = self.route(library, target.path, int(page), query)
        except KeyError as error:
            self.respond(404, str(error.args[0]).encode())
            return
        except CodexError as error:
            self.respond(503, str(error).encode())
            return

        header = (f"3ds-codex/5\n{int(more)}\n{label(title)}\n{status}\n"
                  f"{self.server.session}\n{active_turn}\n{ranges}\n")
        self.respond(200, header.encode() + data)

    def get_input(self, thread_id):
        def single_line(value):
            return " ".join(clean_text(value).split())

        try:
            with self.server.library.lock:
                request = self.server.library.app.pending_input(thread_id)
            if request is None:
                raise KeyError("没有待回答的问题，请返回对话。")
        except KeyError as error:
            self.respond(404, str(error.args[0]).encode())
            return
        questions = request.questions
        if (not 1 <= len(questions) <= 16 or any(len(q.get("options") or []) > 32 for q in questions)
                or any(q.get("isSecret") for q in questions)):
            self.respond(409, "这个表单超出掌机支持范围，请在发起端处理。".encode())
            return
        lines = ["3ds-codex/input/1", request.token, str(len(questions))]
        for q in questions:
            options = q.get("options") or []
            lines.extend((single_line(q["header"]), single_line(q["question"]),
                          str(int(bool(q.get("isOther") or not options))), str(len(options))))
            for option in options:
                lines.extend((single_line(option["label"]), single_line(option["description"])))
        payload = ("\n".join(lines) + "\n").encode()
        if len(payload) > MAX_REVIEW_BYTES:
            self.respond(409, "问题太长，超出掌机显示容量。".encode())
        else:
            self.respond(200, payload)

    def get_approval(self, thread_id):
        library = self.server.library
        try:
            with library.lock:
                approval = library.app.pending_approval(thread_id)
                thread = library.threads.get(thread_id)
                if approval is None or thread is None:
                    raise KeyError("没有待处理的审批，请刷新对话。")
                kind, details, choices = approval.review()
                title = label(thread_title(thread))
                rejection = 1 if "decline" in choices else 2 if "cancel" in choices else 0
                flags = f"{int('accept' in choices)}{rejection}"
                response = f"3ds-codex/approval/2\n{approval.token}\n{flags}\n{kind}\n{title}\n{details}"
        except KeyError as error:
            self.respond(404, str(error.args[0]).encode())
            return
        self.respond(200, response.encode())

    def route(self, library, path, page, query):
        parts = path.strip("/").split("/")
        if parts == ["projects"]:
            library.refresh(force=query.get("refresh") == ["1"])
            return page_body("项目", page, [
                (key, f"{name}（{len(threads)} 条）")
                for key, (name, _, threads) in library.projects.items()
            ])
        if len(parts) == 3 and parts[0] == "projects" and parts[2] == "threads" and TOKEN.fullmatch(parts[1]):
            library.refresh()
            project = library.projects.get(parts[1])
            if project is None:
                raise KeyError("这个项目已不可用。")
            name, _, threads = project
            return page_body(name, page, [(thread["id"], thread_title(thread)) for thread in threads])
        if len(parts) == 2 and parts[0] == "threads" and TOKEN.fullmatch(parts[1]):
            library.refresh()
            thread = library.threads.get(parts[1])
            if thread is None:
                raise KeyError("这条对话已不可用。")
            messages, more, status = library.history(parts[1], page)
            project = Path(thread["cwd"]).resolve() if thread.get("cwd") else None
            body, ranges = history_payload(messages, lambda attachment: library.image_previews.register(
                parts[1], attachment, project, library.codex_home,
            ))
            return thread_title(thread), more, body, status, library.active_turn(parts[1]), ranges
        raise KeyError("这个接口不存在。")

    def respond(self, status, payload, content_type="text/plain; charset=utf-8"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, help="默认端口为本机 8765、局域网 8766")
    parser.add_argument("--lan-address", help="监听指定的局域网 IPv4 地址并启用加密")
    parser.add_argument("--pair", action="store_true", help="生成一次性配对码")
    parser.add_argument("--direct-network", type=IPv4Network,
                        default=os.environ.get("THREEDS_CODEX_DIRECT_NETWORK") or None,
                        help="允许此局域网网段的设备免配对直连")
    parser.add_argument("--wait-for-address", action="store_true", help="启动时等待局域网地址可用")
    parser.add_argument("--devices-file", default=Path(os.environ.get(
        "XDG_CONFIG_HOME", Path.home() / ".config")) / "3ds-codex" / "devices.json")
    management = parser.add_mutually_exclusive_group()
    management.add_argument("--list-devices", action="store_true", help="查看已配对设备")
    management.add_argument("--revoke", metavar="设备编号", help="撤销指定设备")
    parser.add_argument("--whisper-cli", default="whisper-cli")
    parser.add_argument("--whisper-model", help="Path to a local multilingual whisper.cpp model")
    parser.add_argument("--opencc-config", help="Optional OpenCC simplified-Chinese configuration")
    args = parser.parse_args()

    store = DeviceStore(args.devices_file)
    if args.list_devices:
        for device_id, record in store.read().items():
            print(device_id, datetime.fromtimestamp(record["paired_at"]).isoformat(timespec="seconds"))
        return
    if args.revoke:
        if not DEVICE_ID.fullmatch(args.revoke) or not store.revoke(args.revoke):
            parser.error("找不到该设备。")
        print("已撤销设备。")
        return
    if (args.pair or args.direct_network) and not args.lan_address:
        parser.error("配对和局域网直连需要同时指定 --lan-address。")
    if args.port is not None and not 0 <= args.port <= 65535:
        parser.error("端口必须在 0 至 65535 之间。")
    if args.lan_address:
        try:
            address = IPv4Address(args.lan_address)
            if not address.is_private or address.is_unspecified or address.is_multicast or address.is_reserved:
                raise ValueError
        except ValueError:
            parser.error("请输入电脑的局域网 IPv4 地址。")
        if args.direct_network is not None and (address not in args.direct_network or not args.direct_network.is_private):
            parser.error("直连网段必须是包含桥接地址的私有 IPv4 网段。")
        if not getattr(ssl, "HAS_PSK", False):
            parser.error("加密局域网模式需要支持 PSK 的 Python 3.13 或更新版本。")
        port = 8766 if args.port is None else args.port
        while True:
            try:
                server_instance = LanServer((str(address), port), Handler, store, args.pair, args.direct_network)
                break
            except OSError as error:
                if not args.wait_for_address or error.errno != errno.EADDRNOTAVAIL:
                    raise
                sleep(3)
    else:
        server_instance = ThreadingHTTPServer(("127.0.0.1", 8765 if args.port is None else args.port), Handler)
    library = Library()
    with server_instance as server:
        server.library = library
        server.transcriber = Transcriber(args.whisper_cli, args.whisper_model, args.opencc_config)
        server.session = token_hex(16)
        host, port = server.server_address
        if isinstance(server, LanServer):
            print(f"3DS Codex 加密局域网连接：{host}:{port}", flush=True)
            def enable_pairing(*_):
                print(f"一次性配对码（10 分钟有效）：{server.open_pairing()}", flush=True)

            if hasattr(signal, "SIGUSR1"):
                signal.signal(signal.SIGUSR1, enable_pairing)
            if server.code:
                print(f"一次性配对码（10 分钟有效）：{server.code}", flush=True)
        else:
            print(f"3DS Codex bridge: http://{host}:{port}/health", flush=True)
        try:
            discovery = advertise(host, port, server.direct_network) if isinstance(server, LanServer) else nullcontext()
            with discovery:
                server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            with library.lock:
                library.app.close()


if __name__ == "__main__":
    main()
