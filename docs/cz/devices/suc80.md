# 80sloupcový kit SUC (MZ-700)

"MZ-700 80 Column Kit" klubu Sharp Users Club UK (1990) přestavuje základní
desku MZ-700 na 80 sloupců x 25 řádků. Nemá vlastní videopaměť: využívá
horní poloviny existující znakové a atributové VRAM, které 40sloupcový
režim nezobrazuje. Emulátor ho nabízí jen na MZ-700 (PAL i NTSC).

> Autor emulátoru zařízení nikdy neviděl. Model stojí jen na rešerši
> informací z internetu (dobový článek se schématem); na reálném hardwaru
> nic měřeno nebylo. Podrobnosti, které prameny nerozhodují, jsou volby,
> ne ověřené chování hardwaru. Software SUC CP/M pro kit není součástí
> emulátoru.

## Připojení kitu

- Menu **HW & Devices** - **SUC 80-column kit** - **Connected**.
- INI soubor, sekce `[SUC80]`, klíč `connected = 1`.
- MCP: `emu_periph_attach` s `kind = "suc80"`.

Výchozí stav je odpojeno. Po připojení zůstává obraz ve 40 sloupcích a vypadá
přesně jako bez kitu, dokud program nepřepne na 80 sloupců. Stav kitu (40
nebo 80 sloupců) se ukládá do snapshotu.

## Přepínání 40 / 80 sloupců

Libovolný zápis na adresy 0E00Ch - 0E00Fh nastaví režim podle datového bitu
D4; ostatní bity nehrají roli:

| Hodnota | D4 | Režim |
|---------|----|-------|
| 28h (40) | 0 | 40 sloupců |
| 50h (80) | 1 | 80 sloupců |

```asm
        LD   A,50h
        LD   (0E00Ch),A     ; 80 sloupců
        ...
        LD   A,28h
        LD   (0E00Ch),A     ; 40 sloupců
```

- Kit zápis jen sleduje; počítač se chová jako bez kitu. Režim nejde
  přečíst zpět a čtení adres ho nemění.
- Kit reaguje jen tehdy, když je na 0E000h namapovaná oblast paměťově
  mapovaných periferií (po resetu, `OUT (0E3h)`, `OUT (0E4h)`), ne po
  `OUT (0E1h)`.
- RESET vrací 40 sloupců.
- Změna režimu platí od dalšího kresleného obrazového řádku; změna uprostřed
  řádku se neemuluje.

## Rozložení VRAM v 80 sloupcích

Každá 40sloupcová znaková pozice zobrazí dva znaky: první z dolní poloviny
znakové VRAM (blok A, 0D000h - 0D3E7h), druhý z horní poloviny (blok B,
0D401h - 0D7E8h). Pro textový řádek r = 0 - 24 a sloupec c = 0 - 79 platí
`k = 40 * r + c / 2` (celočíselné dělení):

| Sloupec | Znak | Atribut barvy (výchozí volba) |
|---------|------|-------------------------------|
| sudé c | 0D000h + k | 0D800h + k |
| liché c | 0D401h + k | 0DC01h + k |

Pořadí na obrazovce je tedy 0D000h, 0D401h, 0D001h, 0D402h, ...; poslední
dvojice obrazovky je 0D3E7h / 0D7E8h. Bajty 0D400h a 0DC00h se nezobrazí
nikdy.

Znaky mají písmo a barvy MZ-700, jen šířka znaku je poloviční (8 bodů
z řádku 640 bodů). Bajt atributu má obvyklý význam: bity 6-4 barva popředí,
bity 2-0 barva pozadí, bit 7 vybírá druhou sadu znaků.

Je-li připojená i karta s programovatelným generátorem znaků, kreslí
80sloupcový obraz její znaky, protože kit mění jen adresování VRAM. Tuto
kombinaci na skutečném hardwaru nikdo nedoložil.

## Volba Attributes in 80 columns

Prameny neříkají, jak se v 80 sloupcích chovají barvy. Volba
**Attributes in 80 columns** (klíč INI `attributes` v `[SUC80]`) vybírá:

| Hodnota v menu (INI) | Význam |
|----------------------|--------|
| Per character (`per_char`, výchozí) | Každý znak má svůj atribut: sudé sloupce 0D800h + k, liché 0DC01h + k. |
| Per pair of characters (`per_pair`) | Oba znaky dvojice berou atribut bloku A (0D800h + k), včetně sady znaků (bit 7). |

Volba platí hned, i za běhu programu. Ve 40 sloupcích nemá vliv.

## Debugger, snímky obrazovky a video

- Okno I/O Ports ukazuje při připojeném kitu v sekci Expansion devices
  položku **Device - SUC latch** (0E00Ch). Hodnota je zrcadlo režimu
  (10h = 80 sloupců, 0 = 40 sloupců), skutečný latch přečíst nejde.
- Snímky obrazovky, nahrávání videa a nástroje MCP pro snímek obrazovky
  obsahují 80sloupcový obraz tak, jak je na obrazovce.
