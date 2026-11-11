"""Device enrollment and the authenticated LAN listener."""

import base64
import fcntl
from http.server import ThreadingHTTPServer
import json
from ipaddress import IPv4Address
import os
from pathlib import Path
import re
from secrets import token_bytes, token_hex
import ssl
from tempfile import NamedTemporaryFile
from threading import Lock, local
from time import monotonic, time


DEVICE_ID = re.compile(r"[0-9a-f]{16}\Z")
DEVICE_KEY = re.compile(r"[0-9a-f]{64}\Z")
PAIR_SECONDS = 600
TLS_CIPHER = "PSK-AES128-GCM-SHA256"
DIRECT_CIPHER = "ECDHE-PSK-AES128-CBC-SHA256"
DIRECT_IDENTITY = "lan-direct"
DIRECT_KEY = b"3DS-CODEX-LAN-DIRECT-V1"


class DeviceStore:
    def __init__(self, path):
        self.path = Path(path).expanduser()

    def read(self):
        try:
            with self.path.open(encoding="utf-8") as source:
                return json.load(source)
        except FileNotFoundError:
            return {}

    def key(self, device_id):
        if not DEVICE_ID.fullmatch(device_id or ""):
            return b""
        try:
            key = self.read().get(device_id, {}).get("key", "")
            return bytes.fromhex(key) if DEVICE_KEY.fullmatch(key) else b""
        except (OSError, ValueError, TypeError, AttributeError):
            return b""

    def update(self, change):
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        lock_path = self.path.with_name(self.path.name + ".lock")
        with os.fdopen(os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o600), "r+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            records = self.read()
            result = change(records)
            temp_path = None
            try:
                with NamedTemporaryFile(mode="w", encoding="utf-8", dir=self.path.parent,
                                        prefix=".devices-", delete=False) as destination:
                    temp_path = Path(destination.name)
                    json.dump(records, destination, indent=2)
                    destination.flush()
                    os.fsync(destination.fileno())
                os.replace(temp_path, self.path)
            finally:
                if temp_path is not None:
                    temp_path.unlink(missing_ok=True)
            return result

    def enroll(self):
        def add(records):
            device_id = token_hex(8)
            while device_id in records:
                device_id = token_hex(8)
            key = token_hex(32)
            records[device_id] = {"key": key, "paired_at": int(time())}
            return device_id, key

        return self.update(add)

    def revoke(self, device_id):
        return self.update(lambda records: records.pop(device_id, None) is not None)


class LanServer(ThreadingHTTPServer):
    def __init__(self, address, handler, store, pairing, direct_network=None):
        self.devices = store
        self.direct_network = direct_network
        self.code = None
        self.pair_until = 0
        self.pair_lock = Lock()
        if pairing:
            self.open_pairing()
        self.connection = local()
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.minimum_version = self.context.maximum_version = ssl.TLSVersion.TLSv1_2
        self.context.set_ciphers(f"{TLS_CIPHER}:{DIRECT_CIPHER}" if direct_network else TLS_CIPHER)
        self.context.options |= ssl.OP_NO_TICKET
        self.context.set_psk_server_callback(self.psk)
        super().__init__(address, handler)

    def open_pairing(self):
        with self.pair_lock:
            self.code = base64.b32encode(token_bytes(13)).decode()[:20]
            self.pair_until = monotonic() + PAIR_SECONDS
            return self.code

    def psk(self, identity):
        if identity == DIRECT_IDENTITY:
            if self.connection.direct_allowed:
                self.connection.identity = DIRECT_IDENTITY
                return DIRECT_KEY
            return b""
        if identity == "pair":
            with self.pair_lock:
                if self.code and monotonic() < self.pair_until:
                    self.connection.identity = "pair"
                    return self.code.encode()
            return b""
        key = self.devices.key(identity)
        if key:
            self.connection.identity = identity
        return key

    def enroll(self):
        with self.pair_lock:
            if not self.code or monotonic() >= self.pair_until:
                return None
            device = self.devices.enroll()
            self.code = None
            return device

    def process_request_thread(self, request, client_address):
        self.connection.identity = None
        self.connection.direct_allowed = (self.direct_network is not None and
                                          IPv4Address(client_address[0]) in self.direct_network)
        try:
            request.settimeout(8)
            with self.context.wrap_socket(request, server_side=True) as connection:
                if self.connection.identity is None:
                    return
                connection.device_identity = self.connection.identity
                connection.lan_direct = connection.device_identity == DIRECT_IDENTITY
                if connection.lan_direct and connection.cipher()[0] != DIRECT_CIPHER:
                    return
                self.finish_request(connection, client_address)
        except (OSError, ssl.SSLError):
            pass
        except Exception:
            self.handle_error(request, client_address)
        finally:
            self.connection.identity = None
            self.close_request(request)
