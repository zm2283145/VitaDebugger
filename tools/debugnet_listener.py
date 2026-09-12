#!/usr/bin/env python3
"""Small cross-platform receiver for VitaDebugger's DebugNet-compatible logs."""

import argparse
import datetime
import ipaddress
import socket


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0", help="local address to bind")
    parser.add_argument("--port", type=int, default=18194, help="UDP port")
    parser.add_argument("--buffer", type=int, default=2048, help="maximum datagram size")
    parser.add_argument("--source", help="accept packets only from this Vita IPv4 address")
    args = parser.parse_args()

    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if not 1 <= args.buffer <= 65535:
        parser.error("--buffer must be between 1 and 65535")
    if args.source:
        try:
            ipaddress.IPv4Address(args.source)
        except ipaddress.AddressValueError as error:
            parser.error(f"invalid --source address: {error}")

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
        receiver.bind((args.bind, args.port))
        print(f"Listening for VitaDebugger UDP logs on {args.bind}:{args.port}")
        try:
            while True:
                payload, source = receiver.recvfrom(args.buffer)
                if args.source and source[0] != args.source:
                    continue
                timestamp = datetime.datetime.now().astimezone().isoformat(timespec="milliseconds")
                decoded = payload.decode("utf-8", errors="replace").rstrip("\r\n")
                message = "".join(
                    character
                    if character == "\t" or character.isprintable()
                    else f"\\x{ord(character):02x}"
                    for character in decoded
                )
                print(f"{timestamp} {source[0]}:{source[1]} {message}", flush=True)
        except KeyboardInterrupt:
            print("\nStopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
