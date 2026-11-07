# Third-party components (draft)

Our own code is source-available under the PolyForm Noncommercial License
1.0.0 (see `LICENSE`). The components below are **not** our code: they keep
their own licenses. Binaries are not stored in git; they ship as release
attachments with their license texts (versions and SHA-256 in
`third_party/tools.lock`). This list is a draft and will be finalised at
export time. Nothing here is legal advice.

## Own artwork (not third-party)

The launcher background image (`poc-native/launcher/res/bg_nutzer.png`,
built from `bg_nutzer.webp` via `res/convert_bg.py`) is the project
owner's own work and is licensed like the project's own assets. It is
listed here only for attribution; it is not a third-party component.

## Shipped with the release

| Component | Version | License | Purpose |
|---|---|---|---|
| Real-ESRGAN-ncnn-vulkan (`realesrgan-ncnn-vulkan.exe`) | 20220424 (release v0.2.5.0), https://github.com/xinntao/Real-ESRGAN | BSD-3-Clause | AI upscaler used by the Pack Creator on the user's own GPU |
| ncnn (runtime inside the exe above) | see Real-ESRGAN release, https://github.com/Tencent/ncnn | BSD-3-Clause | inference runtime |
| realsr-ncnn-vulkan (upstream of the exe) | https://github.com/nihui/realsr-ncnn-vulkan | MIT | upscaler frontend |
| texconv (`texconv.exe`) | may2026, https://github.com/microsoft/DirectXTex | MIT | DDS conversion (BC1/BC7 + mipmaps) in the Pack Creator |

## Planned for the release (Pack Creator, phase 2 — not bundled yet)

| Component | License | Purpose |
|---|---|---|
| Embedded Python runtime (without network modules) | PSF-2.0, https://www.python.org/downloads/windows/ | runs the Pack Creator pipeline locally |
| Pillow | HPND, https://pypi.org/project/pillow/ | image handling in the pipeline |
| Replacement fonts (e.g. Montserrat, Roboto, Arimo, Carlito and others) | OFL-1.1 / Apache-2.0, https://github.com/google/fonts | redistributable fonts for text reconstruction; the Windows fonts used during development are NOT redistributable |

## Referenced during development (not shipped)

| Component | License | Use |
|---|---|---|
| nv2a_vsh_cpu | Public Domain | knowledge reference for the vertex-shader bit layout (hardware facts); our interpreter is our own implementation |
| XbSymbolDatabase | MIT | recognition results only (function names/addresses); no code taken, its files are not in the export |
| Cxbx-Reloaded | GPL-2.0 | read-only reference during development; no copied code, tables or patterns in our code (see the license audit in the private repo). The kernel ordinal table (`ordinal_table.inc`) was re-derived clean-room from our own analysis of the user's disc and non-GPL sources |
| IMA-ADPCM step table, FNV-1a hash, SHA-256 | public standard / public-domain algorithm / own implementation per FIPS 180-4 | small standard algorithms implemented in our own code |
| MSVC runtime | user's Visual Studio license | NOT shipped |

## Open points (must be resolved before release)

1. **Model weights license:** the Real-ESRGAN model files (`*.bin`,
   `*.param`) in the ncnn-vulkan release carry no recognisable license of
   their own; it is **open** whether they fall under the repo's BSD-3.
   They will only be bundled once this is checked.
2. **Training-data terms:** the training sets behind the weights (DIV2K,
   Flickr2K, OST) have their own terms; their effect on the weights is open.
3. **XbSymbolDatabase attribution:** whether the MIT attribution duty
   applies to recognition results (names/addresses) is open.
4. **Fonts:** no replacement font is bundled yet; an OCR-A replacement and
   custom pixel fonts are still to be drawn as our own OFL fonts.
5. The license texts above will be placed next to the binaries as `.txt`
   files (`third_party/licenses/`) once the tools are bundled.
