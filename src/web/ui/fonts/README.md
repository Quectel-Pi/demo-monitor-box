# Web UI fonts

These WOFF2 files are bundled with the Monitor Box Web UI so browsers do not
depend on fonts installed by the client operating system.

| File | Family | Weight | Source |
| --- | --- | --- | --- |
| `noto-sans-sc-400.woff2` | Noto Sans SC | 400 | `@fontsource/noto-sans-sc@5.2.8`, `chinese-simplified` subset |
| `noto-sans-sc-600.woff2` | Noto Sans SC | 600 | same |
| `noto-sans-sc-700.woff2` | Noto Sans SC | 700 | same |
| `noto-sans-sc-102-*-normal.woff2` | Noto Sans SC symbol slice | 400/600/700 | same package, slice 102 (`U+2265`) |
| `noto-sans-sc-105-*-normal.woff2` | Noto Sans SC symbol slice | 400/600/700 | same package, slice 105 (`U+2190`) |
| `noto-sans-sc-109-*-normal.woff2` | Noto Sans SC symbol slice | 400/600/700 | same package, slice 109 (`U+2192`) |
| `noto-sans-mono-400.woff2` | Noto Sans Mono | 400 | `@fontsource/noto-sans-mono@5.2.8`, `latin` subset |
| `noto-sans-mono-700.woff2` | Noto Sans Mono | 700 | same |

Fontsource package: https://www.npmjs.com/package/@fontsource/noto-sans-sc

The fonts are licensed under the SIL Open Font License 1.1. See
`LICENSE.txt`. `tools/embed_web_ui.py` embeds the files into
`src/web/web_ui_generated.h`; the running server exposes them below `/fonts/`.
