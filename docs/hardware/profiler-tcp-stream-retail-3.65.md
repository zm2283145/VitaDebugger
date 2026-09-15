# VitaProfiler live TCP stream on retail 3.65

Date: 2026-09-15

The ordinary user-mode TCP gate (`VDPT00001`) was built for the local PC
endpoint `10.1.1.146:18195`, installed through the signed VitaDevDeploy FTP
path, and run on retail 3.65. The test did not change or call the VitaDebugger
kernel plugin.

## Result

The live TCP path **passed end to end** after inbound TCP port 18195 was
allowed by the PC firewall. The receiver accepted the connection only from
the expected Vita address and independently validated a clean 828-byte stream:

- one sealed name dictionary with 11 names;
- exactly 13 events and zero unresolved referenced IDs;
- a paired `tcp_gate.zone` begin/end event;
- `tcp_gate.counter = 314`;
- two frame markers;
- three memory samples, one process-time sample, and four thread samples;
- zero producer drops or sink loss; and
- clean EOF and app-side network teardown.

The first successful capture measured a 16,826 us frame interval (59.43 FPS).
Its evidence is retained as the [raw capture](profiler-tcp-gate-pass-2026-09-15.vptrace)
and [decoded JSON](profiler-tcp-gate-pass-2026-09-15.json). The same capture is
also available as a [Perfetto/Chrome Trace export](profiler-tcp-gate-pass-2026-09-15.perfetto.json)
for timeline inspection.

A read-only VitaDevDeploy recovery-status check after the installations reported
`clean`, with no journal, partial journal, or promotion marker and
`safe_to_retry = true`.

## Lifecycle and failure recovery

The following bounded lifecycle gates also passed:

1. Circle before X cancelled without opening a profiler socket, released the
   app-owned network state, and allowed the title to launch again.
2. A source-checked PC listener accepted the Vita and deliberately reset the
   TCP connection. Vita Companion remained reachable after the reset and the
   gate could be closed normally.
3. The title then relaunched and completed a second valid capture, proving the
   failed transport did not poison the next profiler session. That recovery
   run measured a 16,729 us frame interval (59.78 FPS); see its
   [raw capture](profiler-tcp-gate-recovery-pass-2026-09-15.vptrace) and
   [decoded JSON](profiler-tcp-gate-recovery-pass-2026-09-15.json).

Before the firewall rule was enabled, the Vita reported `VP_ERROR_IO` during
TCP connect and the PC receiver timed out without accepting a sender. No trace
bytes were recorded in that failed attempt.

## Artifact identity

- tested VPK: 85,769 bytes, SHA-256
  `1050ABEBD0D6AC4278A7C131B2B20868B9236D3CB4992EFACB2E1EACD6DCA015`
- matching unstripped ELF: 144,304 bytes, SHA-256
  `3EB47E288F8E0CE01371E2C891A2FE95C58C5DC18A76E3BF92359E5D25222B99`
- first raw capture: SHA-256
  `1ED0BBA6A38BBDA36C8CC7395CF18EE4C16985BFD746E3CF38ADA51828C88734`
- first decoded JSON: SHA-256
  `8F5E2BF63F5956947F779CA403EA90D875F1054E207E7292197CAFFC15FDB34C`
- first Perfetto/Chrome Trace export: SHA-256
  `35310192A081676A3EC15869AF3C777A3A1082088B309866E8565207AE246DA0`
- recovery raw capture: SHA-256
  `8CC3E62205F66471CD66AAF580ECC7CF548531A207D2A0923D136B4715BDE36D`
- recovery decoded JSON: SHA-256
  `D9EFF7E5348BD996A5331A227DDD4CAE32544CA6D25B779166FCBE55ECA1E3CE`

This gate proves the current caller-owned SceNet TCP sink, binary stream
writer, dictionary transport, PC receiver, decoder, teardown, and post-failure
relaunch path. Sustained backpressure/partial-send stress and authenticated or
encrypted telemetry remain separate future work.
