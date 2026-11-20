# Zachycení zvuku

Zaznamená do složky, co dělá zvukový hardware emulovaného počítače,
v emulovaném čase - pro hledání chyb zvuku v programu i v emulátoru.
Všechny platformy (MZ-800, MZ-700 PAL a NTSC, MZ-1500); MZ-700 má jen
reproduktor.

## Spuštění a zastavení

| Cesta | Jak |
|-------|-----|
| Menu | **Tools -> Audio Capture -> Record Audio Capture** (`Alt + Q`) zachycení spustí; stejná položka nebo klávesa ho zastaví. |
| Příkazová řádka | `--audio-capture <složka>` ho spustí hned po startu, `--audio-capture-frames <počet>` ho zastaví po zadaném počtu snímků; v režimu `--headless` pak emulátor skončí. |
| MCP | `emu_audio_capture_start`, `emu_audio_capture_stop`, `emu_audio_capture_status`; čtení paměti zachycení `emu_trace_read` (`audio_*`), `emu_audio_capture_summary`, `emu_audio_capture_pcm` (viz [`mcp-server/tools-overview.md`](mcp-server/tools-overview.md) a níže). |

Z menu vytvoří každé zachycení novou složku `<platforma>_audio_RRRRMMDD_HHMMSS`
ve výstupní složce (výchozí: `audio_captures` vedle emulátoru; používá ji
i MCP bez `dir`). Za běhu ukazuje levý horní roh obrazu **AUDIO** a zachycený
čas (v češtině **ZVUK**); po konci notifikace ohlásí složku. **Open Output
Folder** otevře výstupní složku ve správci souborů.

**Tools -> Audio Capture -> Audio Capture Control...** (`Alt + Shift + Q`)
otevře plovoucí okno, které může zůstat otevřené i při hraní: stav
(zachytává, spouští se, vypnuto), zachycený čas, složku zachycení, počet
změn úrovní a zápisů PSG, výsledek posledního zachycení, soubory, které se
zapíšou, a výstupní složku. Jeho tlačítka zachycení spustí nebo ukončí
(`Alt + Q`) a otevřou výstupní složku a nastavení. Okno, které bylo
otevřené, se po novém spuštění emulátoru otevře znovu.

Zachycení začne na konci nejbližšího snímku; když se zapisuje `sdl.wav`, až
po dalších šesti snímcích (asi 0,1 s), během kterých se ustálí zvukový
výstup. Skončí stopem, po zadaném počtu snímků, po jedné hodině, nebo když
emulovaný čas začne znovu (reset, načtení snapshotu).

## Nastavení

**Tools -> Audio Capture -> Settings...** nastavuje výstupní složku a vybírá
soubory k zápisu; ukládá se do konfigurace (sekce `[AUDIO_CAPTURE]`).
Příkazová řádka používá stejný výběr souborů.

| Nastavení | Klíč INI | Výchozí |
|-----------|----------|---------|
| Výstupní složka | `output_dir` | prázdný = `audio_captures` vedle emulátoru |

| Soubor | Klíč INI | Obsah (výchozí vše zapnuto) |
|--------|----------|-------|
| `events.csv` | `events` | každá změna výstupní úrovně zvukového kanálu |
| `psg_writes.csv` | `psg_writes` | každý zápis do PSG s dekódováním |
| `reference.wav` | `reference_wav` | zvuk spočítaný přesně v emulovaném čase |
| `sdl.wav` | `sdl_wav` | zvuk ze zvukového výstupu - to, co slyšíte |

`capture.json` s popisem zachycení se zapisuje vždy.

## Soubory

Časy v CSV: `ticks` = takty hodin videočipu od začátku zachycení (frekvence
je `gdg_clk_hz` v capture.json), `time_us` = totéž v mikrosekundách,
`frame` = snímek od začátku (20 ms PAL, 16,7 ms NTSC).

**events.csv** - `ticks,time_us,frame,channel,level`. Kanály: `ctc0`
(reproduktor, úroveň 0, 15 nebo 31), `psg0_0` .. `psg0_3` (PSG, kanál 3 = šum) a na
MZ-1500 nebo MZ-800 s druhým PSG `psg1_0` .. `psg1_3`. Úroveň 0-15 je výstup
kanálu (u PSG 15 minus útlum, když je výstup generátoru v jedničce, jinak 0).
Úroveň 16-30 má tónový kanál PSG s děličem 1: stálá úroveň 0,49 plné výchylky
stupně `level - 15` (na skutečném čipu výstup kmitá 110,8 kHz, slyšet je jen
střední úroveň; používá se k přehrávání vzorků přes hlasitost).
Úroveň 31 má jen kanál `ctc0`, když čítač CTC0 běží v režimu 3 s děličem 2
a bit PC0 je 1: obdélník 554 kHz se nezapisuje po hranách, kanál drží stálou
úroveň 0,64 úrovně 15 (změřeno na skutečném MZ-800); při PC0 = 0 je úroveň 0.
V emulátorech MZ-700 znamená úroveň 31 polovinu úrovně 15 (0,50, odvozeno ze
schématu) a bit PC0 na ni nemá vliv.
První řádky (`ticks` 0) jsou úrovně na začátku.

**psg_writes.csv** - `ticks,time_us,frame,psg,byte,channel,reg,value`:
`psg` = cíl (1 = PSG0, 2 = PSG1, 3 = oba), `byte` = zapsaná hodnota (hex),
`channel` 0-3, `reg` a `value` ze stavu PSG po zápisu:

| reg | value |
|-----|-------|
| `tone_lo` | latch bajt tónu: spodní 4 bity děliče (dělič se změní až dalším datovým bajtem) |
| `tone` | celý 10bitový dělič |
| `volume` | útlum 0-15 (15 = vypnuto) |
| `noise` | `(typ << 2) \| rychlost` |

**Úrovně v reference.wav a sdl.wav** - oba soubory obsahují výstup za
dorovnáním hlasitosti a měkkým omezovačem: součet zdrojů se násobí 2,51
(+8 dB) se zapnutou výstupní propustí a 1,58 (+4 dB) s vypnutou
a v emulátorech MZ-700. Do výchylky 0,7 plného rozsahu (22 937 z 32 767) je
výstup přesně lineární, takže poměry mezi zdroji platí beze změny; větší
špičky jsou plynule stlačené k plnému rozsahu a neoříznou se. Jeden kanál
PSG s útlumem 0 má po ustálení rozkmit 0,25 krát násobitel, reproduktor
(CTC0) 1,49krát tolik. Proti verzi 2.0.7 jsou úrovně o 8 dB (propust
zapnutá) nebo o 4 dB (vypnutá) vyšší. Hlavní hlasitost a ztlumení v souborech
nejsou.

**reference.wav** a **sdl.wav** - 44100 Hz, 16 bit, stereo, stejný formát,
lze je přímo porovnat. reference.wav je spočítaný přesně v emulovaném čase,
sdl.wav je to, co vyrobil zvukový výstup emulátoru. Jejich rozdíl ukazuje
vady zvukového výstupu. sdl.wav je opožděný: bez zvukového zařízení
(`--headless`) přesně o jeden snímek, se zařízením podle jeho bufferu. Se
zařízením při MAX SPEED zvukový výstup snímky vynechává - capture.json má pak
`sdl_blocks` menší než `frames`.

**capture.json** - platforma, `gdg_clk_hz`, `frames_per_sec`, `wav_rate`,
názvy kanálů, počty (`frames`, `events`, `psg_writes`, `reference_samples`,
`sdl_blocks`, `sdl_samples`) a `stop_reason`. Dále časy v taktech GDG od
spuštění emulace (stejná časová osa jako `total_screens` *
`screen_total_ticks_per_frame` + `total_ticks` v MCP `emulator://periph/gdg`):

- `began` - zachycení opravdu začalo (`false`, když se zastavilo ještě
  během čekání na konec snímku nebo na rozjezd zvukového výstupu),
- `origin_ticks` - začátek zachycení = `ticks` 0 v CSV a první vzorek
  reference.wav,
- `end_ticks` - konec posledního dokončeného snímku = konec reference.wav,
- `last_event_ticks` - čas posledního řádku CSV; po ručním zastavení mezi
  snímky může ležet za `end_ticks` (k těmto událostem už reference.wav
  zvuk nemá),

(bez začátku nebo bez řádků CSV jsou hodnoty `null`) a `clocks` - hodiny
platformy se stejnými názvy jako v MCP `emulator://platform/info`:
`gdg_base_hz` (zaokrouhlená frekvence, na které běží emulovaný čas),
`gdg_real_base_hz` (skutečný krystal), `cpu_divider`, `ctc0_divider`,
`psg_divider` (bez PSG `null`) a `screen_total_ticks_per_frame`.

## Čtení zachycení přes MCP (paměť)

Zachycení spuštěné přes MCP drží data i v paměti (`memory`, přes MCP
výchozí zapnuté, z menu a příkazové řádky vypnuté). MCP klient je čte za
běhu zachycení i po jeho konci až do dalšího startu - bez přístupu
k souborům, tedy i po síti. Zachycení jen do paměti (všechny čtyři soubory
vypnuté) nezakládá složku.

| Co | Nástroj |
|----|---------|
| zápisy do PSG, změny úrovní (řádky CSV) | `emu_trace_read` kanály `audio_psg_writes`, `audio_levels` |
| souhrn každého snímku (změny úrovně, čas s úrovní > 0 a RMS úrovně po kanálech, RMS a špička reference a zvukového výstupu) | `emu_trace_read` kanál `audio_frames` |
| co se dělo: úseky ticha a zvuku, noty hrané PSG, hraje reproduktor tón nebo 1bitové PCM (odhad), zpoždění a rozdíl zvukového výstupu proti referenci | `emu_audio_capture_summary` |
| krátké okno PCM jako WAV (převzorkovaný náhled, nebo přesné vzorky reference.wav / sdl.wav při 44100 Hz stereo) | `emu_audio_capture_pcm` |

Paměť: změny úrovní 1M záznamů (16 MiB), zápisy do PSG 256k (6 MiB),
souhrny snímků na 10 minut, PCM reference na 60 s (10,6 MB za minutu;
zvukový výstup s `pcm` `sdl`). Po naplnění se zahazují nejstarší záznamy.
Noty a výšku tónu PSG počítá vzorec PSG emulátoru (stejný jako PSG Audio
Scope debuggeru).

## Příklad

```
mz800emu --headless --run-mzf hra.mzf --audio-capture C:/tmp/snd --audio-capture-frames 500
```

Zachytí 10 sekund hry (PAL) do `C:/tmp/snd` a skončí.
