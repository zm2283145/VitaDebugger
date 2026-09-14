# Third-party material

## VitaShell

VitaDevDeploy includes `third_party/vitashell/head.bin`, copied unchanged from
VitaShell at commit `81af70971ba18b8ce86215b04180f1e3d21cdfc9`.

- Upstream: <https://github.com/TheOfficialFloW/VitaShell>
- File SHA-256: `cbb88299319048e19115a1fc9c76b04e33745291749de0e08963b9b425623f45`
- Copyright: TheFloW and VitaShell contributors
- License: GNU General Public License version 3 or later

The host-side `fpkg_hmac` and `head.bin` transformation are based on
VitaShell's `package_installer.c`. The current on-device package-promotion
adapter instead follows the VitaDB-Downloader revision documented below. The
whole VitaDevDeploy project is distributed under GPL-3.0-only so these parts
can be redistributed together.

## VitaDB-Downloader

The internal-PAF argument block and asynchronous PromoterUtil sequence in
`agent/src/promoter_vitadb.c` are adapted from VitaDB-Downloader's
`source/promoter.cpp` and `source/main.cpp`. VitaDevDeploy adds complete return
checking, terminal-result validation, progress callbacks, power ticks,
known-versus-unknown outcome tracking, and reverse-order cleanup.

- Upstream: <https://github.com/Rinnegatamante/VitaDB-Downloader>
- Pinned commit: [`415033d90e08a6bc0a30e2ee6e9456db600d7b22`](https://github.com/Rinnegatamante/VitaDB-Downloader/commit/415033d90e08a6bc0a30e2ee6e9456db600d7b22)
- Copyright: Rinnegatamante and VitaDB-Downloader contributors
- License: GNU General Public License version 3

## libvita2d

Production agent artifacts remain headless and do not link a graphics library.
Only a build explicitly requested with `-EnableExperimentalDisplayUi` (CMake
`VDEV_ENABLE_DISPLAY_UI=ON`) links the libvita2d package installed by VitaSDK.
The UI uses its primitive renderer and the Vita's default PGF font; no libvita2d
source or font data is vendored in this repository. The currently tested
VitaSDK package is `libvita2d 0.0.0.r188.ga8f15ab-1`.

- Upstream: <https://github.com/xerpi/libvita2d>
- Tested upstream revision: `a8f15ab`
- Copyright: xerpi and libvita2d contributors
- License: MIT

The libvita2d license permits use, modification, and redistribution provided
that its copyright and permission notice are retained. Binary distributors of
the optional graphical build must include the complete MIT notice supplied by
the installed libvita2d package or upstream source revision.

## Monocypher

The Vita agent vendors the required Monocypher 4.0.3 source files for Ed25519
verification. Monocypher is dual-licensed under BSD-2-Clause or CC0-1.0; this
project uses the BSD-2-Clause option and retains the upstream notices in the
vendored files.

- Upstream: <https://github.com/LoupVaillant/Monocypher>
- Release: 4.0.3, commit `ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f`
- Copyright: Loup Vaillant, Michael Savage, Fabio Scotoni, and contributors
- License: BSD-2-Clause OR CC0-1.0
