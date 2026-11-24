
# Plán dalších vlastností

- debugger Step variants (Scanline step, until ROM exit, until SP unwind, ...)
- VRAM viewer, především v MZ-800 s možností přepínat si režimy zobrazení a modifikovat palety
- MZ-800: CG-RAM editor
- MZ-1500: PCG viewer - přehled všech znaků PCG banky najednou (editor jednotlivých znaků už je součástí Memory Browseru)
- LUA scripting
- dynamická IORQ sběrnice, plug-and-play
- možnost připojovat externí moduly/knihovny na IORQ
- Hledání referencí (kde všude se čte/zapisuje právě tohle)
- Back step
- vytvoření kompletního overscreen menu
- podpora gdb


## Emulace

- MZ-700 a MZ-1500: přeměřit na reálném HW `VIDEO_H_BACK_PORCH_TICKS` a `VIDEO_H_FRONT_PORCH_TICKS` (MZ-700 NTSC a MZ-1500 aktuálně 104 + 39, MZ-700 PAL 136 + 216; přesné rozdělení není ověřené). MZ-800 je na skutečném počítači ověřený: HSYNC 80, back porch 106, zobrazení 928, front porch 22 bodů, řádek 1136 bodů.
- MZ-1500 gdg: ověřit nepřítomnost VRAM latch
- Přesný takt: emulátor odvozuje takt od celých snímků za sekundu, ne od krystalu. PAL běží na 17 721 600 Hz místo 17 734 475 Hz (o 0,07 % pomaleji), NTSC na 14 336 640 Hz místo 14 318 180 Hz (o 0,13 % rychleji); stejně se posouvá všechno odvozené, včetně tónů CTC0. V plánu je přesnější emulace: počet taktů na snímek zůstane stejný, ale převod na skutečný čas (zvuk, nahrávání AVI, zápis a čtení WAV kazety, synchronizace bez zvuku) bude vycházet z frekvence krystalu, případně jako volitelné nastavení.

### Čítač 8253: známé rozdíly proti skutečnému počítači

Čítač je opraven podle měření logickým analyzátorem na MZ-800. Dva případy se dál chovají jinak než skutečný čip:

- **Čtení čítače za běhu bez povelu zachycení.** Program může čítač číst dvěma způsoby: nejdřív poslat povel zachycení (hodnota se zmrazí a pak se v klidu přečte), nebo číst rovnou za běhu. Na skutečném počítači čtení za běhu občas vrátí nesmysl: CTC0 se mění 1,1 milionkrát za sekundu, čtecí pulz trvá 550 ns, hodnota se často změní uprostřed čtení a část bitů se přečte ze staré a část z nové hodnoty (naměřeno například F8h místo FBh). Emulátor vrací vždy čistou hodnotu. Pravidlo, kdy a jak se hodnota rozpadne, se z měření určit nepodařilo: rozpadlá hodnota přichází i tehdy, když během čtení žádná hrana hodin není, a jen část z nich je smíchání staré a nové hodnoty. Důsledek: program, který čte bez zachycení, běží v emulátoru spolehlivěji než na skutečném počítači. Programy, které zachycení používají, se neliší. K upřesnění by bylo potřeba měření i s datovou sběrnicí.
- **Dvoubajtový dělič přerušený řídicím slovem pro jiný čítač.** Šestnáctibitový dělič se zapisuje na dvakrát, dolní bajt a pak horní. Když na skutečném počítači mezi ty dva bajty přijde řídicí slovo pro jiný čítač (měřeno: dělič CTC0, řídicí slovo 54h pro CTC1), dolní bajt se ztratí: místo zapsaného děliče 0020h se CTC0 rozběhl s děličem mezi 13 a 255, v každém opakování jiným. Povel zachycení téhož čítače mezi bajty nevadí. Emulátor dělič nahraje správně, jako by řídicí slovo mezi bajty nebylo. Čím čip dolní bajt nahradí, není známo, a hodnota vypadá náhodně. Důsledek: program s touto chybou by na skutečném počítači hrál špatný tón nebo měl špatné časování, v emulátoru běží správně.


# Chyby čekající na bugfix

- **topmenu**: Když uživatel použije k navigaci kurzory, jsou načítány jako vstup z klávesnice do emulace (zvážit, zda toto chování půjde potlačit)
- **MZ-700 NTSC, CTC0**: dělič 13 dává vstup CTC0 asi 1,1 MHz, ale japonská ROM 1Z-009B má tabulku not spočítanou pro asi 895 kHz (krystal / 16). S japonskou ROM proto hrají tóny asi o 23 % výš (pípnutí asi 1085 Hz místo 880 Hz). Ověřit měřením na MZ-1500, který má stejný krystal, a opravit.
