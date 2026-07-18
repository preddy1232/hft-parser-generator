#!/usr/bin/env python3
"""Generate a deterministic, length-prefixed ITCH replay fixture."""

import struct
import sys
from pathlib import Path


def header(message_type: bytes, timestamp: int) -> bytes:
    return message_type + struct.pack(">HH", 1, 1) + timestamp.to_bytes(6, "big")


def framed(body: bytes) -> bytes:
    return struct.pack(">H", len(body)) + body


def add(ref: int, side: bytes, shares: int, price: int, timestamp: int) -> bytes:
    return framed(header(b"A", timestamp) + struct.pack(">Q", ref) + side
                  + struct.pack(">I", shares) + b"TEST    " + struct.pack(">I", price))


def execute(ref: int, shares: int, timestamp: int) -> bytes:
    return framed(header(b"E", timestamp) + struct.pack(">QI", ref, shares)
                  + struct.pack(">Q", 1000 + timestamp))


def cancel(ref: int, shares: int, timestamp: int) -> bytes:
    return framed(header(b"X", timestamp) + struct.pack(">QI", ref, shares))


def delete(ref: int, timestamp: int) -> bytes:
    return framed(header(b"D", timestamp) + struct.pack(">Q", ref))


def replace(old_ref: int, new_ref: int, shares: int, price: int,
            timestamp: int) -> bytes:
    return framed(header(b"U", timestamp)
                  + struct.pack(">QQII", old_ref, new_ref, shares, price))


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} OUTPUT_FILE")
    output = Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    messages = [
        add(1, b"B", 100, 1_000_000, 1),
        add(2, b"S", 80, 1_010_000, 2),
        execute(1, 40, 3),
        replace(2, 3, 50, 1_005_000, 4),
        cancel(3, 10, 5),
        delete(1, 6),
    ]
    output.write_bytes(b"".join(messages))
    print(f"wrote {len(messages)} messages ({output.stat().st_size} bytes) to {output}")


if __name__ == "__main__":
    main()
