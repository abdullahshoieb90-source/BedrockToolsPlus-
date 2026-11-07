"""Integration tests for the dependency-free UDP voice relay.

Run with: python3 -m unittest discover -s tests -p 'test_*.py'
"""

import os
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"BTVC"
VERSION = 1


def packet(kind, payload=b""):
    return MAGIC + bytes((VERSION, kind)) + payload


def join_packet(request_id, room="friends", name="Steve"):
    room_raw = room.encode("utf-8")
    name_raw = name.encode("utf-8")
    return packet(1, struct.pack("<QHB", request_id, len(room_raw), len(name_raw)) + room_raw + name_raw)


class VoiceRelayIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Reserve an available loopback port, then release it for the child.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            cls.port = probe.getsockname()[1]
        cls.process = subprocess.Popen(
            [sys.executable, str(ROOT / "tools/voicechat_relay.py"),
             "--host", "127.0.0.1", "--port", str(cls.port), "--log-level", "ERROR"],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env={**os.environ, "PYTHONUNBUFFERED": "1"},
        )
        cls.address = ("127.0.0.1", cls.port)
        deadline = time.monotonic() + 5
        while cls.process.poll() is None and time.monotonic() < deadline:
            try:
                with cls.client() as ready:
                    ready.sendto(join_packet(1, "readiness", "probe"), cls.address)
                    ready.settimeout(0.1)
                    ready.recvfrom(1200)
                    return
            except TimeoutError:
                time.sleep(0.05)
        raise RuntimeError("voice relay did not start")

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        try:
            cls.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            cls.process.kill()
            cls.process.wait(timeout=3)

    @staticmethod
    def client():
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(1.0)
        sock.bind(("127.0.0.1", 0))
        return sock

    def join(self, sock, request_id, room="friends", name="Steve"):
        sock.sendto(join_packet(request_id, room, name), self.address)
        data, source = sock.recvfrom(1200)
        self.assertEqual(source, self.address)
        self.assertEqual(data[:6], packet(2))
        client_id, returned_request, peers = struct.unpack_from("<QQH", data, 6)
        self.assertEqual(returned_request, request_id)
        self.assertGreater(client_id, 0)
        self.assertGreaterEqual(peers, 1)
        return client_id, peers

    def test_join_ping_forward_leave_and_isolation(self):
        with self.client() as first, self.client() as second, self.client() as outsider:
            first_id, peers = self.join(first, 101, name="First")
            self.assertEqual(peers, 1)
            second_id, peers = self.join(second, 102, name="Second")
            self.assertEqual(peers, 2)
            outsider_id, peers = self.join(outsider, 103, room="another", name="Outsider")
            self.assertEqual(peers, 1)

            # Rejoining from the same endpoint is idempotent, which makes UDP
            # join retransmissions safe.
            duplicate_id, _ = self.join(first, 104, name="First renamed")
            self.assertEqual(duplicate_id, first_id)

            first.sendto(packet(4, struct.pack("<Q", first_id)), self.address)
            pong, _ = first.recvfrom(1200)
            self.assertEqual(pong[:6], packet(5))
            self.assertEqual(struct.unpack_from("<QH", pong, 6), (first_id, 2))

            # The relay treats voice ciphertext as opaque but validates the
            # outer sender ID and exact v1 frame size before forwarding it.
            voice = bytearray(secrets.token_bytes(714))
            voice[:6] = packet(3)
            struct.pack_into("<Q", voice, 6, first_id)
            first.sendto(voice, self.address)
            forwarded, _ = second.recvfrom(1200)
            self.assertEqual(forwarded, bytes(voice))

            first.settimeout(0.15)
            with self.assertRaises(TimeoutError):
                first.recvfrom(1200)  # no sender echo
            outsider.settimeout(0.15)
            with self.assertRaises(TimeoutError):
                outsider.recvfrom(1200)  # room isolation

            # A datagram claiming another endpoint's client ID is ignored.
            forged = bytearray(voice)
            struct.pack_into("<Q", forged, 6, second_id)
            first.sendto(forged, self.address)
            second.settimeout(0.15)
            with self.assertRaises(TimeoutError):
                second.recvfrom(1200)

            second.sendto(packet(6, struct.pack("<Q", second_id)), self.address)
            time.sleep(0.05)
            first.settimeout(1.0)
            first.sendto(packet(4, struct.pack("<Q", first_id)), self.address)
            pong, _ = first.recvfrom(1200)
            self.assertEqual(struct.unpack_from("<QH", pong, 6), (first_id, 1))

            outsider.sendto(packet(6, struct.pack("<Q", outsider_id)), self.address)


if __name__ == "__main__":
    unittest.main()
