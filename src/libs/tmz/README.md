# Knihovna tmz (src/libs/tmz/)

Knihovna pro čtení, zápis a přehrávání souborů TMZ (TapeMZ), kazetového
archivu pro počítače Sharp MZ. TMZ je rozšíření formátu TZX v1.20 o bloky
Sharp MZ v rozsahu 0x40-0x4F. Aktuální verze formátu je 1.1 (blok 0x46,
`tmz-v1.1-specification.md`); bloky verze 1.0 (`tmz-specification.md`)
se dál čtou a přehrají.

Soubor TMZ i TZX čte a zapisuje knihovna tzx (`tzx_load()`, `tzx_save()`);
knihovna tmz k ní přidává bloky Sharp MZ, registr profilů a player.

## Soubory

| Soubor | Účel |
|--------|------|
| tmz.h/.c | Hlavička TMZ (`tmz_header_init`, `tmz_header_is_tmz`), ID a názvy bloků, enumy (stroj, pulzní sada, formát, rychlost), verze |
| tmz_blocks.h/.c | Packed struktury bloků 0x40-0x45, parse a create, převod MZF <-> blok |
| tmz_tlv.h/.c | Kódování TLV pro blok 0x46 (iterátor, buffer, čísla tagů) |
| tmz_mzfile.h/.c | Blok 0x46 MZ File: čtení, validace a zápis; parser nemění data bloku |
| tmz_part_fm.h/.c | TLV části rodiny FM <-> parametry generátoru mzcmt_fm, kontrola rezervy čtecího bodu |
| tmz_part_manchester.h/.c | TLV části rodiny MANCHESTER <-> parametry generátoru mzcmt_manchester |
| tmz_profile.h/.c | Registr profilů zapisovače: MZF -> blok 0x46 |
| tmz_v1conv.h/.c | Převod bloků v1.0 (0x40, 0x41, 0x45) na 0x46 (spec v1.1 kap. 9.2); player v paměti, `tmzconv --upgrade` do souboru |
| tmz_mzt.h/.c | MZT a hlavička CMTSPEED UniCMT: čtení, import na bloky UNICMT, export klasický a UniCMT |
| tmz_player.h/.c | Player: blok -> CMT stream, stavový automat přehrávání souboru |

## Profily (tmz_profile)

ROM-800-NOMINAL, ROM-800-EXACT, TC-1.21 (TurboCopy), TAPEMZ-V1-LEGACY
(generátory v1.0 pro převedené bloky), UNICMT, IC-10.2-NORMAL,
IC-10.2-TURBO, IC-10.2-FASTIPL, IC-10.2-CPM (Intercopy V10.2), PEZIK-2.0A
(CP/M TAPE.COM), BASIC-1Z016 (datové soubory BASICu) a MZ800EMU-CMT
(signál, který emulátor mz800emu hraje z MZF přes mztape; ID v makru
`TMZ_PROFILE_ID_MZ800EMU_CMT`). Hodnoty a zdroje jsou ve specifikaci v1.1
(kap. 6-8).

## Architektura

```
TMZ/TZX soubor (generic_driver)
  |
  +-- tzx_load() --> st_TZX_FILE (hlavička + pole bloků st_TZX_BLOCK)
  |
  +-- tmz_player_play_next() / tmz_player_play_block() --> CMT stream
        |
        +-- 0x40, 0x41, 0x45 --> tmz_v1conv --> 0x46 (převoditelné případy)
        |                    --> mztape / mzcmt_* (ostatní, cesta v1.0)
        +-- 0x46 --> části FM (mzcmt_fm) a MANCHESTER (mzcmt_manchester)
        +-- TZX audio bloky (0x10-0x15, 0x18, 0x20) --> tzx --> cmt_vstream
        +-- řídicí a informační bloky --> bez zvuku (stavový automat)
```

## Bloky Sharp MZ

| ID | Blok | Parse | Create | Player | Poznámka |
|----|------|-------|--------|--------|----------|
| 0x40 | MZ Standard Data | ano | ano | ano | Převod na 0x46 (sada MZ-800 -> ROM-800-NOMINAL, jinak TAPEMZ-V1-LEGACY), s `v1_convert = 0` mztape |
| 0x41 | MZ Turbo Data | ano | ano | ano | NORMAL 1:1 -> TAPEMZ-V1-LEGACY, TURBO -> TC-1.21, FASTIPL 1:1/2:1/7:3/8:3 -> IC-10.2-FASTIPL, CPM_TAPE -> PEZIK-2.0A (vždy); NORMAL nad 1:1, ostatní FASTIPL, FSK, SLOW a DIRECT přes mzcmt_*; SINCLAIR se nepřehraje (ZX záznam patří do bloku TZX 0x10/0x11) |
| 0x42 | MZ Extra Body | ano | ne | ne | Player blok přeskočí s varováním |
| 0x43 | MZ Machine Info | ano | ano | info | Takt CPU pro časovou základnu částí 0x46 |
| 0x44 | MZ Loader | ano | ne | ne | Player blok přeskočí s varováním |
| 0x45 | MZ BASIC Data | ano | ano | ano | Vždy převod na 0x46 BASIC-1Z016 (1 až 254 kusů, víc se nepřehraje) |
| 0x46 | MZ File (TMZ v1.1) | ano | ano | ano | MZF + seznam částí s parametry v TLV; rodiny FM a MANCHESTER |
| 0x47-0x4F | rezervované | - | - | ne | Player blok přeskočí s varováním |

## Bloky TZX (knihovna tzx)

| ID | Blok | Player | Poznámka |
|----|------|--------|----------|
| 0x10-0x15 | Standard/Turbo Speed, Pure Tone, Pulse Sequence, Pure Data, Direct Recording | ano | generuje knihovna tzx |
| 0x18 | CSW Recording | ano | jen komprese RLE (Z-RLE = TMZ_PLAYER_ERROR_STREAM_CREATE); úrovně pulzů určuje blok (první LOW), po bloku opak posledního pulzu jako u 0x15 |
| 0x19 | Generalized Data | ne | |
| 0x20 | Pause | ano | Pause 0 zastaví pásku |
| 0x21-0x28 | Group, Jump, Loop, Call, Return, Select | řídí | stavový automat `tmz_player_play_next()`; bezstavové `tmz_player_play_block()` je jen přeskočí |
| 0x2A | Stop Tape if 48K | ano | zastavení pásky |
| 0x2B | Set Signal Level | ano | nastaví úroveň dalšího pulzu |
| 0x30-0x35, 0x5A | informační bloky | přeskočí | |

## Úroveň signálu a zastavení pásky

- `tmz_player_play_next()` drží úroveň signálu mezi bloky
  (`st_TMZ_PLAYER_STATE`): blok TZX začne aktuální úrovní (pravidla TZX
  1.20), 0x2B ji nastaví. Po bloku MZ, který končí tichem, platí nízká
  úroveň, jinak opak poslední hodnoty, aby se hrana na přechodu neztratila.
- Pause 0 a 0x2A zastaví pásku: `tmz_player_play_next()` vrátí NULL
  s `TMZ_PLAYER_OK` a `tmz_player_state_stopped()` vrátí true; další volání
  pokračuje za blokem zastavení.
- Pojistka proti nekonečné smyčce řídicích bloků: jedno volání
  `tmz_player_play_next()` nejvýš `TMZ_PLAYER_MAX_ITERATIONS` (1 000 000)
  zpracovaných bloků. Volající, který player volá opakovaně (přeskakuje
  bloky, které přehrát nejdou), použije `tmz_player_play_next_budget()`
  s jedním rozpočtem pro celou posloupnost volání; vyčerpání = `finished`
  a `TMZ_PLAYER_ERROR_STREAM_CREATE`.
- `pause_ms` bloků v1.0 je ticho na nízké úrovni za signálem.

## Chybové kódy

Čtení a zápis souboru vrací `en_TZX_ERROR` (tzx.h): `TZX_OK`,
`TZX_ERROR_NULL_INPUT`, `TZX_ERROR_INVALID_BLOCK`, `TZX_ERROR_UNSUPPORTED`,
`TZX_ERROR_STREAM_CREATE`, `TZX_ERROR_ALLOC`, `TZX_ERROR_IO`,
`TZX_ERROR_INVALID_SIGNATURE`, `TZX_ERROR_INVALID_VERSION`,
`TZX_ERROR_UNEXPECTED_EOF`, `TZX_ERROR_UNKNOWN_BLOCK`. `tzx_load()` je
striktní: při chybě vrátí NULL, částečně načtený soubor nevrací.

Player vrací `en_TMZ_PLAYER_ERROR` (tmz_player.h):

| Kód | Kdy nastane |
|-----|-------------|
| TMZ_PLAYER_OK | úspěch, i u bloků bez zvuku (NULL stream) |
| TMZ_PLAYER_ERROR_NULL_INPUT | parametr je NULL |
| TMZ_PLAYER_ERROR_UNSUPPORTED | Generalized Data, neznámé ID, blok bez přehratelné části (také části mimo meze generátoru), blok, který nejde převést, neplatný blok (parse 0x40 / 0x41, data bloku TZX), blok TZX, který knihovna tzx nepřehraje (CSW Z-RLE) - trvalá chyba bloku |
| TMZ_PLAYER_ERROR_STREAM_CREATE | generátor nevytvořil stream z platného bloku (zápis do streamu) |
| TMZ_PLAYER_ERROR_ALLOC | selhala alokace |
| TMZ_PLAYER_ERROR_UNREADABLE | signál by po převodu na vzorky čtecí rutina nepřečetla (rezerva čtecího bodu pod 0, FSK/SLOW/DIRECT při nečitelné frekvenci) |

Chyby se navíc vypisují přes error callback knihoven (`tzx_set_error_callback()`,
výchozí výpis na stderr). Parse funkce bloků v1.0 mění data bloku (převod
endianity), volají se nad kopií dat.

## Konverze MZF

```
MZF --> tmz_block_from_mzf() --> blok 0x40
blok 0x40 / 0x41 / 0x46 --> tmz_block_to_mzf() --> MZF
MZF --> tmz_profile_build_block() --> blok 0x46 podle profilu
```

## Závislosti

generic_driver, mzf, mztape, cmt_stream, cmtspeed, endianity, tzx,
mzcmt_fm, mzcmt_manchester a kodéry mzcmt_* (cesta v1.0).

## Použití

```c
#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_player.h"

/* načtení souboru (TMZ i TZX) */
en_TZX_ERROR err;
st_TZX_FILE *file = tzx_load ( handler, &err );
if ( !file ) { printf ( "Error: %s\n", tzx_error_string ( err ) ); return; }

/* přehrání celého souboru včetně řídicích bloků */
st_TMZ_PLAYER_CONFIG cfg;
tmz_player_config_init ( &cfg );
st_TMZ_PLAYER_STATE state;
tmz_player_state_init ( &state, file, &cfg );
while ( !tmz_player_state_finished ( &state ) ) {
    en_TMZ_PLAYER_ERROR perr;
    st_CMT_STREAM *stream = tmz_player_play_next ( &state, &perr );
    if ( stream ) {
        /* export do WAV, přehrání v emulátoru, ... */
        cmt_stream_destroy ( stream );
    } else if ( perr != TMZ_PLAYER_OK ) {
        printf ( "Error: %s\n", tmz_player_error_string ( perr ) );
    }
}

tzx_free ( file );
```
