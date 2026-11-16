
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

- Přeměřit na reálném HW: `VIDEO_H_BACK_PORCH_TICKS`, `VIDEO_H_FRONT_PORCH_TICKS` (aktuálně 104+39=143, ale přesný split není ověřený)
- Zvážit emulaci částečně obsazené VRAM u MZ-800
- MZ-1500 gdg: ověřit nepřítomnost VRAM latch

### Čítač 8253: známé rozdíly proti skutečnému počítači

Čítač je opraven podle měření logickým analyzátorem na MZ-800. Tři případy se dál chovají jinak než skutečný čip:

- **Čtení čítače za běhu bez povelu zachycení.** Program může čítač číst dvěma způsoby: nejdřív poslat povel zachycení (hodnota se zmrazí a pak se v klidu přečte), nebo číst rovnou za běhu. Na skutečném počítači čtení za běhu občas vrátí nesmysl: CTC0 se mění 1,1 milionkrát za sekundu, čtecí pulz trvá 550 ns, hodnota se často změní uprostřed čtení a část bitů se přečte ze staré a část z nové hodnoty (naměřeno například F8h místo FBh). Emulátor vrací vždy čistou hodnotu. Pravidlo, kdy a jak se hodnota rozpadne, se z měření určit nepodařilo: rozpadlá hodnota přichází i tehdy, když během čtení žádná hrana hodin není, a jen část z nich je smíchání staré a nové hodnoty. Důsledek: program, který čte bez zachycení, běží v emulátoru spolehlivěji než na skutečném počítači. Programy, které zachycení používají, se neliší. K upřesnění by bylo potřeba měření i s datovou sběrnicí.
- **Dvoubajtový dělič přerušený řídicím slovem pro jiný čítač.** Šestnáctibitový dělič se zapisuje na dvakrát, dolní bajt a pak horní. Když na skutečném počítači mezi ty dva bajty přijde řídicí slovo pro jiný čítač (měřeno: dělič CTC0, řídicí slovo 54h pro CTC1), dolní bajt se ztratí: místo zapsaného děliče 0020h se CTC0 rozběhl s děličem mezi 13 a 255, v každém opakování jiným. Povel zachycení téhož čítače mezi bajty nevadí. Emulátor dělič nahraje správně, jako by řídicí slovo mezi bajty nebylo. Čím čip dolní bajt nahradí, není známo, a hodnota vypadá náhodně. Důsledek: program s touto chybou by na skutečném počítači hrál špatný tón nebo měl špatné časování, v emulátoru běží správně.
- **CTC0 v režimu 3 s děličem 2 není slyšet.** S děličem 2 dává CTC0 obdélník 554 kHz, daleko nad slyšitelným pásmem. Emulátor tento jeden případ do zvuku nepouští (ani do záznamu zvuku a do stop zvuku pro ladění); čítač sám čítá správně. Důvodem je hra Ralye (disk Tatra-sys pro CP/M), která CTC0 takto nastaví a povolí reproduktor; na skutečném počítači ten zvuk zřejmě neprojde filtrem zesilovače. Jak takový signál zesilovačem skutečně projde, změřeno není.


# Chyby čekající na bugfix

- **topmenu**: Když uživatel použije k navigaci kurzory, jsou načítány jako vstup z klávesnice do emulace (zvážit, zda toto chování půjde potlačit)
