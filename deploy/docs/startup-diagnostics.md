# Startup diagnostics

The agent writes two best-effort, payload-free diagnostic files before a job
exists. They are observability records only: failures to write them are ignored
and they do not replace, relax, or change any deployment durability check.

`ux0:data/VitaDevDeploy.startup` retains the original 16-byte little-endian
`VDD1` version 1 record: `magic:u32`, `version:u32`, `stage:u32`, `code:i32`.
Stage 7 is the complete atomic challenge write and its code is the value
returned to the startup state machine.

`ux0:data/VitaDevDeploy.startup_io` is reset when that challenge write begins.
It starts with the 16-byte little-endian header
`magic=VIO1:u32`, `version=1:u32`, `header_size=16:u32`,
`event_size=16:u32`. Each following record is
`sequence:u32`, `step:u32`, `event:u32`, `code:i32`. A torn final record may be
ignored; earlier complete records remain useful. Event 2 means the operation
was entered and event 3 contains its exact return code. Success is normalized
to zero for descriptor-returning open operations. The step IDs are stable:

| ID | Operation |
| ---: | --- |
| 1 | open challenge `.part` file |
| 2 | write the complete challenge payload |
| 3 | `SyncByFd` on the challenge file |
| 4 | close the challenge file |
| 5 | rename `.part` to `challenge.v1` |
| 6 | post-rename stat and regular-file/size validation |
| 7 | open the parent directory with `Dopen` |
| 8 | `SyncByFd` on the parent directory |
| 9 | close the parent directory with `Dclose` |
| 10 | checked device-wide `sceIoSync` after directory sync is rejected |

Both enter and result events are recorded because cleanup still runs after
some failures. For example, a successful step 9 result must not conceal a
negative step 8 result. The trace deliberately uses only raw open/write/close
operations on its own file—never the atomic writer or a sync helper—so it
cannot recurse into the path under observation. It contains no challenge
nonce, request, key, path, or package bytes.

After pulling both files, decode them on the PC with:

```powershell
py -3 tools/decode_startup_trace.py VitaDevDeploy.startup `
  --io-trace VitaDevDeploy.startup_io
```

The decoder prints signed decimal and exact unsigned 32-bit hexadecimal forms
of each Vita return code. Pull both files from the same launch; the records do
not contain a shared session identifier.
