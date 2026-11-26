
# Plán dalších vlastností

- debugger: další varianty krokování v okně debuggeru - do dalšího řádku obrazu, do opuštění ROM, do návratu z podprogramu (přes MCP už jsou emu_step_out a emu_run_until_raster)
- VRAM viewer, především v MZ-800 s možností přepínat si režimy zobrazení a modifikovat palety
- MZ-800: CG-RAM editor
- MZ-1500: PCG viewer - přehled všech znaků PCG banky najednou (editor jednotlivých znaků už je součástí Memory Browseru)
- LUA scripting
- sběrnice rozšiřujících jednotek (zatím jen MZ-700) i pro MZ-800 a MZ-1500
- možnost připojovat externí moduly/knihovny na IORQ
- Hledání referencí: kde všude se adresa čte (zápisy už ukazuje RAM Write Log)
- Back step: krok zpět po instrukcích v debuggeru (návrat po snímcích umí Replay)
- overlay menu: i zbývající nastavení hardwaru
- podpora gdb


## Emulace

- MZ-700 NTSC (japonská verze): poloha obrazu v řádku, čekání při přístupu do VRAM a vstup čítače CTC0 jsou převzaté z MZ-1500, který má stejný krystal, čekací takt při čtení z monitorové ROM z evropského MZ-700; na japonském MZ-700 změřené nejsou.
- Přesný takt: emulátor odvozuje takt od celých snímků za sekundu, ne od krystalu. PAL běží na 17 721 600 Hz místo 17 734 475 Hz (o 0,07 % pomaleji), NTSC na 14 336 640 Hz místo 14 318 180 Hz (o 0,13 % rychleji); stejně se posouvá všechno odvozené, včetně tónů CTC0. V plánu je přesnější emulace: počet taktů na snímek zůstane stejný, ale převod na skutečný čas (zvuk, nahrávání AVI, zápis a čtení WAV kazety, synchronizace bez zvuku) bude vycházet z frekvence krystalu, případně jako volitelné nastavení.

### Čítač 8253: známé rozdíly proti skutečnému počítači

Čítač je opraven podle měření logickým analyzátorem na MZ-800. Dva případy se dál chovají jinak než skutečný čip, třetí bod se týká MZ-700 a MZ-1500:

- **Čtení čítače za běhu bez povelu zachycení.** Program může čítač číst dvěma způsoby: nejdřív poslat povel zachycení (hodnota se zmrazí a pak se v klidu přečte), nebo číst rovnou za běhu. Na skutečném počítači čtení za běhu občas vrátí nesmysl: CTC0 se mění 1,1 milionkrát za sekundu, čtecí pulz trvá 550 ns, hodnota se často změní uprostřed čtení a část bitů se přečte ze staré a část z nové hodnoty (naměřeno například F8h místo FBh). Emulátor vrací vždy čistou hodnotu. Pravidlo, kdy a jak se hodnota rozpadne, se z měření určit nepodařilo: rozpadlá hodnota přichází i tehdy, když během čtení žádná hrana hodin není, a jen část z nich je smíchání staré a nové hodnoty. Důsledek: program, který čte bez zachycení, běží v emulátoru spolehlivěji než na skutečném počítači. Programy, které zachycení používají, se neliší. K upřesnění by bylo potřeba měření i s datovou sběrnicí.
- **Dvoubajtový dělič přerušený řídicím slovem pro jiný čítač.** Šestnáctibitový dělič se zapisuje na dvakrát, dolní bajt a pak horní. Když na skutečném počítači mezi ty dva bajty přijde řídicí slovo pro jiný čítač (měřeno: dělič CTC0, řídicí slovo 54h pro CTC1), dolní bajt se ztratí: místo zapsaného děliče 0020h se CTC0 rozběhl s děličem mezi 13 a 255, v každém opakování jiným. Povel zachycení téhož čítače mezi bajty nevadí. Emulátor dělič nahraje správně, jako by řídicí slovo mezi bajty nebylo. Čím čip dolní bajt nahradí, není známo, a hodnota vypadá náhodně. Důsledek: program s touto chybou by na skutečném počítači hrál špatný tón nebo měl špatné časování, v emulátoru běží správně.
- **Fáze vstupních hodin čítače vůči obrazovému řádku.** Na MZ-800 je okamžik, kdy vstupní hodiny čítače CTC0 tiknou vůči řádku obrazu, nastavený podle měření. Na MZ-700 a MZ-1500 změřený není. Projevit se to může jen u programu, který se řídí polohou paprsku a zároveň nahrává dělič CTC0: nový dělič se pak může uplatnit o jednu periodu vstupních hodin (asi 1 mikrosekundu) jinak než na skutečném počítači.

### Neměřeno nebo vědomě ponecháno

Tyto rozdíly proti skutečným počítačům jsou známé. Buď je zatím nikdo nezměřil, nebo v emulátoru zůstávají záměrně.

- **Prodloužené zápisy na porty 0E0h a 0E4h (MZ-700 PAL a MZ-1500).** Na skutečném MZ-700 PAL trvá zápis na port 0E0h o 1 takt procesoru déle a zápis na 0E4h o 2 takty déle. Na MZ-1500 trvá o 1 takt déle zápis na 0E4h, zápis na 0E0h normálně. Měřeno bylo jen s připojenou kartou Unicard; emulátor tyto zápisy neprodlužuje. Pravděpodobně je prodlužuje karta, ne počítač (MZ-800 bez karty je neprodlužuje). Rozhodne měření bez karty.
- **Paleta MZ-1500 po resetu.** Jakou paletu (port 0F1h) má skutečný MZ-1500 po resetu, změřené není.
- **Útlum kanálů PSG.** Hlasitost kanálu PSG se nastavuje čtyřmi bity útlumu. V emulátoru mají přesně 2, 4, 8 a 16 dB podle katalogového listu, na skutečném MZ-800 naměřeno 1,89, 3,96, 7,75 a 16,11 dB. Rozdíl je nejvýš 0,25 dB.
- **Kanál PSG s děličem tónu 001h.** Skutečný čip vydává obdélník 110,8 kHz, který za výstupní propustí působí jako stálá úroveň asi poloviční výchylky (0,49); takto se přehrávají vzorky. Emulátor obdélník negeneruje a vydává rovnou tuto stálou úroveň.
- **Výstupní propust.** Na skutečném MZ-800 závisí propust na poloze knoflíku hlasitosti a zvuk dál mění zesilovač a reproduktor. Emulátor má (volba Hardware output filter) pevnou propust prvního řádu se zlomem 346 Hz, změřenou při hlasitosti na maximu; zesilovač a reproduktor nemodeluje.
- **Zvuk MZ-1500 a MZ-700.** Na MZ-1500 jsou výstupní propust a střídavé vazby převzaté z MZ-800, změřené nejsou. Zvuk samostatného MZ-700 změřený není vůbec: že výstup nemá propust, vychází ze schématu, a střídavá vazba, která brání lupání při stálé úrovni, je pomůcka emulátoru, ne vlastnost počítače.
- **Kazeta: délky pulzů zapisovaných z ROM.** Délky pulzů, které na pásku zapisují rutiny ROM MZ-700 a MZ-1500, na skutečných počítačích změřené nejsou. V emulátoru vycházejí z běhu ROM s čekacím taktem při čtení z monitorové ROM, který změřený je.


# Chyby čekající na bugfix

- **topmenu**: Když uživatel použije k navigaci kurzory, jsou načítány jako vstup z klávesnice do emulace (zvážit, zda toto chování půjde potlačit)
