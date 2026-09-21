# Screen stream protocol version 1

All integers are unsigned and network byte order. The sender first writes one
72-byte authentication/session preface:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | ASCII `VDSA` |
| 4 | 2 | version, exactly `1` |
| 6 | 2 | preface size, exactly `72` |
| 8 | 32 | nonzero pre-shared session token |
| 40 | 9 | exact uppercase application title ID |
| 49 | 3 | reserved, all zero |
| 52 | 4 | nonzero process ID |
| 56 | 8 | nonzero application-supplied process generation |
| 64 | 8 | nonzero random session ID |

The receiver compares the token in constant time before reading any frame.
The token is admission control, not encryption: screen pixels and metadata are
visible to an observer on the network. Use a fresh token for each supervised
session and only a trusted private LAN. Do not expose the port to the internet.
This token grants only screen submission; it must never authorize file, launch,
inventory, input, debugger, deployment, or MCP operations.

Each frame is a 64-byte header followed by exactly `payload_length` bytes:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | ASCII `VDSC` |
| 4 | 2 | version, exactly `1` |
| 6 | 2 | header size, exactly `64` |
| 8 | 8 | monotonically increasing frame sequence |
| 16 | 8 | application-supplied monotonic timestamp in microseconds |
| 24 | 2 | visible width |
| 26 | 2 | visible height |
| 28 | 4 | byte stride |
| 32 | 4 | pixel format |
| 36 | 4 | payload length, exactly `stride * height` |
| 40 | 4 | IEEE CRC-32 of the complete payload |
| 44 | 4 | flags, exactly `SOURCE_OWNED` (`1`) |
| 48 | 8 | session ID, exactly matching the authenticated preface |
| 56 | 8 | reserved, all zero |

Pixel formats describe bytes in memory, not GPU register notation:

| Value | Format |
| ---: | --- |
| 1 | `RGBA8888` |
| 2 | `BGRA8888` |
| 3 | little-endian `RGB565` |

There is no compression, delta frame, arbitrary address, target identifier,
display query, or control record. A new version is required to add fields or
flags. The dependency-free PPM output discards alpha and is larger than PNG,
but avoids adding an image codec or native parser attack surface.

## Receiver rules

The reference receiver validates the fixed header before allocating a payload.
Defaults allow at most 960x544, 4 MiB per frame, 10 frames/second with a
two-frame burst, three seconds idle, and one hour per connection. A bad token,
unknown field, zero or mismatched identity, oversized or inconsistent payload,
checksum mismatch, sequence or timestamp regression, rate violation, timeout,
or truncated record closes the connection. A duplicate complete sequence is
counted and dropped. Gaps are accepted and counted because producer-side
drop-old rate limiting is expected.

Only a checksum-valid complete frame is published. Two fixed PPM slots bound
disk usage; `latest.json` is atomically replaced to select a slot. Consumers
must verify `image_sha256` and reread `latest.json`, as implemented by
`vdscreen.receiver.read_latest`, to avoid racing a later slot rotation.

The host listener defaults to TCP 18197. It accepts overrides only from 18000
through 18999 and reserves 18194-18196 for existing VitaDebugger services.
VitaCompanion's 1337/1338 and vita-agent-bridge's 1348 are outside the accepted
range. A bind conflict returns an error and closes the failed listener; it
never falls back to a different port.
