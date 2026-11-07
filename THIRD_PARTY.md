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

No third-party binaries are shipped with the community release.

## Referenced during development (not shipped)

| Component | License | Use |
|---|---|---|
| nv2a_vsh_cpu | Public Domain | knowledge reference for the vertex-shader bit layout (hardware facts); our interpreter is our own implementation |
| XbSymbolDatabase | MIT | recognition results only (function names/addresses); no code taken, its files are not in the export |
| Cxbx-Reloaded | GPL-2.0 | read-only reference during development; no copied code, tables or patterns in our code (see the license audit in the private repo). The kernel ordinal table (`ordinal_table.inc`) was re-derived clean-room from our own analysis of the user's disc and non-GPL sources |
| IMA-ADPCM step table, FNV-1a hash, SHA-256 | public standard / public-domain algorithm / own implementation per FIPS 180-4 | small standard algorithms implemented in our own code |
| MSVC runtime | user's Visual Studio license | NOT shipped |

## Open points (must be resolved before release)

1. **XbSymbolDatabase attribution:** whether the MIT attribution duty
   applies to recognition results (names/addresses) is open.
