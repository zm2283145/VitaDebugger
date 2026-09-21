# Future companion hardware qualification

**Current status:** host-qualified and Vita compile/link-qualified foundation
only. This runbook does not authorize contacting a Vita, installing an
artifact, opening a listener on hardware, or publishing a build.

The candidate is one normal source-owned user-mode title linked with
`libvitadebug_companion.a`. It has no resident SUPRX/SKPRX, kernel API, module
loader, injection path, system hook, or protected-target path. VitaCompanion
must remain installed and unmodified as a separately invoked fallback
throughout qualification.

## Fixed side-by-side identities

| Surface | Candidate value |
| --- | --- |
| Screen receiver | `vdscreen-v1`, TCP 18197 |
| Control service | `vitadebug-companion-v1`, TCP 18198 |
| Application title | `VDSCRN001` |
| Application module | `vitadebug_companion_gate` |
| Static archive | `libvitadebug_companion.a` |
| Candidate artifact | `vitadebug-companion-gate.vpk` |
| Resident modules | none |

Both ports may be changed only within 18000-18999 after validation, must
remain distinct, and must not collide with 18194-18196 or each other. The
screen receiver additionally reserves the default control port 18198.
VitaCompanion 1337/1338 and agent-bridge 1348 are forbidden. Do not use an
address assigned to another hardware effort. A bind conflict is terminal;
do not probe for or silently select another port.

## Preconditions

1. Obtain explicit device-owner authorization and confirm no other effort owns
   the Vita, network interface, or selected ports.
2. Record device/firmware, repository commit and tree, VitaSDK revision,
   application build, host private address, ports, capability mask, and how
   the one-session secret/session ID were generated and destroyed.
3. Confirm the manifest in
   `screen/config/hardware-gate-side-by-side.json`, verify VitaCompanion files
   and configuration are untouched, and verify its ordinary fallback path is
   still available before installing the disposable source-owned title.
4. Start loopback-only. Moving control or screen traffic to a private LAN
   requires the explicit LAN consent value and one specific private interface.
   Never bind a wildcard/public interface or use port forwarding.

## Serialized checks

1. **Default-off and bind:** launch without consent and verify neither surface
   starts. Enable the disposable title, occupy 18198, and verify control
   initialization fails terminally without trying another port. Repeat for
   the screen receiver.
2. **Pair and identity:** use a fresh nonzero secret and session ID. Negotiate
   only status first. Verify the response reports exactly `VDSCRN001`, the
   current PID, generation, and session. Restart the title and confirm the old
   generation/session/sequence cannot control the new instance.
3. **Malformed clients:** send truncated headers and payloads, bad magic,
   version, sizes, reserved fields and tags, zero/stale identities, duplicate,
   skipped and regressed sequences, expired/excessive deadlines, unknown
   enums/capabilities, oversized paths/counts/reads, and slow partial records.
   Each must fail closed with bounded memory and deadline, then release the
   connection and any active input.
4. **Cooperative input:** register only a disposable application's callback.
   Test 50 ms and 1000 ms leases, renewal, disconnect, host process death,
   title exit, and service shutdown. In every case the stale-input watchdog
   must force neutral no later than the lease deadline. Confirm there is no
   system/PS/power input and no effect outside the current source-owned title.
5. **Input recording:** grant only the recording capability and consent, make
   the recording state visible, and submit physical controller/touch samples
   through the application API with frame/checkpoint markers. Verify exact
   values, monotonic relative time, event/duration bounds, overflow abort
   without silent drops, checksum, and explicit completion/abort reasons.
6. **Input playback:** in a fresh session, grant separate playback and mutation
   consent, import only a complete exact-identity trace, and explicitly start
   1x playback. Verify bounded tick work and drift reporting. Exercise cancel,
   timeout, callback failure, Wi-Fi loss, disconnect, shutdown, and
   title/generation change; every path must force neutral and none may
   auto-play after reconnect. Pause remains unsupported.
7. **Read-only debug root:** use a disposable application-owned root containing
   regular files, directories, a symlink candidate, a device-like candidate,
   traversal names, and a path swapped between resolution and open. Verify
   canonical list/read succeeds only for regular in-root objects, respects 32
   entries, 2048 bytes/request, and 1 MiB/session, and never writes, deletes,
   uploads, or accesses another mount.
8. **Screen integration:** use the same initialized companion instance and its
   registered application-owned color-bar buffers. Validate 18197 framing,
   identity, CRC, sequence, deadlines, disconnect cleanup, and the existing
   conservative rate gate. Do not query a system framebuffer or another PID.
9. **Wi-Fi and suspend lifecycle:** disconnect Wi-Fi during header, payload,
   input lease, and file read; suspend and resume once idle and once paired.
   The transport must close, input must neutralize, stale sessions must not
   resume, and a fresh explicit initialization must be required.
10. **Fallback:** after every destructive lifecycle test, exit the candidate
   title and verify VitaCompanion remains installed, unmodified, and available
   through its pre-existing workflow. Do not use fallback success to mask a
   companion failure.

Stop on the first unexplained result. Preserve logs without secrets, remove
the disposable candidate only after fallback verification, and update this
document with evidence before claiming hardware support.
