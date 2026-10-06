/*
 * io_activity.h - Sliding window IORQ activity tracker pro UI panel.
 *
 * Per-port hits/s metrika pro vizualizaci ("heat map") v Overview tabu
 * I/O Ports panelu (V1.5 redesign D.7). Inspirováno no$gba IO Map.
 *
 * Datový model:
 *   - g_io_activity[65536] - jeden záznam per **plná 16-bit** IORQ adresa
 *     (= obsah BC při IN/OUT instrukci). 16-bit indexing umožňuje rozlišit
 *     porty jako 0xCF01..0xCF07 (Unicard sub-registry), které dříve
 *     sdílely jeden 8-bit slot 0xCF (V1.5 fix #1).
 *   - hits_per_frame_in[60] / hits_per_frame_out[60] - rotující ring buffer
 *     60 buckets per smer, 1 bucket = 1 emulační frame (~20 ms na 50Hz).
 *     Posledních 50 buckets ≈ sliding window 1s (V1.5.D fix #2 -
 *     rozdelene per smer, drive jediny counter).
 *   - current_bucket - rotace přes io_activity_advance_frame() na vsync,
 *     sdileny pro IN i OUT (= obe strany rotuji synchronne).
 *   - total_hits_in / total_hits_out - kumulativně od reset, per smer.
 *
 * Velikost statické alokace: sizeof(st_IO_PORT_ACTIVITY) ≈ 280 B (struct
 * pack + alignment), 65536 entries → ~18 MB BSS. Akceptovatelné (= jednorázová
 * alokace, neplatí se RAM když panel zavřený a tracking flag vypnutý;
 * advance_frame iteruje všech 65536 entries jen při zapnutém trackingu).
 *
 * Hot-path hook v port_*_with_logging_cb (mz800_iorq.c, mz1500_iorq.c)
 * je gated přes flag g_io_window_tracking_active. Default OFF (panel
 * zavrený) = jeden flag check + branch predictor (= zero overhead).
 * Při otevreném panelu hook udělá pole indexaci + atomic-free inkrement.
 *
 * Threading: hot-path běží v emulačním vláknu, UI render z UI vlákna.
 * Ring je single-writer / single-reader, bez locků - krátký data race
 * při čtení (nesourodý sliding window value) je akceptovatelný (= UI
 * jen zobrazí číslo). Zapisuje do tabulky jen emu vlákno, včetně resetů
 * (io_activity_reset_*): UI je posílá přes frontu dbgapi
 * (DBGAPI_CMD_IO_ACTIVITY_RESET, DBGAPI_CMD_IO_ACTIVITY_RESET_PORT).
 *
 * Licence: GPLv3
 */

#ifndef IO_ACTIVITY_H
#define IO_ACTIVITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Počet rotujících bucketů pro sliding window.
 *
 * Hodnota 60 = 60 emulačních frames. Při 50Hz frame rate to odpovídá
 * 1.2s historii. Sliding window 50 buckets ≈ 1s window.
 */
#define IO_ACTIVITY_BUCKET_COUNT 60

/**
 * @brief Velikost sliding window pro hits/s výpočet.
 *
 * 50 frames @ 50Hz = 1s. Přesnější při menší jitter v advance_frame.
 */
#define IO_ACTIVITY_WINDOW_BUCKETS 50


/**
 * @brief Per-port aktivity záznam.
 *
 * V1.5.D fix #2: counters jsou rozdelene per smer (IN vs OUT). Drive
 * sdileli jediny counter, takze napriklad u portu 0xF0 (W = GDG Palette,
 * R = JOY0) Overview tab zobrazoval aktivitu i pro JOY0 R radek, kdyz
 * jenom write probihal na palette.
 *
 * @field hits_per_frame_in   Ring buffer 60 buckets pro IN (CPU read) hits.
 * @field hits_per_frame_out  Ring buffer 60 buckets pro OUT (CPU write) hits.
 * @field current_bucket      Index aktualne aktivniho bucketu (0..59).
 *                            Sdileny pro IN i OUT (= synchronni rotace).
 *                            Inkrementuje io_activity_advance_frame() modulo 60.
 * @field total_hits_in       Kumulativni IN hits od posledniho reset.
 * @field total_hits_out      Kumulativni OUT hits od posledniho reset.
 * @field last_write_value    Posledni IORQ write hodnota pro tento port. Validni
 *                            pokud last_write_frame > 0.
 * @field last_write_frame    Frame number (= screens) kdy byl write zaznamenan.
 *                            0 = neni cache.
 * @field last_read_value     Posledni IORQ read hodnota pro tento port. Validni
 *                            pokud last_read_frame > 0.
 * @field last_read_frame     Frame kdy byl read zaznamenan. 0 = neni cache.
 *
 * Pozn.: cache je distinct per smer (= write a read jsou ulozeny zvlast),
 * protoze nektere porty maji rozdilnou semantiku R/W (napr. 0xCE OUT=DMD
 * vs IN=Status). UI Overview pri R/W direction zobrazi prislusnou cached
 * hodnotu (= fix #10). Uvodni stav nuly = "no data captured".
 */
typedef struct st_IO_PORT_ACTIVITY
{
    uint16_t hits_per_frame_in[ IO_ACTIVITY_BUCKET_COUNT ];
    uint16_t hits_per_frame_out[ IO_ACTIVITY_BUCKET_COUNT ];
    uint8_t  current_bucket;
    uint64_t total_hits_in;
    uint64_t total_hits_out;
    uint8_t  last_write_value;
    uint32_t last_write_frame;
    uint8_t  last_read_value;
    uint32_t last_read_frame;
} st_IO_PORT_ACTIVITY;


/**
 * @brief Velikost activity tabulky - plný 16-bit prostor IORQ adres.
 *
 * 65536 entries pokryje všechny možné kombinace BC při IN/OUT.
 * Statická BSS alokace ~18 MB (= sizeof(st_IO_PORT_ACTIVITY) * 65536)
 * po V1.5.D fix #2 (drive ~9 MB s jednim counter setem).
 */
#define IO_ACTIVITY_TABLE_SIZE 65536u


/**
 * @brief Globální tabulka aktivit per **plná 16-bit IORQ adresa**.
 *
 * Indexováno celým 16-bit BC (= addr v port_read_cb / port_write_cb hooks).
 * Distinct counter pro každou 16-bit kombinaci - např. 0xCF01 a 0xCF06
 * (Unicard sub-registry) mají oddělené counters (V1.5 fix #1).
 *
 * Pro 8-bit porty (= 0x00..0xFF v IO katalog), low byte je primární klíč;
 * UI Overview lookup používá `port->addr` jako přímý index. High byte se
 * při IN/OUT typicky přebírá z B registru (Z80 IN A,(C) / OUT (C),A).
 */
extern st_IO_PORT_ACTIVITY g_io_activity[ IO_ACTIVITY_TABLE_SIZE ];


/**
 * @brief UI tracking flag - default 0 (= zero overhead).
 *
 * Lifecycle:
 *  - UI vrstva (io_window) sets na 1 při otevření panelu + tracking
 *    enabled (= user opt-in).
 *  - UI sets na 0 při zavření panelu nebo při vypnutí tracking.
 *
 * Hot-path hook v port_*_with_logging_cb dělá `if (g_io_window_tracking_active)`
 * - default 0 = branch predictor friendly skip.
 *
 * Threading: read v emu vláknu, write v UI vláknu - akceptovatelný race
 * (nejhorší dopad = jeden frame zpoždění zapnutí/vypnutí trackingu).
 */
extern uint8_t g_io_window_tracking_active;


/**
 * @brief Inicializace activity tracking - vynuluje g_io_activity.
 *
 * Volat při startu emulátoru (jednou). Idempotentní.
 */
void io_activity_init ( void );


/**
 * @brief Reset activity counters pro všechny porty.
 *
 * Vynuluje celou tabulku g_io_activity (všech IO_ACTIVITY_TABLE_SIZE slotů:
 * hits_per_frame_in/out[], current_bucket, total_hits_in/out, last value).
 *
 * @pre Volat jen z emu vlákna (nebo když emu vlákno neběží), jinak souběh
 *      s io_activity_record_hit / io_activity_advance_frame. UI tlačítko
 *      "Reset Activity" posílá DBGAPI_CMD_IO_ACTIVITY_RESET.
 */
void io_activity_reset_all ( void );


/**
 * @brief Reset activity counter pro jeden slot tabulky.
 *
 * Vynuluje celý slot g_io_activity[port] (čítače, ring, last value).
 *
 * @param port  Klíčová 16-bit adresa slotu (bus adresa IORQ 0x0000..0xFFFF,
 *              resp. MMIO adresa 0E000h..0E008h u MZ-700 MMIO).
 *
 * @pre Volat jen z emu vlákna (nebo když emu vlákno neběží), jinak souběh
 *      s io_activity_record_hit / io_activity_advance_frame. Kontextové
 *      menu okna I/O Ports ("Reset activity counter") posílá
 *      DBGAPI_CMD_IO_ACTIVITY_RESET_PORT s is_8bit = 0.
 */
void io_activity_reset_port ( uint16_t port );


/**
 * @brief Hot-path hook - zaznamenat IORQ hit + cache last value.
 *
 * V1.5.D fix #2: inkrementuje hits_per_frame_in / hits_per_frame_out
 * a total_hits_in / total_hits_out podle smeru (drive jediny counter set).
 * Aktualizuje last_{read,write}_value + frame podle smeru.
 * Volat z port_*_with_logging_cb gated přes g_io_window_tracking_active.
 *
 * @param port           Plná 16-bit IORQ adresa (= obsah BC při IN/OUT).
 *                       V1.5 fix #1: per-16-bit-port granularita
 *                       (např. 0xCF01 vs 0xCF06 mají oddělené counters).
 * @param value          IORQ value (read result nebo write data).
 * @param is_in          true = IN (CPU read), false = OUT (CPU write).
 *                       Smer rozhoduje, ktery counter set se inkrementuje.
 * @param frame          Aktualni screen number z g_gdg.total_elapsed.screens.
 *                       Frame 0 je validni kontextove (= start emu); UI
 *                       reaguje na 0 jako "neni cache" (= viz get_last_value).
 */
void io_activity_record_hit ( uint16_t port, uint8_t value,
                               bool is_in, uint32_t frame );


/**
 * @brief Vrati posledni cached value pro port + smer (fix #10).
 *
 * Pokud port nikdy nemel zaznam (= last_*_frame == 0 z init), vraci false.
 * Jinak vraci value + age (= aktualni_frame - last_frame). UI pouziva
 * age pro fade-out aging cached display (typicky >500 frames = stale).
 *
 * @param port             Plná 16-bit IORQ adresa.
 * @param is_in            true = read cache, false = write cache.
 * @param current_frame    Aktualni screen number pro vypocet age.
 * @param out_value        Pointer kde se ulozi cached value (povinne).
 * @param out_age_frames   Pointer kde se ulozi age v frames (povinne).
 * @return true pokud existuje cached value, false pokud port nemel zaznam.
 */
bool io_activity_get_last_value ( uint16_t port, bool is_in,
                                   uint32_t current_frame,
                                   uint8_t *out_value,
                                   uint32_t *out_age_frames );


/**
 * @brief Vrátí hits/s pro daný port a smer (sliding window cca 1s).
 *
 * V1.5.D fix #2: counter je per smer. Volajici musi specifikovat,
 * jestli chce IN nebo OUT counter. Pro UI radek s direction R/RW
 * se obvykle pouziva is_in=true; pro W radek is_in=false.
 *
 * Sečte posledních IO_ACTIVITY_WINDOW_BUCKETS bucketů (= ~1s @ 50Hz).
 * UI volá per render frame v Activity sloupci.
 *
 * @param port   Plná 16-bit IORQ adresa.
 * @param is_in  true = IN counter, false = OUT counter.
 * @return Hits/s odhad (nepřesný - ring nemusí mít celý window plný).
 */
uint32_t io_activity_get_hits_per_sec ( uint16_t port, bool is_in );


/**
 * @brief Aggregate hits/s pro 8-bit port přes všech 256 high-byte hodnot.
 *
 * V1.5.D fix #2: per smer (is_in) - drive jediny counter.
 *
 * Z80 IORQ má 16-bit adresu (= obsah BC). U IN A,(N) je high byte = A
 * (nepředvídatelné) a u IN A,(C) je high byte = B (programátor volí). Pro
 * 8-bit porty v IO katalogu (= dispatch na low byte) UI součtem získá
 * skutečnou aktivitu napříč všemi high-byte variantami.
 *
 * Volá se per render frame z Overview tabu pro porty s catalog addr <= 0xFF.
 * Pro 16-bit porty (např. 0xCF01..0xCF07) UI volá io_activity_get_hits_per_sec
 * přímo (= jeden konkrétní 16-bit slot).
 *
 * @param low_byte  Low byte port adresy (0x00..0xFF).
 * @param is_in     true = IN counter, false = OUT counter.
 * @return Součet hits/s napříč 256 high-byte slotů pro dany smer.
 */
uint32_t io_activity_get_hits_per_sec_8bit ( uint8_t low_byte, bool is_in );


/**
 * @brief Aggregate last value pro 8-bit port přes všech 256 high-byte hodnot.
 *
 * Vrací nejmladší (= nejvyšší age) cached value napříč všemi high-byte
 * variantami pro daný low byte. Použito UI Overview pro 8-bit porty.
 *
 * @param low_byte         Low byte port adresy.
 * @param is_in            true = read cache, false = write cache.
 * @param current_frame    Aktualni screen number pro vypocet age.
 * @param out_value        Pointer kde se ulozi cached value (povinne).
 * @param out_age_frames   Pointer kde se ulozi age v frames (povinne).
 * @return true pokud existuje aspoň jedna cached value, false jinak.
 */
bool io_activity_get_last_value_8bit ( uint8_t low_byte, bool is_in,
                                        uint32_t current_frame,
                                        uint8_t *out_value,
                                        uint32_t *out_age_frames );


/**
 * @brief Reset activity counters pro 8-bit port přes všech 256 high-byte slotů.
 *
 * Vynuluje sloty g_io_activity[(hb << 8) | low_byte] pro hb = 0..255.
 *
 * @param low_byte  Low byte port adresy.
 *
 * @pre Volat jen z emu vlákna (nebo když emu vlákno neběží), jinak souběh
 *      s io_activity_record_hit / io_activity_advance_frame. Kontextové
 *      menu okna I/O Ports ("Reset activity counter" u 8-bit portu) posílá
 *      DBGAPI_CMD_IO_ACTIVITY_RESET_PORT s is_8bit = 1.
 */
void io_activity_reset_port_8bit ( uint8_t low_byte );


/**
 * @brief Posun ringu o jeden frame - volat na vsync.
 *
 * Inkrementuje current_bucket modulo 60 pro všechny porty a vynuluje
 * hits_per_frame_in[new_bucket] + hits_per_frame_out[new_bucket]
 * (= příprava na další frame, V1.5.D fix #2: per smer).
 * Pokud g_io_window_tracking_active == 0, neudělá nic (= žádný overhead
 * při zavřeném panelu).
 *
 * Volat z gdg vsync handler (= konec frame).
 */
void io_activity_advance_frame ( void );


#ifdef __cplusplus
}
#endif

#endif /* IO_ACTIVITY_H */
