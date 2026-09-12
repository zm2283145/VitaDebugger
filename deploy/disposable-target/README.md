# Disposable deployment target

This directory builds the deliberately small `VDDT00001` application used to
validate VitaDevDeploy installation and post-install launch without replacing
another development app.

The target is an ordinary safe user-mode fSELF. It imports only Vita file I/O
in addition to the normal C runtime. It has no network, display, controller,
AppMgr, promoter, kernel-plugin, or unsafe-authority code.

Each successful launch increments, file-syncs, and fully re-reads:

```text
ux0:data/VitaDevDeploy/disposable-target.last-run
```

The marker contains `launch_count`, `title_id`, `status=marker_committed`, and
`marker_version`. The app returns normally immediately after syncing the
marker; it owns no framebuffer and starts no background threads. Compare
`launch_count` before and after a launch when proving that a new process ran.
If firmware rejects directory-descriptor sync with `EACCES`, the safe app uses
a writable file resync plus a separate read-only verification pass. That proves
the complete file is visible, not that the rename namespace survives sudden
power loss.

Build and audit the package from the repository root:

```powershell
.\tools\build_disposable_target.ps1
py -3 -m host.vitadevdeploy verify .\dist\disposable-test\VitaDevDeployDisposableTest.vpk
```

The build helper publishes `eboot.bin`, the VPK, `SHA256SUMS.txt`, and
`BUILD-INFO.txt` under `dist/disposable-test/`. It rejects a non-safe auth ID,
an unexpected VPK identity or file list, or a VPK whose embedded eboot differs
from the separately published eboot.

Before the first hardware install, independently confirm that `VDDT00001` is
not already present on that Vita. The title ID is intentionally dedicated to
this test, but no PC-side build can inventory a disconnected device.
