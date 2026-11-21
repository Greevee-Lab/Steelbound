# Third-party components

Our own code is source-available under the PolyForm Noncommercial License
1.0.0 (see `LICENSE`). The components below are **not** our code: they keep
their own licenses. Nothing here is legal advice.

## Own artwork (not third-party)

The launcher background image (`poc-native/launcher/res/bg_nutzer.png`,
built from `bg_nutzer.webp` via `res/convert_bg.py`) is the project
owner's own work and is licensed like the project's own assets. It is
listed here only for attribution; it is not a third-party component.

## Shipped with the release

No third-party binaries are shipped with the community release.

### Compiled into the client

| Component | License | Use |
|---|---|---|
| BearSSL 0.6 (subset) | MIT | X25519 key exchange and AES-256-GCM for the encrypted online traffic of Line of Contact. Unmodified source files in `poc-native/harness/third_party/bearssl/` (constant-time implementations `aes_ct`, `ghash_ctmul`, `c25519_m31`), compiled into `sb_emu.dll`. Source: https://bearssl.org/ |

BearSSL license:

```
Copyright (c) 2016 Thomas Pornin <pornin@bolet.org>

Permission is hereby granted, free of charge, to any person obtaining 
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be 
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Referenced during development (not shipped)

| Component | License | Use |
|---|---|---|
| nv2a_vsh_cpu | Public Domain | knowledge reference for the vertex-shader bit layout (hardware facts); our interpreter is our own implementation |
| XbSymbolDatabase | MIT | recognition results only (function names/addresses); no code taken, its files are not in the export |
| Cxbx-Reloaded | GPL-2.0 | read-only reference during development; no copied code, tables or patterns in our code (see the license audit in the private repo). The kernel ordinal table (`ordinal_table.inc`) was re-derived clean-room from our own analysis of the user's disc and non-GPL sources |
| IMA-ADPCM step table, FNV-1a hash, SHA-256 | public standard / public-domain algorithm / own implementation per FIPS 180-4 | small standard algorithms implemented in our own code |
| MSVC runtime | user's Visual Studio license | NOT shipped |

## Acknowledgements

Thanks to the authors of XbSymbolDatabase (MIT License) and nv2a_vsh_cpu
(Public Domain), and to the Cxbx-Reloaded and Xbox preservation community,
whose public research helped us understand the original hardware. None of
their code is included in Steelbound.
