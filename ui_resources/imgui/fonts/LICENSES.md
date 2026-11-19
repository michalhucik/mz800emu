# Písma a ikony overlay menu - evidence licencí

Soubory, které používá overlay menu (`src/ui-imgui/overlay/`). Načítá je
`overlay_style_load_fonts()`; chybějící soubor se přeskočí a overlay použije
základní písmo UI.

| Soubor | Co to je, verze | Licence | Text licence | Zdroj | SHA-256 |
|---|---|---|---|---|---|
| `Inter-Regular.ttf` | Inter 4.1, statický řez Regular | SIL OFL 1.1 | `Inter-LICENSE.txt` | https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip (`extras/ttf/`) | `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82` |
| `Inter-SemiBold.ttf` | Inter 4.1, statický řez SemiBold | SIL OFL 1.1 | `Inter-LICENSE.txt` | tamtéž | `78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3` |
| `JetBrainsMono-Regular.ttf` | JetBrains Mono Regular, větev master repa (číslo verze souboru neověřeno) | SIL OFL 1.1 | `JetBrainsMono-OFL.txt` | https://raw.githubusercontent.com/JetBrains/JetBrainsMono/master/fonts/ttf/JetBrainsMono-Regular.ttf | `e6fd0d7e91550b3ed2b735d4312474362c4716edc4fc0577a0f61ed782d5aed1` |
| `lucide.ttf` | ikony Lucide, balíček lucide-static 1.42.0 | ISC (část ikon z Feather: MIT) | `lucide-LICENSE.txt` | https://unpkg.com/lucide-static@1.42.0/font/lucide.ttf | `17dc5fd2ef603b487f8a307ca03b6c944579b214189dd21a34e98228fc8077cd` |

Hlavička s kódovými body ikon je ve zdrojích:

| Soubor | Co to je | Licence | Text licence | Zdroj | SHA-256 |
|---|---|---|---|---|---|
| `src/ui-imgui/overlay/res/IconsLucide.h` | IconFontCppHeaders, větev main (generováno pro "latest" Lucide) | zlib | `src/ui-imgui/overlay/res/IconFontCppHeaders-licence.txt` | https://raw.githubusercontent.com/juliettef/IconFontCppHeaders/main/IconsLucide.h | `ad2e0fcef96dddadd05abc51ead1ad2966092bba79af3d12dd2e969ea2a59f8d` |

Staženo 2026-11-18.

## Poznámky pro údržbu

- Hlavička je generovaná pro nejnovější Lucide, písmo je verze 1.42.0. Kódový
  bod každé nově použité ikony ověř proti `lucide.css` téže verze písma
  (https://unpkg.com/lucide-static@1.42.0/font/lucide.css); ikona, která v
  písmu není, se nevykreslí. Ověřené ikony: save e14d, cpu e0a9, monitor e11d,
  volume-2 e1ab, keyboard e284, camera e064, pause e12e, play e13c,
  rotate-cw e149, folder-open e247, download e0b2, upload e19e,
  cassette-tape e4ca.
- Lucide nemá ikonu diskety: disketa je `ICON_LC_SAVE`. Ikonu vysunutí média
  písmo má (U+E733), hlavička `IconsLucide.h` ji ale nezná (končí na U+E6FD);
  vysunutí proto kreslí `ovl_draw_icon()` přes `ImDrawList` (`OVL_ICON_EJECT`).
- Kódové body Lucide leží v soukromé oblasti Unicode a překrývají se s
  `ui_resources/imgui/symbols/mzglyphs.ttf` (U+E100 až U+E4FF). Lucide se proto
  slučuje jen do písem overlaye, nikdy do základního písma UI.
- Inter má v soukromé oblasti vlastní glyfy na týchž kódových bodech; při
  načítání se z něj soukromá oblast vylučuje (`GlyphExcludeRanges`), jinak by
  ikony zakryl.
- Ostatní písma v tomto adresáři (DroidSans, Cousine, Karla, NotoSansJP,
  Proggy, Roboto) tato evidence nepokrývá; z nich má vlastní text licence jen
  Cousine (`Cousine-OFL.txt`).
