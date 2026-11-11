"""Advertise the encrypted LAN endpoint without exposing any device credentials."""

from contextlib import contextmanager
import socket
import struct
from ipaddress import IPv4Address
from threading import Event, Thread


DISCOVERY_PORT = 18765
QUERY = b"3DS-CODEX-DISCOVER-1"
REPLY = b"3DS-CODEX-BRIDGE-1 "


@contextmanager
def advertise(address, port, direct_network=None):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        interface = getattr(socket, "IP_PKTINFO", None)
        if interface is not None:
            listener.setsockopt(socket.IPPROTO_IP, interface, 1)
        listener.bind(("", DISCOVERY_PORT))
        listener.settimeout(0.5)
        stop = Event()
        reply = REPLY + str(port).encode()
        local_address = socket.inet_aton(address)

        def run():
            while not stop.is_set():
                try:
                    if interface is None:
                        data, peer = listener.recvfrom(64)
                    else:
                        data, info, _, peer = listener.recvmsg(64, socket.CMSG_SPACE(12))
                        if peer[0] != "127.0.0.1" and not any(
                            level == socket.IPPROTO_IP and kind == interface and
                            len(packet) >= 12 and packet[4:8] == local_address
                            for level, kind, packet in info
                        ):
                            continue
                    if data == QUERY:
                        direct = direct_network is not None and (
                            peer[0] == "127.0.0.1" or IPv4Address(peer[0]) in direct_network
                        )
                        payload = reply + (b" direct" if direct else b"")
                        if peer[0] == "127.0.0.1" and interface is not None:
                            route = struct.pack("=I4s4s", 0, local_address, bytes(4))
                            listener.sendmsg([payload], [(socket.IPPROTO_IP, interface, route)], 0, peer)
                        else:
                            listener.sendto(payload, peer)
                except TimeoutError:
                    pass
                except OSError:
                    return

        thread = Thread(target=run, name="lan-discovery", daemon=True)
        thread.start()
        try:
            yield
        finally:
            stop.set()
            thread.join()
