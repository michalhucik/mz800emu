# QuickDisk (QD) - uživatelská dokumentace

QuickDisk je sériový diskový subsystém Sharp MZ-1F11 připojený přes SIO
porty 0xF4-0xF7. Emulátor mz800new podporuje tři režimy:

| Režim    | Popis                                                                         |
|----------|-------------------------------------------------------------------------------|
| IMAGE    | klasický .MZQ obraz QD diskety (typicky 64 kB užitečně, max ~92 kB)           |
| VIRTUAL  | adresář s .MZF soubory, dynamicky se prezentuje jako QD obsah                 |
| UNICARD  | image, který spravuje UNICARD FW (read-only z pohledu emulátoru)              |

IMAGE a UNICARD běží přes abstrakci nad memory driverem a file driverem -
stejný mechanismus jako FDC. VIRTUAL režim má samostatnou I/O cestu.

Médium se vkládá v menu HW & Devices > Quick Disk > QD Drive: **Mount
Image...** (režim IMAGE) nebo **Mount Directory...** (režim VIRTUAL) a
podnabídka **Recent** s naposledy vloženými obrazy nebo adresáři (nejvýš
12, celá cesta v tooltipu). Obojí je dostupné jen s prázdnou mechanikou.
Recent nabízí jen položky, které mechanika v současném režimu bere (soubor
obrazu, nebo adresář); soubor, který už neexistuje, se nevloží, z historie
se odstraní a emulátor to ohlásí. Hned po otevření podnabídky může klik
skončit zprávou "Still checking whether the file exists. Try again in a
moment", než emulátor na pozadí zjistí, zda soubory existují.

## Storage mode (IMAGE / UNICARD)

Volba se persistuje v INI klíči `mz1f11_storage_mode`.

| Mode    | Co se děje                                                                |
|---------|---------------------------------------------------------------------------|
| CACHED  | Celý .MZQ image se načte do RAM (memory driver). Změny se drží v RAM      |
|         | bufferu, do souboru se zapíšou až při motor-off, umount, exitu emu nebo   |
|         | manuálním "Save now". Default. Nízká I/O režie, riziko ztráty při crashi. |
| DIRECT  | Žádný RAM buffer, každý byte jde přímo na soubor (file driver). Vyšší    |
|         | I/O režie, ale write je perzistentní okamžitě.                            |
| DISCARD | Image se načte do RAM (memory driver), všechny writes končí JEN v RAM    |
|         | bufferu. Při motor-off / close / exit se NIC nezapíše zpět. Pro test runs |
|         | bez modifikace zdrojového souboru. "Save now" tlačítko umí jednorázový    |
|         | force-save i v tomto módu.                                                |

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
- Memory buffer obsah .MZQ image (snapshot obsah disku nenese; neuložené
  změny zůstávají po načtení snapshotu v mechanice, viz níže)
- User INI nastavení (`storage_mode`, `user_readonly`) - INI je SSOT
  pro user pref; snapshot je SSOT pro chip kontinuitu
- Stav virtuálního Quick Disku (který soubor MZF je rozečtený); snapshot
  uložený uprostřed čtení nebo zápisu ve virtuálním režimu proto po načtení
  nemusí pokračovat správně

**Po snapshot load:** vložený Quick Disk patří k emulátoru, ne ke stavu
emulovaného počítače. Načtení snapshotu disk nevkládá, nevyměňuje ani
nevysouvá: mechanika si nechá obraz, který v ní byl před načtením, i s jeho
jménem souboru, ochranou proti zápisu, režimem ukládání a neuloženými
změnami; prázdná mechanika zůstane prázdná. Ze snapshotu se obnoví stav
čipu SIO a mechaniky (poloha hlavy, synchronizace). Zda emulovaný počítač
vidí v mechanice disk a zda je chráněný proti zápisu, se řídí tím, co je
skutečně vložené, ne snapshotem. Neuložené změny se dál ukládají do
souboru, ze kterého byl obraz vložen.

Pokud byl při uložení snapshotu vložený jiný Quick Disk než teď (jiný
soubor, prázdná mechanika místo plné nebo naopak), emulátor to po načtení
ohlásí řádkem ve zprávě společné s disketami (viz fdc.md):

```
The snapshot was saved with different disks:
Quick Disk: game.mzq (now: work.mzq)
The inserted disks were kept.
```

Emulovaný program pak pokračuje nad jiným obsahem, než s jakým počítal;
poloha hlavy zůstává ze snapshotu, takže program, který byl při uložení
uprostřed čtení, čte od stejného místa jiný disk. Chcete-li pokračovat
přesně tam, kde snapshot vznikl, vložte disk uvedený ve zprávě a snapshot
načtěte znovu. Opakované načtení se stejným rozdílem zprávu znovu
neukazuje. Adresář virtuálního Quick Disku snapshot nenese, proto se
neporovnává.

Typ mechaniky (obraz, virtuální adresář, Unicard) a její připojení jsou
součást stavu počítače a berou se ze snapshotu jen pro běh; INI si drží
volbu uživatele. Když snapshot přepne mechaniku mezi obrazem (nebo
Unicardem) a virtuálním adresářem, vložené médium k novému druhu mechaniky
nepatří: emulátor ho zavře jako při vysunutí (neuložené změny uloží do
jeho souboru) a vloží médium, které má pro nový druh mechaniky v INI,
stejně jako při startu. Mezi typy obraz a Unicard zůstává vložený obraz
na místě.

**Obraz se neuloží do jiného souboru, než ze kterého byl načten.** Tuto
pojistku má mechanika navíc: kdyby jméno souboru u mechaniky označovalo
jiný soubor, než ze kterého mechanika svůj obraz načetla, emulátor kopii
z paměti do souboru nezapíše (Save now, vypnutí motoru, vysunutí,
ukončení, "Save and switch") a ohlásí "The Quick Disk was not saved: the
drive now refers to a different image file than the one it was loaded
from. Re-insert the image to continue." Neuložené změny pak zůstávají jen
v paměti a při vysunutí, výměně obrazu nebo ukončení emulátoru se zahodí.
Po běžném načtení snapshotu k tomu nedochází. Nastat to může po přehrání
staršího záznamu vstupů, který nenese seznam médií, a po návratu zpět
v čase během nahrávání záznamu vstupů: tam emulátor mechanice vrací jméno
souboru ze snapshotu, aby přehrání odpovídalo nahrávce.

## Známé limity

- **VIRTUAL nemá storage_mode** - jeho I/O cesta je vždy přímo na soubor
  per .MZF (žádná RAM cache). Storage mode UI control je v VIRTUAL módu
  irrelevantní.
- **Per-drive struktura není** - QD je fyzicky jedna mechanika, druhý
  disk se nedá mountnout (na rozdíl od FDC, kde jsou dva drive sloty A/B).
- **Memory buffer .MZQ se ne-persistuje v snapshotu** - viz výše.
- **Stav virtuálního Quick Disku se v snapshotu neukládá** - viz výše.

## Referenční INI klíče

| Klíč                          | Typ     | Default  | Význam                              |
|-------------------------------|---------|----------|-------------------------------------|
| `mz1f11_connected`            | BOOL    | 0        | QD subsystém zapnutý                |
| `mz1f11_type`                 | INT     | 0        | 0=IMAGE, 1=VIRTUAL, 2=UNICARD       |
| `mz1f11_std_filepath`         | TEXT    | ""       | Cesta k .MZQ image                  |
| `mz1f11_virt_dirpath`         | TEXT    | ""       | Cesta k adresáři s .MZF soubory     |
| `mz1f11_write_protected`      | BOOL    | 0        | user_readonly                       |
| `mz1f11_storage_mode`         | TEXT    | "cached" | "cached" / "direct" / "discard"     |
