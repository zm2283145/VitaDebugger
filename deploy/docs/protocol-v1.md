# VitaDevDeploy protocol v1

VitaDevDeploy is a one-shot, signed deployment bridge. Vita Companion supplies
the default file-transfer path and application lifecycle control; an opt-in
direct TCP carrier can transfer package bytes to the agent itself. Both paths
commit the same canonical job, and the VitaDevDeploy app is the only component
allowed to promote it into an installed Vita application.

The on-device root is fixed at `ux0:data/VitaDevDeploy`. Client input never
selects an arbitrary device path.

## Transaction outline

1. The PC validates and extracts a VPK into a private local staging directory.
2. The PC adds `sce_sys/package/head.bin`, builds a canonical file manifest,
   and signs the request and manifest with the developer's private key.
3. The PC launches title `VDEVDEP01` through Vita Companion.
4. VitaDevDeploy creates a fresh challenge and waits for one committed job.
5. The PC reads the challenge and delivers the job through FTP or direct TCP.
   Both carriers make `request.v1` visible last as the only commit point.
6. VitaDevDeploy verifies the signature, challenge, metadata, package tree,
   hashes, and requested title before verification or promotion.
7. VitaDevDeploy writes an atomic verification or installation result and
   exits. For `install_launch`, the host launches the target only after reading
   that durable success result.

One challenge authorizes at most one job. Relaunching the deployer creates a
new challenge and invalidates the previous one.

The canonical files and verification rules below are transport-independent.
The direct carrier authenticates request and manifest metadata before accepting
package bytes; its framing, acknowledgements, interruption behavior, and
remaining hardware gates are specified in [direct-tcp-v1.md](direct-tcp-v1.md).

## Device layout

```text
ux0:data/VitaDevDeploy/
  challenge.v1
  inbox/<job-id>/
    package/
    manifest.v1
    signature.bin
    request.v1
  results/<job-id>.result
  state/journal.v1
```

`<job-id>` is exactly 32 lowercase hexadecimal characters. All other paths
are derived from it by the deployer.

## Challenge

`challenge.v1` uses LF endings and ends with LF:

```text
VITADEVDEPLOY-CHALLENGE-1
nonce=<64-lowercase-hex-characters>
```

The nonce is 32 bytes from `sceKernelGetRandomNumber`. The file is written to a
temporary sibling and renamed into place.

## Manifest

`manifest.v1` begins with:

```text
VITADEVDEPLOY-MANIFEST-1
```

Each following line is:

```text
<sha256-lowercase-hex><TAB><size-in-canonical-decimal><TAB><relative-path><LF>
```

Rows are sorted by the UTF-8 bytes of the path. A path must be printable ASCII,
use `/` separators, and be relative. Empty, `.` and `..` components, control
characters, tabs, backslashes, colons, duplicate paths, and ASCII case-folding
collisions are rejected. The tree must contain exactly the listed regular
files, including `eboot.bin`, `sce_sys/param.sfo`, and
`sce_sys/package/head.bin`; directories, symbolic links, and special files do
not count as manifest entries.

## Request

`request.v1` has exactly these lines, in this order, with LF endings and a final
LF:

```text
VITADEVDEPLOY-REQUEST-1
job=<32-lowercase-hex-characters>
nonce=<64-lowercase-hex-characters>
action=<verify|install|install_launch>
title_id=<9-uppercase-ASCII-alphanumeric-characters>
manifest_sha256=<64-lowercase-hex-characters>
file_count=<canonical-decimal>
total_size=<canonical-decimal>
```

`VDEVDEP01`, the deployer's own title, is never a valid target. Bootstrap builds
add their host bubble title to the reject list and may be restricted to
installing only `VDEVDEP01`.

## Signature

`signature.bin` is a raw 64-byte Ed25519 signature over this exact byte string:

```text
VITADEVDEPLOY-SIGNED-JOB-1\0 || request.v1 || manifest.v1
```

The canonical non-identity, prime-subgroup 32-byte public key is validated at
configure time and compiled into the deployer. Its exact raw-byte SHA-256
fingerprint is recorded in published build metadata. The private key remains
on the developer PC and is never uploaded or committed.

## Result

`results/<job-id>.result` is written as `.part` and atomically renamed. It uses
this exact order and ends with LF:

```text
VITADEVDEPLOY-RESULT-1
job=<job-id>
state=<success|failed>
stage=<lowercase-ASCII-token>
code=<canonical-signed-decimal>
title_id=<title-id>
message=<percent-encoded-UTF-8>
```

`code` preserves the exact signed Vita error code. A successful verification or
promotion reports `0`. The `stage` field distinguishes failures such as
`request`, `signature`, `tree`, `sfo`, and `promote`. A successful
`install_launch` result confirms installation, not the later host-side launch;
the host reports that lifecycle outcome separately.

## Limits

The host and Vita enforce matching conservative limits. Version 1 is intended
for developer-built homebrew packages, not arbitrary PSN content:

- one transaction at a time;
- 8,192 files maximum;
- 1 GiB expanded data maximum;
- 256 MiB maximum for one file;
- 240 bytes maximum per relative path;
- no encrypted ZIP entries, links, devices, or other special entries;
- no target-title deletion before promotion;
- no deployer self-update.

The initial hardware milestone compiled promotion off and accepted only
`action=verify`. Verification-only remains the required first gate for a new
agent/key pairing. The current install-enabled path has subsequently completed
signed install-and-launch on retail 3.65 through both staged FTP and direct TCP.
Direct TCP remains experimental while its hardware interruption/security matrix
is incomplete; passing the basic install gate does not authorize automatic
retry after an ambiguous outcome.
