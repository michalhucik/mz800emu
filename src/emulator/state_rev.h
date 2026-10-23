/**
 * @file   state_rev.h
 * @brief  Revize stavu stroje: číslo, které se mění pokaždé, když se mohl
 *         změnit stav emulovaného stroje.
 *
 * Stav stroje = CPU, paměti a jejich mapování, čipy, řadiče, vložená média,
 * ROM, HW konfigurace (SW1, PSG1, připojená zařízení). Konfigurace debuggeru
 * (breakpointy, watch, symboly, trace, freeze tabulka), výstupy pro
 * hostitele (obraz, zvuk, soubory) a volby hostitele (rychlost) do stavu
 * nepatří.
 *
 * Revize roste monotónně a nemusí být souvislá; klient smí testovat jen
 * rovnost. Falešné zvednutí je bezpečné (agentovi se odmítne podmíněný
 * zápis), chybějící zvednutí ne (agent by věřil zastaralému čtení). Platí
 * jen v pauze: za běhu se stav mění bez zvednutí, ale rozběh i zastavení
 * revizi zvednou, takže revize přečtená za běhu se nikdy nerovná revizi
 * zastaveného stavu.
 *
 * Kde se zvedá (vše studené cesty emu vlákna, nikdy instrukční smyčka ani
 * paměťové a I/O callbacky): výstup z pauzy a vstup do ní (run_control),
 * příkaz fronty dbgapi, který mění stav (tabulka vlastností příkazů),
 * reset (mzarch_main_reset()), načtení snapshotu
 * (snapshot_load_through_io()), zásah zařízení z GUI (device_request_poll()),
 * mapa MemExt z okna (memext_map_request_process()), práce záznamu vstupů
 * v pauze (replay_on_paused()), vstupy a kazeta z UI mimo záznam
 * (replay_ui_off_drain(), replay_cmt_run()). **Nová cesta, která mění stav
 * stroje mimo tato místa, musí revizi zvednout (STATE_REV_BUMP(zdroj)).**
 *
 * Vlákna: zapisuje a čte jen emu vlákno. Jiná vlákna dostávají hodnotu
 * kopií v emu vlákně (slot fronty dbgapi, události `paused` a `resumed`).
 *
 * Zdroje: každé zvednutí přidá do g_state_rev_src bit zdroje
 * (en_STATE_REV_SRC). Událost MCP `state_changed` (paused smyčka,
 * state_rev_take_unannounced()) z nich ohlásí změny v pauze, které
 * nepocházejí od MCP klienta ani z běhu.
 *
 * Modul je v buildu s debuggerem i bez něj (zvedá se i v kódu mimo
 * debugger); bez debuggeru revizi nikdo nečte.
 */

#ifndef STATE_REV_H
#define STATE_REV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Aktuální revize stavu stroje.
     *
     * @invariant > 0; po state_rev_init() v horních bitech náhodná epocha
     *            procesu, celá hodnota pod 2^53 (bezpečná v JSON čísle).
     * @note Jen emu vlákno (čtení i zápis).
     */
    extern uint64_t g_state_rev;

    /**
     * @brief Zdroj zvednutí revize (bitová maska, pro událost `state_changed`).
     */
    typedef enum en_STATE_REV_SRC
    {
        STATE_REV_SRC_RUN = 1u << 0,      /**< Rozběh a zastavení (Z1, Z2), ohlašují je `resumed` a `paused`. */
        STATE_REV_SRC_MCP = 1u << 1,      /**< Příkaz fronty dbgapi od MCP klienta, včetně zvednutí uvnitř něj (reset, snapshot). */
        STATE_REV_SRC_DEBUGGER = 1u << 2, /**< Příkaz fronty dbgapi jiného původu (okna debuggeru v GUI, test, emulátor). */
        STATE_REV_SRC_RESET = 1u << 3,    /**< Reset mimo frontu dbgapi (menu, klávesa, záznam vstupů) (Z4). */
        STATE_REV_SRC_SNAPSHOT = 1u << 4, /**< Načtení snapshotu mimo frontu dbgapi (Load, Quick Load) (Z5). */
        STATE_REV_SRC_DEVICE = 1u << 5,   /**< Zásah zařízení z GUI a mapa MemExt z okna (Z6, Z7). */
        STATE_REV_SRC_REPLAY = 1u << 6,   /**< Požadavek záznamu vstupů (start, stop, seek, goto; z GUI i MCP) (Z8). */
        STATE_REV_SRC_INPUT = 1u << 7,    /**< Vstupy z UI: virtuální klávesnice, autotype (Z8, Z9). */
        STATE_REV_SRC_TAPE = 1u << 8,     /**< Operace kazety z UI nebo záznamu vstupů (Z9). */
        STATE_REV_SRC_COUNT_BITS = 9      /**< Počet bitů zdrojů (není to zdroj). */
    } en_STATE_REV_SRC;

    /**
     * @brief Zdroje zvednutí od posledního ohlášení (bitová maska en_STATE_REV_SRC).
     * @note Jen emu vlákno.
     */
    extern uint32_t g_state_rev_src;

    /**
     * @brief Zdroj, kterým se nahradí zdroj každého zvednutí (0 = nenahrazovat).
     *
     * Fronta dbgapi ho nastaví na STATE_REV_SRC_MCP po dobu příkazu od MCP
     * klienta, takže reset nebo načtení snapshotu uvnitř se připíše MCP.
     * @note Jen emu vlákno.
     */
    extern uint32_t g_state_rev_src_override;

    /**
     * @brief Zdroj čekajícího resetu vyžádaného příkazem fronty dbgapi (0 = žádný).
     *
     * Příkaz RESET za běhu nebo při záznamu vstupů reset jen vyžádá
     * (provede ho až smyčka emulace mimo příkaz); uloží sem
     * g_state_rev_src_override, aby se reset od MCP klienta připsal MCP.
     * mzarch_main_reset() ho použije a vynuluje. V pauze bez záznamu se
     * reset provede přímo v příkazu.
     * @note Jen emu vlákno.
     */
    extern uint32_t g_state_rev_reset_src;

    /**
     * @brief Nastaví počáteční revizi s náhodnou epochou procesu.
     *
     * Epocha (1 .. 2^20) posunutá o 32 bitů: revize z jiného procesu (pipe
     * režim, restart emulátoru) se s revizí nového procesu prakticky nikdy
     * nerovná, takže podmíněný zápis se starou revizí selže.
     *
     * @pre Před startem emu vlákna (inicializace emulátoru).
     * @post g_state_rev = epocha << 32.
     */
    void state_rev_init(void);

    /**
     * @brief Označí aktuální revizi za ohlášenou a vynuluje zdroje
     *        (i zdroj čekajícího resetu g_state_rev_reset_src).
     *
     * Volá se při vstupu do pauzy (Z2): stav po běhu ohlašuje událost
     * `paused`, změny během běhu se v `state_changed` neohlašují.
     *
     * @note Emu vlákno.
     */
    void state_rev_mark_announced(void);

    /**
     * @brief Zjistí, zda se revize od posledního ohlášení změnila, a vezme zdroje.
     *
     * @param[out] rev Aktuální revize (smí být NULL).
     * @return Maska zdrojů od posledního ohlášení (0 = revize beze změny);
     *         po návratu je revize ohlášená a maska vynulovaná.
     * @note Emu vlákno.
     */
    uint32_t state_rev_take_unannounced(uint64_t *rev);

    /**
     * @brief Jméno zdroje pro výstupy (`run`, `mcp`, `debugger`, `reset`,
     *        `snapshot`, `device`, `replay`, `input`, `tape`).
     *
     * @param src Jeden bit en_STATE_REV_SRC.
     * @return Statický řetězec; neznámý bit "unknown".
     */
    const char *state_rev_src_name(uint32_t src);

    /**
     * @brief Zvedne revizi stavu stroje a zapíše zdroj.
     *
     * @param src Zdroj (en_STATE_REV_SRC); nahradí ho g_state_rev_src_override,
     *            je-li nenulový.
     * @note Emu vlákno, studená cesta (nikdy instrukční smyčka).
     */
#define STATE_REV_BUMP(src) \
    (g_state_rev++, g_state_rev_src |= (g_state_rev_src_override ? g_state_rev_src_override : (uint32_t)(src)))

#ifdef __cplusplus
}
#endif

#endif /* STATE_REV_H */
