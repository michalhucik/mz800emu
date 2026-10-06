
# Plán dalších vlastností

- debugger Step variants (Scanline step, until ROM exit, until SP unwind, ...)
- VRAM viewer, především v MZ-800 s možností přepínat si režimy zobrazení a modifikovat palety
- MZ-800: CG-RAM editor
- MZ-1500: PCG viewer - přehled všech znaků PCG banky najednou (editor jednotlivých znaků už je součástí Memory Browseru)
- LUA scripting
- Chytrý mem search
- dynamická IORQ sběrnice, plug-and-play
- možnost připojovat externí moduly/knihovny na IORQ
- Hledání referencí (kde všude se čte/zapisuje právě tohle)
- Reverse trace (kdo zapsal hodnotu na adrese X?)
- Back step
- Cheat search
- vytvoření kompletního overscreen menu
- podpora TapeMZ a online převod z wav na mzf
- podpora gdb


## Emulace

- Přeměřit na reálném HW: `VIDEO_H_BACK_PORCH_TICKS`, `VIDEO_H_FRONT_PORCH_TICKS` (aktuálně 104+39=143, ale přesný split není ověřený)
- Zvážit emulaci částečně obsazené VRAM u MZ-800
- MZ-1500 gdg: ověřit nepřítomnost VRAM latch


# Chyby čekající na bugfix

- **topmenu**: Když uživatel použije k navigaci kurzory, jsou načítány jako vstup z klávesnice do emulace (zvážit, zda toto chování půjde potlačit)
- **Chyba v emulaci MZ-800**: Už velice dlouho je pocit, že když hraje Flappy demo, tak se při HW scrollu zrychluje tempo hudby. To by naznačovalo, že v GDG máme nějaký další nedokumentovaný stav vyvolávající CPU WAIT. Tohle je potřeba ověřit měřením na reálném HW.
