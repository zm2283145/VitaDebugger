# VitaDevDeploy artwork contract

The build packages artwork from this directory only when
`VDEV_ENABLE_LIVEAREA_ASSETS=ON` (the default). Keep the source layout exactly
as follows so its paths match the final VPK:

```text
assets/
  source/
    icon-master.png
    livearea-background-master.png
    startup-master.png
  sce_sys/
    icon0.png                         128 x 128
    pic0.png                          960 x 544
    livearea/contents/
      bg.png                          840 x 500
      startup.png                     280 x 158
      template.xml
```

The three PNGs under `source/` are the unmodified editable ImageGen masters.
The production `sce_sys` PNGs are mechanically cropped, resized, and
palette-quantized from those masters: the icon master produces `icon0.png`, the
background master produces `pic0.png` and `livearea/contents/bg.png`, and the
startup master produces `livearea/contents/startup.png`. The build's explicit
file map packages only the five files under `sce_sys` (the four production PNGs
and `template.xml`); it never includes `source/` in a VPK.

Regenerate the four production files deterministically on Windows with:

```powershell
deploy/tools/prepare_livearea_assets.ps1
```

The helper uses the in-box .NET image APIs, a median-cut palette refined by
four deterministic clustering passes, and serpentine error diffusion. It does
not require Python or an external image converter. Each run starts from the
three masters rather than re-quantizing an indexed output. The icon's rounded
transparent corners are flattened onto the UI's opaque midnight background
(`#071426`) so the Vita bubble never receives an alpha palette.

Every production image must be a non-interlaced, 8-bit indexed-color PNG
(PNG IHDR color type 3) with standard compression and filtering. Palette
transparency is permitted where the artwork needs it, such as `startup.png`;
`icon0.png` should remain opaque. Do not rename or resize these files. CMake
reads each PNG's signature and IHDR fields and stops the build if an input does
not match this contract.

`template.xml` uses Vita's minimal `a1` LiveArea layout. `bg.png` fills the
LiveArea panel, `startup.png` is the gate/start button artwork, `icon0.png` is
the home-screen bubble, and `pic0.png` is the full-screen application artwork.

Do not replace the reviewed raster files with empty placeholders: packaging a
zero-byte or mislabeled image would create a broken VPK. For a temporary build
with no artwork, configure CMake with
`-DVDEV_ENABLE_LIVEAREA_ASSETS=OFF` or pass `-DisableLiveAreaAssets` to
`tools/build_agent.ps1`.

The publishing helper verifies every enabled artwork entry inside the finished
VPK byte-for-byte against these sources. It records each source SHA-256 in
`BUILD-INFO.txt`; the existing VPK SHA-256 therefore covers the complete image
set as well.
