#!/usr/bin/env python3
"""Minimal IPC server that pushes a fake channel map to the driver.
Replaces the real app for dev/testing. Stays running so the driver can
reconnect after restarts."""

import os
import signal
import socket
import struct
import sys
import threading

SOCK_PATH = "/tmp/gla-injector.sock"
GLA_MSG_CHANNEL_MAP_UPDATE = 1

# struct GLAChannelEntry layout (80 bytes):
#   uint8_t  channelIndex  @ offset 0
#   [7 bytes padding]
#   uint64_t entityId      @ offset 8
#   char     displayName[64] @ offset 16
ENTRY_SIZE = 80

TEST_CHANNELS = [
    (0, 0x0000000000000001, "Test Guitar"),
    (1, 0x0000000000000002, "Test Vocals"),
]


def make_entry(channel_index, entity_id, display_name):
    entry = bytearray(ENTRY_SIZE)
    struct.pack_into("B", entry, 0, channel_index)
    struct.pack_into("<Q", entry, 8, entity_id)
    name_bytes = display_name.encode("utf-8")[:63]
    entry[16 : 16 + len(name_bytes)] = name_bytes
    return bytes(entry)


def build_channel_map_msg(channels):
    payload = struct.pack("<I", GLA_MSG_CHANNEL_MAP_UPDATE)
    payload += struct.pack("<I", len(channels))
    for ch in channels:
        payload += make_entry(*ch)
    return struct.pack("<I", len(payload)) + payload


TEST_MSG = build_channel_map_msg(TEST_CHANNELS)


def serve_client(conn):
    try:
        print("[inject-test-map] driver connected — sending channel map")
        conn.sendall(TEST_MSG)
        while conn.recv(4096):
            pass
    except OSError:
        pass
    finally:
        conn.close()
        print("[inject-test-map] driver disconnected")


def main():
    try:
        os.unlink(SOCK_PATH)
    except FileNotFoundError:
        pass

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCK_PATH)
    server.listen(8)

    print(f"[inject-test-map] listening on {SOCK_PATH}")
    print(f"[inject-test-map] {len(TEST_CHANNELS)} test channels:")
    for idx, (ch, eid, name) in enumerate(TEST_CHANNELS):
        print(f"    Ch{ch}  entity=0x{eid:016x}  '{name}'")
    print("[inject-test-map] Ctrl-C to stop\n")

    def shutdown(*_):
        server.close()
        try:
            os.unlink(SOCK_PATH)
        except OSError:
            pass
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    while True:
        try:
            conn, _ = server.accept()
            threading.Thread(target=serve_client, args=(conn,), daemon=True).start()
        except OSError:
            break


if __name__ == "__main__":
    main()
