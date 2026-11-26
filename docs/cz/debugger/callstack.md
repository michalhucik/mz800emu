# Callstack okno - shadow stack volaných rutin

## 1. Co je to a k čemu

**Callstack** je samostatné dokovatelné okno zobrazující rekonstrukci
hierarchie aktuálně rozpracovaných volání (CALL / RST / IRQ / NMI).
Slouží k vizualizaci hloubky vnořených volání a k diagnostice atypických
přechodů (PUSH+RET trampolíny, longjmp patterny, IRQ accept přerušující
výpočet).

**Není to pohled na surovou paměť zásobníku** - tu poskytuje sibling
okno **Stack Monitor** ([stack-window.md](stack-window.md)). Stack
Monitor ukazuje **co reálně leží na CPU stacku v RAM**; Callstack
ukazuje **rekonstrukci kdo koho zavolal** vytvořenou paralelně se stavem
CPU. Obě okna se používají současně - Stack Monitor pro RAM data
(PUSH/POP hodnoty), Callstack pro hierarchii volání.

### Proč shadow buffer a ne parser SP?

Z80 nemá ABI - žádný frame pointer, žádná prologue/epilogue konvence.
Z paměti CPU stacku nelze deterministicky rekonstruovat call hierarchii
(rutina si může POP-nout svou return adresu jako data, manipulovat SP,
push libovolných hodnot atd.). Proto Callstack **vede vlastní shadow
buffer**, do kterého pushne při CALL/RST/IRQ accept a popne při
RET/RETI/RETN. Je to heuristika, ne perfect rekonstrukce - viz sekce
Limitace.

### Pokryté události

Pokrývá CALL/RET (Z80 `CALL nn`, `CALL cc,nn`, `RET`, `RET cc`),
RST 00h..38h, IRQ accept v IM 0/1/2 a RETI/RETN unwind. NMI accept je
v API contractu připraven, ale v MZ-800/700/1500 v hot-path není zdroj
NMI signálu.


## 2. Aktivace subsystému

Callstack je **vypnutý** při startu emulátoru (zero hot-path overhead -
Z80 CALL/RET hooky drží NULL). Zapnout lze:

| Cesta | Co se stane |
|---|---|
| **INI klíč** `[CALLSTACK] active=1` | Subsystém aktivován při startu, persistuje |
| **CLI flag** `--callstack` | Force ON nad INI hodnotu při startu |
| **UI Active checkbox** v toolbaru | Hot-toggle za běhu emulátoru |

UI toggle `Active`:

- **při ON**: zaregistruje Z80 CALL/RET hooky a vyresetuje shadow + stats
  (= čistý start)
- **při OFF**: deregistruje hooky a ponechá shadow pro poslední zobrazení
  (= je možno si prohlédnout poslední stav)

Race-window při toggle za chodu emu loopu je minimální (pointer
assignment je atomic na x86_64). V nejhorším případě se ztratí 1 frame
při přechodu; žádný crash.


## 3. Otevření / zavření

- **Menu:** Debugger -> Callstack (toggle viditelnost)
- **DBG Workplace:** flag `wp_callstack` (default OFF) - pokud je
  zapnuto, okno se otevírá s hlavním debugger oknem
- **Default visibility:** zavřené při startu. Po otevření se
  pozice/velikost pamatuje přes `imgui.ini`

Subsystém **běží i pokud je okno zavřené** (= shadow se aktualizuje
průběžně); okno je read-only pohled.


## 4. Layout okna

```
+--- Callstack ---------------------------------------------------+
| [x] Active   [Reset]   [ ] Hide IRQ   [ ] Hide Synth            |
+-----------------------------------------------------------------+
| #  | Ret    | Call   | Target | Sym             | Kind | Cyc-in |
| 0  | 1234h  | 1231h  | 4000h  | sound_play      | CALL | 245    |
| 1  | 2345h  | 2342h  | 5000h  | render_sprite   | CALL | 1248   |
| 2  | 9000h  | 4321h  | 9000h  | ISR_VBL         | IRQ2 | 8194   |
| 3  | 0028h  | 1003h  | 0028h  | KEY_HANDLER     | RST  | 12388  |
| 4  | 0000h  | 0000h  | 0000h  | (no sym)        | SYNT | 25102  |
+-----------------------------------------------------------------+
| Depth: 5 / max=12 | Diverg: 1  SP-swap: 0  Overflow: 0          |
+-----------------------------------------------------------------+
```

### 4.1 Toolbar

| Prvek | Význam |
|---|---|
| **Active** checkbox | Toggle subsystému (viz sekce 2). Při ON zaregistruje hooky + reset shadow. Při OFF deregistruje + ponechá poslední snapshot. |
| **Reset** tlačítko | Vyprázdní shadow + nulluje všechny countery (= jako emu reset, ale jen pro callstack). UI tabulka se vyčistí ihned. |
| **Hide IRQ** | Skryje řádky typu IRQ_IM0/IM1/IM2 a NMI (= UI filtr; subsystém je dál sleduje). |
| **Hide Synth** | Skryje řádky typu SYNTHETIC (divergent emit). Counter `Diverg` zůstává viditelný. |

Filtry persistují přes restart emu (cfg sekce `[CALLSTACK_PANEL]`,
klíče `hide_irq`, `hide_synth`).

### 4.2 Tabulka

**Pořadí řádků = konvence GDB / Visual Studio**:

- **#0 (horní řádek) = TOP** = **nejmladší** frame = **funkce, ve které
  CPU právě stojí**
- vyšší `#` = **starší** = volající směrem k root entry (typicky ROM
  monitor / IRQ vector)
- "Cesta dolů v tabulce" = "cesta nahoru zpět k volajícímu"

Sloupce:

| Sloupec | Význam |
|---|---|
| **#** | Depth index. #0 = TOP (aktuální), #N = nejhlubší (nejstarší) frame. |
| **Ret** | Návratová adresa - **kam se vrátí RET** z tohoto frame. Pro CALL = `call_site + 3`, pro RST = `call_site + 1`, pro IRQ/NMI = PC v momentě IRQ accept (= adresa za přerušenou instrukcí). |
| **Call** | Adresa CALL/RST opcode - **kde** v paměti byla instrukce, která tento frame vytvořila. Pro IRQ frame má stejnou hodnotu jako Ret (= IRQ accept proběhl **mezi** instrukcemi). |
| **Target** | Cílová adresa volání = **entry rutiny** kam se skočilo. Pro RST `(opcode - 0xC7) / 8 * 8` (= 0x00, 0x08, 0x10, ... 0x38), pro NMI vždy `0066h`. Pro IM 2 IRQ = adresa načtená z `(I<<8) | vector`. |
| **Sym** | Symbol resolvovaný proti `target`. Pokud bez symbolu, `(no sym)`. |
| **Kind** | Typ frame (viz tabulka níže). Divergent řádky zobrazí Kind žlutě. |
| **Cyc-in** | Reálné T-states uvnitř framu = `cycles_now - cycles_at_entry`. Nejmladší frame (#0) má nejmenší hodnotu, nejstarší (#N) největší (= obsahuje cykly celé volací cesty pod sebou). |

**Resizable + Reorderable + Hideable** - táhni border mezi sloupci pro
změnu šířky, drag header pro změnu pořadí, right-click na header pro
skrytí sloupce.

#### Hodnoty Kind

| Kind | Co znamená |
|---|---|
| **CALL** | Z80 `CALL nn` nebo `CALL cc,nn` - taken. |
| **RST** | Z80 `RST n` opcode, Target je `n*8` (0x00..0x38). |
| **IRQ0** | IM 0 IRQ accept - bus latch byte interpretován jako RST 38h v MZ-800. |
| **IRQ1** | IM 1 IRQ accept - hardware fixní call na 0x0038. Default MZ-800 ROM ISR. |
| **IRQ2** | IM 2 IRQ accept - call na adresu načtenou z `(I<<8) | vector`. Tooltip ukazuje IM2 vector. |
| **NMI** | NMI accept - call na 0x0066. V MZ HW není zdroj NMI signálu. |
| **SYNT** | Synthetic frame - emit při RET nad prázdným shadow (= divergence). Žlutá barva. |

### 4.3 Footer (statistiky)

| Counter | Co znamená |
|---|---|
| **Depth: N / max=M** | Aktuální hloubka shadow / historický maximum od posledního resetu. |
| **Diverg** | Suma `Trampoline + Longjmp + Mismatch`. Hover na hodnotu ukáže breakdown. |
| **SP-swap** | Počet ručních manipulací s SP (`LD SP,nn`, `EX (SP),HL`, ...). |
| **Overflow** | Počet zamítnutých push pokusů při překročení maximální hloubky shadow. Emu není ovlivněn, jen shadow přestane růst. |
| **Discard** | Heuristický signál stack reset events (= RET, kde SP vyletěl nad nejhlubší tracked frame, typicky warm boot / ROM reset / exception). Shadow NENÍ auto-cleared. Při sporadickém růstu (např. po ^C) stiskni Reset pro čistý start. |

#### Diverg breakdown (hover)

| Sub-counter | Co znamená |
|---|---|
| **Trampoline** | RET nad prázdným shadow nebo SP-nested trampoline (= PUSH+RET dispatch typu CP/M BDOS jump table). Shadow zůstává intact pokud SP-nested. |
| **Longjmp** | RET pop přes více framů naráz (= match deep v shadow přes SP nebo return_addr). Pokrývá taky ISR-via-RET pattern (= ISR vrací RET místo RETI). |
| **Mismatch** | Top mismatch + žádný deep match (= self-modifying RET adresa nebo neznámý pattern). Conservative pop top. |

Reset všech counterů: button `Reset` v toolbaru (= zároveň vyprázdní
shadow). Countery se taky resetují při emu reset (F5 / snapshot load).


## 5. Akce nad řádkem

### Levý klik / Shift+klik / Double-click

| Akce | Co dělá |
|---|---|
| **LMB klik** | Fokus hlavního Disassembly (#1) na `Call` adresu (= **kde se zavolala** funkce). |
| **Shift+LMB** | Fokus hlavního Disassembly na `Target` adresu (= **entry** funkce). |
| **Double-click** | Otevři/fokus sekundární Disassembly (#2) na `Call`. |
| **Hover** | Tooltip s shortcut hintem a detailem entry (Ret/Call/Target/SP/Kind, IM2 vector, DIVERGENT warning). |

### Right-click context menu

Plné menu napříč debuggerem:

- Focus disasm at Call / Target (slot #1)
- Open Disasm #2 at Call / Target (slot #2)
- Add bookmark at Call / Target (s automatickou symbol resolution do názvu)
- Set BP at Call / Target
- Copy hex address Call / Target / Return (do clipboardu)


## 6. Worked example - jak číst tabulku

Příklad ze sekce 4 layout:

```
#  | Ret    | Call   | Target | Sym             | Kind | Cyc-in
0  | 1234h  | 1231h  | 4000h  | sound_play      | CALL | 245
1  | 2345h  | 2342h  | 5000h  | render_sprite   | CALL | 1248
2  | 9000h  | 4321h  | 9000h  | ISR_VBL         | IRQ2 | 8194
3  | 0028h  | 1003h  | 0028h  | KEY_HANDLER     | RST  | 12388
4  | 0000h  | 0000h  | 0000h  | (no sym)        | SYNT | 25102
```

Čteme **od shora dolů = od aktuálního stavu k nejstaršímu volajícímu**:

- **Řádek #0**: CPU je právě uvnitř funkce `sound_play` (entry 0x4000).
  Funkce byla volána CALL instrukcí na adrese 0x1231; po RET se vrátí
  na 0x1234. Strávil v ní 245 T-states.
- **Řádek #1**: Volající funkce `sound_play` byla rutina `render_sprite`
  (entry 0x5000). Ona sama byla volána CALL na 0x2342; po jejím RET se
  CPU vrátí na 0x2345. V `render_sprite` jsme strávili dosud 1248 T-states
  (= rozdíl 1248 - 245 = 1003 T-states je čas v `render_sprite` **mimo**
  `sound_play`).
- **Řádek #2**: `render_sprite` byla volaná z IRQ2 ISR `ISR_VBL` (entry
  0x9000). IM 2 IRQ accept proběhl mezi instrukcemi - PC byl 0x4321,
  na adrese 0x9000 je entry ISR. Když ISR udělá RETI, vrátí se zpět
  na 0x9000... wait, **Ret a Target jsou stejné** - znamená to, že
  v okamžiku IRQ accept byl PC = 0x9000 (= IRQ přerušil normální výpočet
  právě před instrukcí na 0x9000). Cyc-in = 8194 = celkový čas v IRQ
  handleru + jeho voláních.
- **Řádek #3**: RST 28h volaný z 0x1003 - `KEY_HANDLER` rutina (entry
  0x28). Tj. před IRQ_VBL byl program v RST 28h handleru. Cyc-in =
  12388 T-states.
- **Řádek #4**: SYNTHETIC frame (žlutý Kind) - vznikl při divergenci.
  Pravděpodobně byl shadow prázdný (= aktivace callstacku proběhla
  uprostřed výpočtu) a první RET vytvořil tento marker. Cyc-in = 25102
  = od push tohoto frame uplynulo 25102 T-states.

**Pozn. k IRQ entry zanoření**: RETI z #2 popne ten frame a CPU se
vrátí na 0x9000... ale `render_sprite` (řádek #1) je hlubší než ISR_VBL.
Je to **správné** zanoření: `KEY_HANDLER` (#3) byl volán RST 28 -> dělá
svou práci -> IRQ2 ho přeruší -> IRQ_VBL volá `render_sprite` ->
`render_sprite` volá `sound_play` -> CPU je v `sound_play`. RETI ukončí
jen IRQ frame (= #2), pak CPU pokračuje v `KEY_HANDLER` od svého RET
na 0x0028h. Mezitím `render_sprite` a `sound_play` byly "hosté" uvnitř
ISR a skončí svými RETs **před** RETI.

### Hloubka vnoření vs zobrazení Cyc-in

Cyc-in roste se vzdáleností od TOP (#0 je nejmladší = nejméně cyklů).
Není to "exclusive time uvnitř té funkce" - je to **total elapsed od
push framu do teď**.


## 7. SYNTHETIC frame - kdy a proč

SYNTHETIC frame vznikne při **divergenci** - tj. když RET (nebo
RETI/RETN) nedokáže najít odpovídající CALL/IRQ entry v shadow stacku.

### Typické příčiny

1. **PUSH+RET trampolína** (jump trick):
   ```
       LD HL, target
       PUSH HL
       RET             ; ve skutečnosti JP target
   ```
   Shadow stack tuto sekvenci nevidí jako CALL (PUSH není zachycen).
   Když přijde RET, top shadow neobsahuje match -> emit SYNTHETIC frame,
   Diverg counter +1.

2. **Manual call frame**:
   ```
       PUSH return_to
       PUSH func_ptr
       RET             ; nepřímý CALL přes RET
   ```
   Stejný princip.

3. **POP+JP HL** (rutina si vezme svou return adresu jako data):
   ```
   sub:
       POP HL          ; HL = return address
       INC HL          ; přeskoč 1 byte za CALL (= immediate data inline)
       JP HL
   ```
   Shadow stack zůstane konzistentní (CALL si je registroval), ale
   následující "RET-like" chování zachycené není.

4. **Longjmp pattern** - RET na hluboko v shadow nalezenou entry: pop N
   frames naráz, Diverg++ (jednou). Bez SYNTHETIC.

5. **Aktivace uprostřed běhu**: pokud zapneš Active toggle a CPU je
   uvnitř volání, shadow je prázdný a první RET vyemituje SYNTHETIC.
   Toto je **očekávané** chování a po pár sekundách se shadow
   re-stabilizuje (Diverg counter ale ukáže tyto inicialní eventy).

### Co s SYNTHETIC dělat

- Vidíš ho jako žlutý `SYNT` Kind v tabulce
- Hover ukáže "DIVERGENT (push+ret trampoline or longjmp)"
- Lze ho filtrovat přes Hide Synth (ale Diverg counter zůstane)
- Není to chyba emulátoru, je to označení neperfect heuristiky callstacku


## 8. Limitace a vhodné použití

| Limitace | Detail |
|---|---|
| `LD SP, ...` detekce | Vyžaduje opcode classification; SP-swap counter zatím vždy 0. |
| NMI accept push | API contract připraven, ale MZ-800/700/1500 v hot-path nemá NMI source. Pokud by NMI proběhl, push chybí; unwind přes RETN auto-vyrovná divergence handlingem. |
| Multi-listener | Listener slot je jen jeden. Více konzumentů = fan-out ve vrstvě nad. |
| `total_cycles` wraparound | Po wraparoundu 32-bit cycle counteru (~1224 s při 3.5 MHz) vrátí Cyc-in nesmyslnou hodnotu. |
| Pre-aktivační IRQ frames | Pokud zapneš Active toggle uprostřed IRQ handleru, shadow je prázdný; RETI/RETN vyemituje SYNTHETIC + Diverg. Po stabilizaci stiskni Reset. |

### Vhodné použití a omezení pro OS-like programy

Single-shadow přístup sleduje **jeden globální** call stack. Pro typický
single-stack program (= hra, demo, monolitická utilita, ROM analýza,
vlastní Z80 kód s konzistentní stack discipline včetně běžných ISR)
heuristika funguje výborně:

- **CALL/RET páry** match čistě (= Depth oscilluje rozumně, Diverg ~ 0)
- **IRQ accept + RETI/RETN** unwind správně (= ISR frame push/pop bez
  divergence)
- **ISR-via-RET** (= ISR vrací `RET` místo `RETI`) zachyceno přes
  longjmp branch (= return_addr match na IRQ frame). Žádný false discard.
- **PUSH+RET trampolína** (např. tabulkový dispatch) klasifikovaná
  jako Trampoline counter, shadow zůstává intact.

### Kde heuristika naráží

| Vzor | Důsledek | Doporučení |
|---|---|---|
| **CP/M BDOS/BIOS** - každá služba má vlastní stack discipline, mění SP mezi user-stack a BDOS/BIOS stacks. | Diverg + Discard roste průběžně (typicky 1-2 / BDOS call). Shadow nedávno přesný pro user program scope. | Použij **Stack Monitor** (raw SP view) pro CP/M analýzu. |
| **Multitaskingové systémy** (NIPOS, P-CP/M80, vlastní RTOS) - per-task stack switching. | Shadow se rozpadne při task switch. | Nepoužitelné. |
| **HW exception handlery** se stack reset (= warm boot, vector restart) | Discard counter roste, shadow zůstává intact (= bez auto-clear). | Po detekci klikni Reset pro čistý start. |

### Co znamenají countery při analýze

| Counter rychlost růstu | Interpretace |
|---|---|
| **Diverg = 0** během běhu programu | Heuristika perfectly v sync s programem. Single-stack discipline. |
| **Trampoline > 0**, ostatní ~ 0 | Program používá PUSH+RET dispatch tables. OK, heuristika to chápe. |
| **Longjmp > 0**, sporadicky | Program používá longjmp/setjmp pattern nebo ISR-via-RET. OK. |
| **Mismatch roste pravidelně** | Heuristika neumí pattern programu. Buď self-modifying RET adresy, nebo OS-like multi-stack. Doporučení: porovnat s Stack Monitor. |
| **Discard > 0** sporadicky (např. po ^C) | Stack reset event (warm boot, exception). Po detekci stiskni Reset. |
| **Discard roste průběžně** | Pravděpodobně OS s multi-stack discipline (CP/M). Použij Stack Monitor. |


## 9. Vazba na ostatní subsystémy

| Subsystém | Vazba |
|---|---|
| **Symbols** | Sym lookup per render frame pro `target`. Plně závislé na načtených `.lbl` / `.map` / `.sym` souborech. |
| **Stack Monitor** | Sibling raw SP view ([stack-window.md](stack-window.md)). Callstack a Stack Monitor se nedoplňují přes API; uživatel je čte současně. |
| **Eventlog** | Callstack neemituje do eventlogu. Cesty IRQ accept / RST jdou samostatně přes eventlog. |
| **Breakpoints** | Žádná direct vazba. |
| **Disassembly** | Klik/Shift/Double-click v Callstack řádku otevírá Disasm na `Call`/`Target`. |
| **Bookmarks** | Right-click -> "Add bookmark at Call/Target" - automatický symbol resolution do názvu bookmarky. |
| **Profiler** | Konzument listener API. Z `on_enter`/`on_exit` páru spočítá inclusive/exclusive cycles per funkce. |
