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

## VitaSDK debug screen

Production agent artifacts are headless and do not contain the VitaSDK debug
screen. Only a build explicitly requested with
`-EnableExperimentalDisplayUi` (CMake `VDEV_ENABLE_DISPLAY_UI=ON`) compiles
`debugScreen.c` from the installed VitaSDK `samples/common` directory. That
source includes `debugScreenFont.c` and uses the accompanying headers from the
same directory. The entry source and header are build-time inputs rather than
vendored files; CMake accepts only the currently reviewed `debugScreen.c` and
`debugScreen.h` SHA-256 values and stops if either differs. The included font
source still comes from the installed VitaSDK directory and retains its own
notice below.

- Upstream: <https://github.com/vitasdk/samples/tree/master/common>
- VitaSDK samples code license: CC0 1.0 Universal
- Embedded debug-font origin: PSPSDK `font.c`
- Font copyright: Marcus R. Brown, James Forshaw, and John Kelley
- Font license: BSD 3-Clause, using the PSPSDK license text reproduced below

The VitaSDK samples repository declares its code and build scripts to be
CC0-1.0. `debugScreenFont.c` separately carries the PSPSDK BSD notice, so that
notice is retained here for binary redistribution:

> Copyright (c) 2005 adresd; Copyright (c) 2005 Marcus R. Brown;
> Copyright (c) 2005 James Forshaw; Copyright (c) 2005 John Kelley;
> Copyright (c) 2005 Jesper Svennevid. All rights reserved.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> 1. Redistributions of source code must retain the above copyright notice,
>    this list of conditions and the following disclaimer.
> 2. Redistributions in binary form must reproduce the above copyright notice,
>    this list of conditions and the following disclaimer in the documentation
>    and/or other materials provided with the distribution.
> 3. The names of the authors may not be used to endorse or promote products
>    derived from this software without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE AUTHORS “AS IS” AND ANY EXPRESS OR IMPLIED
> WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
> MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
> EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
> SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
> PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
> OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
> WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
> OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
> ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## Monocypher

The Vita agent vendors the required Monocypher 4.0.3 source files for Ed25519
verification. Monocypher is dual-licensed under BSD-2-Clause or CC0-1.0; this
project uses the BSD-2-Clause option and retains the upstream notices in the
vendored files.

- Upstream: <https://github.com/LoupVaillant/Monocypher>
- Release: 4.0.3, commit `ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f`
- Copyright: Loup Vaillant, Michael Savage, Fabio Scotoni, and contributors
- License: BSD-2-Clause OR CC0-1.0
