#!/usr/bin/env python3
"""Small, dependency-free UDP relay for BedrockToolsPlus Voice Chat v1.

The relay routes opaque XChaCha20-Poly1305 voice datagrams between endpoints in
the same room. It never receives room keys and cannot decrypt voice payloads.
It is intentionally not a Minecraft server plugin: run it beside a server, on a
VPS, or on a reachable LAN computer and open the configured UDP port.
"""

from __future__ import annotations

import argparse
import asyncio
from dataclasses import dataclass, field
import logging
import secrets
import signal
import struct
import time
from typing import Optional

LOG = logging.getLogger("voicechat-relay")
MAGIC = b"BTVC"
VERSION = 1
PREFIX_SIZE = 6
JOIN = 1
WELCOME = 2
VOICE = 3
PING = 4
PONG = 5
LEAVE = 6
MAX_DATAGRAM = 1200
MAX_ROOM_BYTES = 64
MAX_NAME_BYTES = 32
JOIN_FIXED_SIZE = PREFIX_SIZE + 8 + 2 + 1
CLIENT_PACKET_SIZE = PREFIX_SIZE + 8
VOICE_PACKET_SIZE = 714  # 54-byte header/tag + 660-byte encrypted PCM frame


def prefix(packet_type: int) -> bytes:
    return MAGIC + bytes((VERSION, packet_type))


def valid_prefix(data: bytes, packet_type: int) -> bool:
    return len(data) >= PREFIX_SIZE and data[:PREFIX_SIZE] == prefix(packet_type)


def valid_text(raw: bytes, maximum: int) -> bool:
    if not raw or len(raw) > maximum:
        return False
    try:
        value = raw.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return all(ord(character) >= 0x20 and ord(character) != 0x7F for character in value)


@dataclass
class Client:
    client_id: int
    address: tuple
    room: str
    name: str
    last_seen: float
    tokens: float = 100.0
    last_rate_update: float = field(default_factory=time.monotonic)


@dataclass
class Room:
    clients: dict[int, Client] = field(default_factory=dict)


class VoiceRelay(asyncio.DatagramProtocol):
    def __init__(
        self,
        *,
        max_room_clients: int = 32,
        max_total_clients: int = 512,
        idle_timeout: float = 15.0,
        packets_per_second: float = 80.0,
    ) -> None:
        self.max_room_clients = max_room_clients
        self.max_total_clients = max_total_clients
        self.idle_timeout = idle_timeout
        self.packets_per_second = packets_per_second
        self.rooms: dict[str, Room] = {}
        self.endpoints: dict[tuple, Client] = {}
        self.transport: Optional[asyncio.DatagramTransport] = None
        self.cleanup_task: Optional[asyncio.Task] = None

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.transport = transport  # type: ignore[assignment]
        self.cleanup_task = asyncio.create_task(self.cleanup_loop())
        LOG.info("UDP voice relay is ready")

    def connection_lost(self, exc: Optional[Exception]) -> None:
        if self.cleanup_task:
            self.cleanup_task.cancel()
        if exc:
            LOG.error("Relay transport stopped: %s", exc)

    def allow_packet(self, client: Client) -> bool:
        now = time.monotonic()
        elapsed = max(0.0, now - client.last_rate_update)
        client.tokens = min(100.0, client.tokens + elapsed * self.packets_per_second)
        client.last_rate_update = now
        if client.tokens < 1.0:
            return False
        client.tokens -= 1.0
        return True

    def remove_client(self, client: Client) -> None:
        self.endpoints.pop(client.address, None)
        room = self.rooms.get(client.room)
        if not room:
            return
        room.clients.pop(client.client_id, None)
        if not room.clients:
            self.rooms.pop(client.room, None)

    def room_for(self, name: str) -> Room:
        return self.rooms.setdefault(name, Room())

    def welcome(self, client: Client, request_id: int) -> None:
        if not self.transport:
            return
        room = self.rooms[client.room]
        count = min(len(room.clients), 0xFFFF)
        packet = prefix(WELCOME) + struct.pack("<QQH", client.client_id, request_id, count)
        self.transport.sendto(packet, client.address)

    def datagram_received(self, data: bytes, addr: tuple) -> None:
        if len(data) > MAX_DATAGRAM or len(data) < PREFIX_SIZE or data[:4] != MAGIC:
            return
        if data[4] != VERSION:
            return
        packet_type = data[5]
        now = time.monotonic()

        if packet_type == JOIN:
            self.handle_join(data, addr, now)
            return

        client = self.endpoints.get(addr)
        if not client or len(data) < CLIENT_PACKET_SIZE:
            return
        client_id = struct.unpack_from("<Q", data, PREFIX_SIZE)[0]
        if client_id != client.client_id or not self.allow_packet(client):
            return

        if packet_type == PING and len(data) == CLIENT_PACKET_SIZE and valid_prefix(data, PING):
            client.last_seen = now
            room = self.rooms.get(client.room)
            if self.transport and room:
                count = min(len(room.clients), 0xFFFF)
                self.transport.sendto(prefix(PONG) + struct.pack("<QH", client_id, count), addr)
            return

        if packet_type == LEAVE and len(data) == CLIENT_PACKET_SIZE and valid_prefix(data, LEAVE):
            self.remove_client(client)
            return

        if packet_type != VOICE or len(data) != VOICE_PACKET_SIZE or not valid_prefix(data, VOICE):
            return
        client.last_seen = now
        room = self.rooms.get(client.room)
        if not room or not self.transport:
            return
        # Forward the original datagram unchanged: its authenticated header
        # contains the sender ID and nonce, and the relay has no voice key.
        for recipient in tuple(room.clients.values()):
            if recipient.client_id != client_id:
                self.transport.sendto(data, recipient.address)

    def handle_join(self, data: bytes, addr: tuple, now: float) -> None:
        if not valid_prefix(data, JOIN) or len(data) < JOIN_FIXED_SIZE:
            return
        request_id = struct.unpack_from("<Q", data, 6)[0]
        room_size = struct.unpack_from("<H", data, 14)[0]
        name_size = data[16]
        if request_id == 0 or room_size == 0 or room_size > MAX_ROOM_BYTES:
            return
        if name_size == 0 or name_size > MAX_NAME_BYTES:
            return
        if len(data) != JOIN_FIXED_SIZE + room_size + name_size:
            return
        room_raw = data[17 : 17 + room_size]
        name_raw = data[17 + room_size :]
        if not valid_text(room_raw, MAX_ROOM_BYTES) or not valid_text(name_raw, MAX_NAME_BYTES):
            return
        room_name = room_raw.decode("utf-8")
        player_name = name_raw.decode("utf-8")

        existing = self.endpoints.get(addr)
        if existing and existing.room == room_name:
            existing.name = player_name
            existing.last_seen = now
            self.welcome(existing, request_id)
            return
        if existing:
            self.remove_client(existing)
        if len(self.endpoints) >= self.max_total_clients:
            LOG.warning("Ignoring join from %s: relay client limit reached", addr)
            return

        room = self.room_for(room_name)
        if len(room.clients) >= self.max_room_clients:
            if not room.clients:
                self.rooms.pop(room_name, None)
            LOG.warning("Ignoring join to full room from %s", addr)
            return

        client_id = 0
        while client_id == 0 or client_id in room.clients:
            client_id = secrets.randbits(64)
        client = Client(client_id, addr, room_name, player_name, now)
        room.clients[client_id] = client
        self.endpoints[addr] = client
        LOG.info("%s joined room %r as %s (%d online)", player_name, room_name, addr, len(room.clients))
        self.welcome(client, request_id)

    async def cleanup_loop(self) -> None:
        try:
            while True:
                await asyncio.sleep(max(1.0, min(5.0, self.idle_timeout / 2)))
                deadline = time.monotonic() - self.idle_timeout
                expired = [client for client in self.endpoints.values() if client.last_seen < deadline]
                for client in expired:
                    LOG.info("Expiring idle voice client %s", client.address)
                    self.remove_client(client)
        except asyncio.CancelledError:
            pass

    def close(self) -> None:
        if self.cleanup_task:
            self.cleanup_task.cancel()
        if self.transport:
            self.transport.close()


async def run(args: argparse.Namespace) -> None:
    loop = asyncio.get_running_loop()
    protocol = VoiceRelay(
        max_room_clients=args.max_room_clients,
        max_total_clients=args.max_total_clients,
        idle_timeout=args.idle_timeout,
        packets_per_second=args.packets_per_second,
    )
    transport, _ = await loop.create_datagram_endpoint(
        lambda: protocol,
        local_addr=(args.host, args.port),
    )
    protocol.transport = transport
    if protocol.cleanup_task is None:
        protocol.cleanup_task = asyncio.create_task(protocol.cleanup_loop())

    sockets = transport.get_extra_info("socket")
    if sockets is not None:
        LOG.info("Listening on UDP %s", sockets.getsockname())
    stop = asyncio.Event()
    for signum in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(signum, stop.set)
        except NotImplementedError:
            pass
    try:
        await stop.wait()
    finally:
        protocol.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0", help="UDP listen address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=39741, help="UDP listen port (default: 39741)")
    parser.add_argument("--max-room-clients", type=int, default=32)
    parser.add_argument("--max-total-clients", type=int, default=512)
    parser.add_argument("--idle-timeout", type=float, default=15.0)
    parser.add_argument("--packets-per-second", type=float, default=80.0)
    parser.add_argument("--log-level", default="INFO", choices=("DEBUG", "INFO", "WARNING", "ERROR"))
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.max_room_clients < 2 or args.max_total_clients < 2:
        parser.error("client limits must be at least 2")
    if args.idle_timeout < 5.0:
        parser.error("--idle-timeout must be at least 5 seconds")
    if args.packets_per_second < 55.0:
        parser.error("--packets-per-second must be at least 55 (voice uses 50 packets/s)")
    return args


def main() -> None:
    args = parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
