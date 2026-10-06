# QuickDisk (QD) - uživatelská dokumentace

QuickDisk je sériový diskový subsystém Sharp MZ-1F11 připojený přes SIO
porty 0xF4-0xF7. Emulátor mz800new podporuje tři režimy:

| Režim    | Popis                                                                         |
|----------|-------------------------------------------------------------------------------|
| IMAGE    | logický `.MZQ` nebo fyzický `.qd` obraz QuickDisku                         |
| VIRTUAL  | adresář s .MZF soubory, dynamicky se prezentuje jako QD obsah                 |
| UNICARD  | image, který spravuje UNICARD FW (read-only z pohledu emulátoru)              |

IMAGE a UNICARD běží přes abstrakci nad memory driverem a file driverem -
stejný mechanismus jako FDC. VIRTUAL režim má samostatnou I/O cestu.

IMAGE automaticky rozpozná legacy Sharp logical `.qd`, HxC `HXCQDDRV` a
FlashFloppy physical `.qd`. Fyzický obraz se při připojení dekóduje do
interního logického pracovního bufferu. Při uložení se znovu vytvoří původní
typ kontejneru; FlashFloppy výstup používá canonical QDF/qdf2qd rozmístění
MFM dat od offsetu `0x3A90`, takže zůstává použitelný i na reálném MZ-800.
Dialog **Create New Image** umí vytvořit `.mzq` i všechny tři varianty `.qd`.
Nový `.qd` vytvořený zadáním dosud neexistujícího souboru v mount dialogu je
standardně FlashFloppy physical; legacy a HxC vyžadují explicitní volbu formátu.

## Storage mode (IMAGE / UNICARD)

Volba se persistuje v INI klíči `mz1f11_storage_mode`.

| Mode    | Co se děje                                                                |
|---------|---------------------------------------------------------------------------|
| CACHED  | Celý image se načte do RAM (memory driver). Změny se drží v RAM           |
|         | bufferu, do souboru se zapíšou až při motor-off, umount, exitu emu nebo   |
|         | manuálním "Save now". Default. Nízká I/O režie, riziko ztráty při crashi. |
| DIRECT  | Žádný RAM buffer, každý byte jde přímo na soubor (file driver). Vyšší    |
|         | I/O režie, ale write je perzistentní okamžitě.                            |
| DISCARD | Image se načte do RAM (memory driver), všechny writes končí JEN v RAM    |
|         | bufferu. Při motor-off / close / exit se NIC nezapíše zpět. Pro test runs |
|         | bez modifikace zdrojového souboru. "Save now" tlačítko umí jednorázový    |
|         | force-save i v tomto módu.                                                |

Fyzické `.qd` vždy používají interní RAM buffer, protože jednotlivé logické
SIO bajty nelze zapisovat přímo do MFM kontejneru. Režim DIRECT je proto pro
připojené `.qd` v UI zakázán; CACHED zapisuje celý validovaný kontejner na
sync bodech a DISCARD zdrojový soubor nemění.

### Přepnutí storage mode za běhu

Z UI menu "Storage mode" lze přepnout mode kdykoliv. Pokud existují
neuložené změny (RAM dirty buffer v CACHED/DISCARD) a přepínáte do
jiného modu, otevře se popup se třemi volbami:

1. **Save and switch** - flush RAM bufferu do souboru a teprve pak přepni
2. **Switch and discard** - zahodit RAM změny a přepnout
3. **Cancel** - zrušit přepnutí

## Read-Only 3-state model

Effective R/O = `user_readonly || fs_readonly`.

| Pole          | Význam                                                                 |
|---------------|------------------------------------------------------------------------|
| user_readonly | Persistentní user volba. INI klíč `mz1f11_write_protected`. Mirror v UI |
|               | jako "Write Protected" checkbox.                                       |
| fs_readonly   | Runtime auto-detekce: pokud k souboru nelze získat write access, je 1. |
|               | V UI signalizováno labelem "[FS R/O]" pod checkboxem.                  |
| readonly      | Effective hodnota. Když je 1, zápis přes SIO selže (host vidí write    |
|               | protect error).                                                        |

`fs_readonly` se přepočte při každém otevření image. `user_readonly`
se mění výhradně user akcí přes UI checkbox nebo INI při startu.
Při zavření image se všechny tři vynulují (mount cycle z čistého stavu).

## UNICARD lock

UNICARD režim (boot loader image spravovaný Unicard FW) má speciální
zacházení:

- `user_readonly` se při mount **vynutí** na 1 (boot image se nezapisuje
  z emulovaného systému)
- `storage_mode` se při mount **vynutí** na CACHED
- INI hodnoty `mz1f11_write_protected` a `mz1f11_storage_mode` se
  **nepřepisují** (user pref pro IMAGE režim zůstává intaktní)
- UI controly Write Protected + Storage mode jsou v UNICARD režimu
  gray-out (vizuální feedback že hodnoty nelze měnit)

## QDisk State debugger window

Z menu DBG / "QDisk State" lze otevřít okno s kompletním stavem QD
subsystému:

- **Connection**: typ (IMAGE / VIRTUAL / UNICARD), connected flag
- **Status flags**: IMG_READY, HEAD_HOME, IMG_SYNC, IMG_READONLY
- **Channel A/B SIO**: aktuální registry WR0-WR7, RR0-RR2
- **Image**: filename, image_position
- **Handler info** (jen IMAGE/UNICARD): driver type (memory/file),
  R/O status, dirty flag
- **Storage mode + R/O 3-state**: user_readonly, fs_readonly, readonly
- **Has unsaved changes**: live indikátor (dirty / clean)
- **Virtual scan** (jen VIRTUAL): počet souborů, aktuální file_num,
  open MZF info

Okno se refreshuje automaticky při každém ImGui rendru. Open/close
přes menu DBG / "QDisk State" (toggle).

## Snapshot kompatibilita

Snapshot QD subsystému ukládá:

- chip kontinuita: SIO channel registry, image_position, status
- informativní info: `filename`, effective `readonly`

**Co se NEUKLÁDÁ:**
- Memory buffer obsah `.MZQ`/`.qd` image (= analogie FDC: dirty RAM změny
  se snapshot save+load **ztratí**, doporučujeme před snapshot save
  manuální "Save now" pokud máte CACHED + dirty)
- User INI nastavení (`storage_mode`, `user_readonly`) - INI je SSOT
  pro user pref; snapshot je SSOT pro chip kontinuitu

**Po snapshot load:** mount se provede ze stávajícího INI nastavení
(jak při startu emu), snapshot dopíše SIO chip state na mountnutý
handler.

## Známé limity

- **VIRTUAL nemá storage_mode** - jeho I/O cesta je vždy přímo na soubor
  per .MZF (žádná RAM cache). Storage mode UI control je v VIRTUAL módu
  irrelevantní.
- **Per-drive struktura není** - QD je fyzicky jedna mechanika, druhý
  disk se nedá mountnout (na rozdíl od FDC, kde jsou dva drive sloty A/B).
- **Memory buffer `.MZQ`/`.qd` se ne-persistuje v snapshotu** - viz výše.

## Referenční INI klíče

| Klíč                          | Typ     | Default  | Význam                              |
|-------------------------------|---------|----------|-------------------------------------|
| `mz1f11_connected`            | BOOL    | 0        | QD subsystém zapnutý                |
| `mz1f11_type`                 | INT     | 0        | 0=IMAGE, 1=VIRTUAL, 2=UNICARD       |
| `mz1f11_std_filepath`         | TEXT    | ""       | Cesta k `.MZQ` nebo `.qd` image     |
| `mz1f11_virt_dirpath`         | TEXT    | ""       | Cesta k adresáři s .MZF soubory     |
| `mz1f11_write_protected`      | BOOL    | 0        | user_readonly                       |
| `mz1f11_storage_mode`         | TEXT    | "cached" | "cached" / "direct" / "discard"     |
