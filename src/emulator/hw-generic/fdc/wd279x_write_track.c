/**
 * @file wd279x_write_track.c
 * @brief WD279x - Type III WRITE TRACK (formátování stopy).
 *
 * Vyextrahováno z wd279x.c v rámci refactoru. Sdílí state s chip
 * core přes st_WD279X struct, helper funkce a konstanty z
 * wd279x_internal.h.
 *
 * Veřejně exportované funkce (volané z wd279x.c):
 *  - do_write_track_setup() - aktivace v dispatch_command pro 0xF0..0xFF
 *  - do_write_track_byte() - per-byte processing v DATA write handler
 *
 * Interně:
 *  - write_track_flush() - flush nasbíraného Track-Info do DSK image
 *
 * License: GPLv3 - viz wd279x.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wd279x.h"
#include "wd279x_internal.h"
#include "fdc.h"
#include "libs/dsk/dsk.h"
#include "libs/generic_driver/generic_driver.h"
#include "media_writes.h"

/* ====================================================================
 * Type III - WRITE TRACK (formátování stopy)
 * ==================================================================== */

/**
 * @brief Inicializuje stav chipu po příjmu WRITE TRACK příkazu.
 *
 * Volá se v dispatch_command pro opcodes 0xF0..0xFF (Type III WRITE TRACK).
 * V praxi Sharp software používá 0xF0 (bez bitu d/h) nebo 0xF4 (s bitem h).
 *
 * Reset state machine na fázi 0 (čekání na IAM marker 0xFC v byte streamu
 * z CPU). Skutečné formátování probíhá inkrementálně v do_write_track_byte()
 * volaném z wd279x_write_byte() při zápisu do DATA registru.
 *
 * @param chip ukazatel na chip.
 * @param command true-bus hodnota příkazu (= regCOMMAND už nastaven volajícím).
 */
void do_write_track_setup(st_WD279X *chip, uint8_t command)
{
    chip->status_mode = WD279X_STATUS_MODE_TYPE_II_III;
    chip->data_counter = 0;
    chip->buffer_pos = 0;
    chip->multiblock_rw = 0;

    if (!drive_is_ready(chip))
    {
#ifdef FDC_DIAG
        fprintf(stderr, "fdc: WRITE TRACK setup REJECT - drive not ready "
                        "(cmd=0x%02X drive=%u track=%u side=%u)\n",
                command, chip->MOTOR & 0x03, chip->regTRACK, chip->SIDE & 0x01);
#else
        (void)command;
#endif
        chip->regSTATUS = WDST_NOT_READY;
        chip->intrq_active = 1;
        return;
    }

    /* R/O kontrola - WRITE TRACK na write-protected drive odmítneme.
     * Datasheet má specifický WPRT bit (S6) v Type II/III statusu;
     * pro jednoduchost vrátíme NOT_READY. */
    struct st_FDDrive *drv = current_drive(chip);
    if (!drv || drv->readonly)
    {
#ifdef FDC_DIAG
        fprintf(stderr, "fdc: WRITE TRACK setup REJECT - drive R/O or NULL "
                        "(cmd=0x%02X drive=%u readonly=%d)\n",
                command, chip->MOTOR & 0x03, drv ? drv->readonly : -1);
#endif
        chip->regSTATUS = WDST_NOT_READY;
        chip->intrq_active = 1;
        return;
    }

    chip->write_track_stage = 0;
    chip->write_track_counter = 0;
    chip->regSTATUS = WDST_BUSY | WDST_T2_DRQ;

#ifdef FDC_DIAG
    fprintf(stderr, "fdc: WRITE TRACK setup OK - cmd=0x%02X drive=%u "
                    "track=%u side=%u\n",
            command, chip->MOTOR & 0x03, chip->regTRACK, chip->SIDE & 0x01);
#endif
}

/**
 * @brief Konečný flush nové stopy do DSK obrazu (fáze 4 -> 5).
 *
 * Volá se po obdržení timeoutu v stage 4 (= konec byte streamu pro stopu).
 * Sestaví Track-Info blok podle chip->buffer (kde jsou nasbíraná Sector ID
 * z fáze 2) a zapíše do DSK obrazu na správný track offset. Update tsize[]
 * v hlavičce DSK podle reálné velikosti nové stopy.
 *
 * Postup (mirror _old_ wd279x_old_do_write_track stage 4 finish, ř. 968-1138):
 *  1. Vypočítat track_offset (přes dsk_compute_track_offset + image_info)
 *  2. Pokud potřeba, narůst paměťový buffer DSK (generic_driver_truncate)
 *  3. Zapsat "Track-Info\r\n\0\0\0\0" (16 bytes)
 *  4. Zapsat Track-Info header (8 bytes: track, side, ?, ?, ssize, sectors, gap, filler)
 *  5. Pro každý sektor (max 29) zapsat 8B sector-info (real + zero-padding)
 *  6. Zapsat fyzická sektor data (filled fill_byte) - počet bytes = sectors × sector_size
 *  7. Update tsize byte v DSK hlavičce na offset 0x34+track*2+side
 *  8. Update track count na offset 0x30 (= max(track) + 1 nebo + side)
 *
 * Velikost sektoru se bere z kódu N v ID poli (128 << N, N = 0..3), stejně
 * jako ji při čtení dekóduje libs/dsk. Každý sektor dostane v Sector Info
 * i skutečnou délku dat, kterou formát Extended CPC DSK vyžaduje.
 *
 * @param chip ukazatel na chip.
 * @param drv mechanika (musí být ready a not readonly).
 *
 * @return 1 = OK, 0 = chyba: stopu nelze do DSK uložit (žádný sektor, víc
 *         než DSK_MAX_SECTORS sektorů, kód N mimo 0..3, stopa delší než
 *         255 bloků po 256 B) nebo selhal zápis do obrazu. Při chybě
 *         zápisu může být obraz změněný jen zčásti.
 */
static int write_track_flush(st_WD279X *chip, struct st_FDDrive *drv)
{
    /* Hodnoty z chip->buffer naplněné v fázi 1-3:
     *   buffer[0] = TRACK (z chip->regTRACK při ID marker)
     *   buffer[1] = SIDE
     *   buffer[2] = (rezerva)
     *   buffer[3] = (rezerva)
     *   buffer[4] = kód velikosti sektoru N z ID pole (0=128, 1=256, 2=512, 3=1024 B)
     *   buffer[5] = sectors count
     *   buffer[6] = GAP3 length = 0x4E (default)
     *   buffer[7] = filler = 0xE5 (default)
     *   buffer[8..] = sector ID list (sectors entries)
     *   buffer[BUFSZ-1] = fill pattern z prvního sektoru data
     */
    uint8_t side = chip->buffer[1] & 0x01;
    uint8_t track = chip->buffer[0];
    uint8_t sectors = chip->buffer[5];
    uint8_t ssize_code = chip->buffer[4]; /* kód N z ID pole */
    uint8_t fill_byte = chip->buffer[WD279X_BUFFER_SIZE - 1];

    if (sectors == 0 || sectors > DSK_MAX_SECTORS || ssize_code > DSK_SECTOR_SIZE_1024)
    {
#ifdef FDC_DIAG
        fprintf(stderr, "fdc: WRITE TRACK flush ABORT - sectors=%u ssize_code=%u "
                        "(track=%u side=%u)\n",
                sectors, ssize_code, track, side);
#endif
        /* Bez sektorů, s více sektory, než pojme hlavička stopy DSK,
         * nebo s kódem velikosti mimo 0..3 - stopu nelze do DSK uložit. */
        return 0;
    }

    /* Velikost sektoru v bajtech podle kódu N (128 << N), stejně jako ji
     * při čtení dekóduje libs/dsk. */
    uint16_t sec_bytes = dsk_decode_sector_size((en_DSK_SECTOR_SIZE)ssize_code);

#ifdef FDC_DIAG
    fprintf(stderr, "fdc: WRITE TRACK flush START - track=%u side=%u "
                    "sectors=%u ssize_code=%u fill=0x%02X\n",
            track, side, sectors, ssize_code, fill_byte);
#endif

    /* Image info kvůli tsize array. Reload z disku - tsize se může změnit
     * od mountu (např. předchozí FORMAT track v této session). */
    st_DSK_SHORT_IMAGE_INFO image_info;
    if (EXIT_SUCCESS != dsk_read_short_image_info(&drv->handler, &image_info))
    {
        return 0;
    }

    /* Pokud formátujeme side=1, vynuť sides=2 v DSK hlavičce (offset 0x31)
     * PŘED výpočtem abstrack. Jinak by abstrack pro track=N side=1
     * kolidoval s abstrack pro track=N+1 side=0 (oba by spadly na
     * stejnou pozici tsize array při sides=1).
     *
     * libs/dsk parser interpretuje offset 0x31 takto:
     *   buffer[1] <= 1  → sides=1
     *   buffer[1] >  1  → sides=2
     * Zápis hodnoty 2 zajistí korektní 2-sided geometrii. */
    if (side == 1 && image_info.sides < 2)
    {
        uint8_t two = 2;
        if (EXIT_SUCCESS != dsk_write_on_offset(&drv->handler, 0x31, &two, 1))
        {
            return 0;
        }
        image_info.sides = 2;
    }

    /* Absolutní stopa = track × sides + side. Pro single-sided sides=1,
     * abstrack=track. */
    uint8_t sides = (image_info.sides == 0) ? 1 : image_info.sides;
    uint8_t abstrack = (uint8_t)(track * sides + side);

    uint32_t track_offset = dsk_compute_track_offset(abstrack, image_info.tsize);
    if (track_offset == 0)
    {
        /* Nová stopa za koncem - umístíme ji za poslední existující.
         * total_track_offset bude offset za poslední non-zero tsize. */
        track_offset = 0x100; /* DSK header size (1 × 256B page). */
        for (uint8_t i = 0; i < abstrack && i < DSK_MAX_TOTAL_TRACKS; i++)
        {
            track_offset += (uint32_t)image_info.tsize[i] * 0x100;
        }
    }

    /* Velikost nové stopy v 256B blocích = 1 (Track-Info block) + data
     * sektorů zaokrouhlená nahoru na celý blok (lichý počet sektorů 128 B).
     * Odpovídá dsk_encode_track_size(), ale bez ořezu na 8 bitů. */
    uint32_t sectors_bytes = (uint32_t)sectors * sec_bytes;
    uint32_t new_tsize_blocks = 1 + (sectors_bytes + 0xFF) / 0x100;
    if (new_tsize_blocks > 0xFF)
    {
        /* Nevejde se do 8-bit tsize - selhání. */
        return 0;
    }

    /* Zajisti že paměťový buffer DSK image je přesně track_offset
     * + new_tsize × 0x100 bytes. Velikost se použije pro:
     *  - růst pokud současný buffer je menší (nová stopa za koncem),
     *  - shrink pokud je větší (= odstranění zbytkových dat z původně
     *    většího DSK obrazu, např. HD formátované na SD).
     *
     * Sharp format je sekvenční (T0S0, T0S1, T1S0, ...), takže každý
     * následující flush má větší track_offset než předchozí - shrink po
     * každém kroku konverguje k velikosti odpovídající nejnovějšímu
     * formátovanému tracku. Po dokončení formátu = velikost odpovídá
     * skutečné formátované geometrii. */
    uint32_t need_size = track_offset + new_tsize_blocks * 0x100;
    {
        /* Pro memory handler můžeme check vynechat pokud size sedí.
         * Pro file handler vždy zavolej truncate - file_driver_truncate
         * je idempotentní vůči stejné velikosti (interně to detekuje). */
        int needs_truncate = 1;
        if (drv->handler.type == HANDLER_TYPE_MEMORY
            && need_size == drv->handler.spec.memspec.size)
        {
            needs_truncate = 0;
        }
        if (needs_truncate
            && EXIT_SUCCESS != generic_driver_truncate(&drv->handler, need_size))
        {
            return 0;
        }
    }

    /* 1. "Track-Info\r\n\0\0\0\0" 16 bytes magic. */
    static const uint8_t track_info_magic[16] = {
        'T', 'r', 'a', 'c', 'k', '-', 'I', 'n', 'f', 'o',
        0x0D, 0x0A, 0x00, 0x00, 0x00, 0x00
    };
    if (EXIT_SUCCESS != dsk_write_on_offset(&drv->handler, track_offset, track_info_magic, 16))
    {
        return 0;
    }

    /* 2. Track-Info header (8 bytes) na offset track_offset + 16.
     *   [0] track number
     *   [1] side
     *   [2..3] unused / data rate / recording mode
     *   [4] kód velikosti sektoru N (0=128, 1=256, 2=512, 3=1024 B),
     *       tedy bajt z ID pole beze změny
     *   [5] number of sectors
     *   [6] GAP#3 length
     *   [7] filler byte
     */
    uint8_t tinfo_header[8];
    tinfo_header[0] = track;
    tinfo_header[1] = side;
    tinfo_header[2] = 0x00;
    tinfo_header[3] = 0x00;
    tinfo_header[4] = ssize_code;
    tinfo_header[5] = sectors;
    tinfo_header[6] = chip->buffer[6]; /* GAP3 = 0x4E */
    tinfo_header[7] = chip->buffer[7]; /* filler = 0xE5 */
    if (EXIT_SUCCESS != dsk_write_on_offset(&drv->handler, track_offset + 16, tinfo_header, 8))
    {
        return 0;
    }

    /* 3. 29 × Sector-Info (8 bytes each) na track_offset + 0x18.
     *   Per sektor (entries 0..sectors-1):
     *     [0] track
     *     [1] side
     *     [2] sector ID (z chip->buffer[8 + i])
     *     [3] kód velikosti sektoru N
     *     [4] FDC status reg 1 = 0
     *     [5] FDC status reg 2 = 0
     *     [6..7] skutečná délka dat sektoru v bajtech (little-endian, 128 << N);
     *            Extended CPC DSK ji vyžaduje
     *   Zbylé entries (sectors..28) vyplníme nulami.
     */
    for (int i = 0; i < 29; i++)
    {
        uint8_t sinfo[8] = {0};
        if (i < sectors)
        {
            sinfo[0] = track;
            sinfo[1] = side;
            sinfo[2] = chip->buffer[8 + i];
            sinfo[3] = ssize_code;
            sinfo[4] = 0x00;
            sinfo[5] = 0x00;
            sinfo[6] = (uint8_t)(sec_bytes & 0xFF);
            sinfo[7] = (uint8_t)((sec_bytes >> 8) & 0xFF);
        }
        uint32_t soff = track_offset + 0x18 + (uint32_t)i * 8;
        if (EXIT_SUCCESS != dsk_write_on_offset(&drv->handler, soff, sinfo, 8))
        {
            return 0;
        }
    }

    /* 4. Fyzická data sektorů na offset track_offset + 0x100. Vyplníme
     *    fill_byte celou datovou část stopy, tedy i případné zarovnání
     *    na celý 256B blok za posledním sektorem. */
    uint32_t data_offset = track_offset + 0x100;
    uint32_t data_total = (new_tsize_blocks - 1) * 0x100;
    uint8_t fillbuf[0x100];
    memset(fillbuf, fill_byte, sizeof(fillbuf));
    while (data_total > 0)
    {
        uint16_t chunk = (data_total >= sizeof(fillbuf)) ? (uint16_t)sizeof(fillbuf) : (uint16_t)data_total;
        if (EXIT_SUCCESS != dsk_write_on_offset(&drv->handler, data_offset, fillbuf, chunk))
        {
            return 0;
        }
        data_offset += chunk;
        data_total -= chunk;
    }

    /* 5. Update tsize[] v DSK hlavičce (offset 0x34+abstrack). Symetrie
     *    s _old_ ř. 1113-1116. */
    uint32_t tsize_offset = 0x34 + abstrack;
    uint8_t tsize_val = (uint8_t)new_tsize_blocks;
    if (EXIT_SUCCESS != dsk_write_on_offset(&drv->handler, tsize_offset, &tsize_val, 1))
    {
        return 0;
    }

    /* 6. Update track count na offset 0x30 - musí být ALESPOŇ (track+1),
     *    jinak libs/dsk parser čte tsize array pouze pro `tracks*sides`
     *    bajtů (viz dsk_read_short_image_info ř. 142,152). Při tracks=0
     *    pak vrátí prázdné tsize → dsk_compute_track_offset() vždy
     *    0x100 → další flush přepíše start souboru.
     *
     *    _old_ používá `regTRACK + (side ? 1 : 0)`, ale _old_ nečte tsize
     *    přes libs/dsk - používá vlastní FS layer. Pro _new_ s libs/dsk
     *    musíme udržet tracks count přesný. */
    uint8_t old_track_count = 0;
    (void)dsk_read_on_offset(&drv->handler, 0x30, &old_track_count, 1);
    uint8_t want_track_count = (uint8_t)(track + 1);
    if (want_track_count > old_track_count)
    {
        if (EXIT_SUCCESS != dsk_write_on_offset(&drv->handler, 0x30, &want_track_count, 1))
        {
            return 0;
        }
    }

    /* Označ paměťový handler jako modifikovaný - při sync/save na FS
     * vrstvu projde zpět do souboru. Pro file handler je sync implicitní
     * (write už šel přes file_driver_write_cb → fwrite), preskočit. */
    if (drv->handler.type == HANDLER_TYPE_MEMORY)
    {
        drv->handler.spec.memspec.updated = 1;
    }

#ifdef FDC_DIAG
    /* Verify-read Track-Info ssize byte (= track_offset + 20). Detekuje
     * případ, kdy by se zápis dostal jinam než očekáváno (např. silent
     * generic_driver write failure nebo offset mismatch). */
    {
        uint8_t verify_ssize = 0;
        (void)dsk_read_on_offset(&drv->handler, track_offset + 20, &verify_ssize, 1);
        fprintf(stderr, "fdc: WRITE TRACK flush VERIFY - track_offset=0x%X "
                        "ssize_byte_written=%u ssize_byte_readback=%u\n",
                track_offset, ssize_code, verify_ssize);
    }
#endif

    /* Geometry se změnila (nový/upravený track) - znovu načti cache
     * z modifikované DSK hlavičky. Pozn.: NEinvalidujeme jen flagem,
     * protože drive_is_ready() vyžaduje geometry_valid=1 pro další
     * WRITE TRACK volání (formátování druhé strany / další stopy). */
    if (EXIT_SUCCESS == dsk_get_geometry(&drv->handler, &drv->geometry))
    {
        drv->geometry_valid = 1;
    }
    else
    {
        drv->geometry_valid = 0;
    }

    return 1;
}

/**
 * @brief Zpracuje jeden byte data z CPU během WRITE TRACK.
 *
 * Volá se z wd279x_write_byte() (DATA register) když je v běhu WRITE TRACK
 * příkaz (= regCOMMAND ∈ {0xF0, 0xF4}). Symetrie s _old_ wd279x_old_do_write_track().
 *
 * State machine fází 0..5 (mirror _old_):
 *  - Stage 0: čekání na Index Address Mark (IAM, 0xFC). Po IAM přejde na 1.
 *    Při T=0 a S=0 zapíše Sharp DSK header magic (info pole o tvůrci).
 *  - Stage 1: čekání na ID Address Mark (IDAM, 0xFE). Po IDAM init Track-Info
 *    skládání v chip->buffer.
 *  - Stage 2: 4 bytes ID (track, side, sector, size), pak čekání na
 *    Data Address Mark (0xFB nebo 0xF8).
 *  - Stage 3: data sektoru (DATA_COUNTER bytes). První byte uložen jako
 *    fill_byte. Po posledním bytu přejde na stage 4.
 *  - Stage 4: čekání na další ID (zpět do stage 2) NEBO timeout = flush
 *    nového Track-Info bloku do DSK image, přechod do stage 5 = idle.
 *
 * @param chip ukazatel na chip.
 * @param io_data byte z CPU (true-bus, NEinvertovaný).
 */
void do_write_track_byte(st_WD279X *chip, uint8_t io_data)
{
#ifdef FDC_DIAG
    uint8_t prev_stage = chip->write_track_stage;
    fprintf(stderr, "fdc: WRITE TRACK byte stage=%u cnt=%u data=0x%02X\n",
            chip->write_track_stage, chip->write_track_counter, io_data);
#endif

    /* Stage 0: čekáme na IAM marker 0xFC. */
    if (chip->write_track_stage == 0)
    {
        if (io_data == 0xFC)
        {
            chip->write_track_stage = 1;
            chip->write_track_counter = 0;

            /* Při T=0 S=0 zapíšeme "Unicard v1.00" identifikační blok
             * + 204 nul (= zbytek 0x22..0x100 oblasti DSK hlavičky).
             * Symetrie s _old_ ř. 790-846. */
            if (chip->regTRACK == 0 && (chip->SIDE & 0x01) == 0)
            {
                struct st_FDDrive *drv = current_drive(chip);
                if (drv && !drv->readonly)
                {
                    /* "Unicard v1.00\0\0" + 0x02 0x00 0x00 = 18 bytes
                     * (creator string + reserved). Symetrie s _old_ ř. 802. */
                    static const uint8_t creator[18] = {
                        'U', 'n', 'i', 'c', 'a', 'r', 'd', ' ', 'v', '1', '.', '0', '0',
                        0x00, 0x00, 0x02, 0x00, 0x00
                    };
                    (void)dsk_write_on_offset(&drv->handler, 0x22, creator, 18);

                    /* Vynulování zbytku hlavičky (offset 0x34..0xFF = 204 bytů). */
                    uint8_t zeros[0x100 - 0x34];
                    memset(zeros, 0, sizeof(zeros));
                    (void)dsk_write_on_offset(&drv->handler, 0x34, zeros, sizeof(zeros));
                }
            }

            /* Připrav buffer na sběr Track-Info dat. */
            memset(chip->buffer, 0, WD279X_BUFFER_SIZE);
        }
        else if (chip->write_track_counter > 100)
        {
            /* Timeout - nikdo neformátuje. Reset chipu. */
            chip->regSTATUS = 0x00;
            chip->regCOMMAND = 0x00;
            chip->write_track_stage = 5;
            chip->intrq_active = 1;
            return;
        }
    }
    /* Stage 1: čekáme na první ID Address Mark (IDAM, 0xFE). */
    else if (chip->write_track_stage == 1)
    {
        if (io_data == 0xFE)
        {
            chip->write_track_stage = 2;
            chip->write_track_counter = 0;

            /* Inicializace Track-Info build bufferu. */
            chip->buffer[0] = chip->regTRACK;
            chip->buffer[1] = chip->SIDE & 0x01;
            /* [2..3] unused, [4] sector size code (vyplní stage 2),
             * [5] = 1 (= aktuálně budujeme první sektor), [6] GAP3, [7] filler. */
            chip->buffer[5] = 1;
            chip->buffer[6] = 0x4E;
            chip->buffer[7] = 0xE5;
            chip->buffer_pos = 8; /* další bajt = sector ID list */
        }
        else if (chip->write_track_counter > 100)
        {
            chip->regSTATUS = 0x00;
            chip->regCOMMAND = 0x00;
            chip->write_track_stage = 5;
            chip->intrq_active = 1;
            return;
        }
    }
    /* Stage 2: 4 bytes ID (counter 0..3 = track, side, sector, size),
     * potom čekání na DATA AM (0xFB/0xF8). */
    else if (chip->write_track_stage == 2)
    {
        if (chip->write_track_counter <= 4)
        {
            if (chip->write_track_counter == 3)
            {
                /* Sector ID - uložíme do buffer (sector ID list). */
                if (chip->buffer_pos < WD279X_BUFFER_SIZE)
                {
                    chip->buffer[chip->buffer_pos++] = io_data;
                }
            }
            else if (chip->write_track_counter == 4)
            {
                /* Kód velikosti sektoru N (0=128, 1=256, 2=512, 3=1024 B).
                 * Předpokládáme stejnou velikost pro všechny sektory
                 * na stopě. */
                chip->buffer[4] = io_data;
            }
        }
        else if (io_data == 0xFB || io_data == 0xF8)
        {
            /* Data Address Mark (0xFB = data, 0xF8 = deleted data).
             * Počet bajtů datového bloku je dán kódem N z ID pole
             * (128 << N). Kód mimo 0..3 dsk_decode_sector_size() ořízne
             * na 1024 B a write_track_flush() takovou stopu odmítne. */
            chip->write_track_stage = 3;
            chip->write_track_counter = 0;
            chip->data_counter = dsk_decode_sector_size((en_DSK_SECTOR_SIZE)chip->buffer[4]);
        }
        else if (chip->write_track_counter > 100)
        {
            chip->regSTATUS = 0x00;
            chip->regCOMMAND = 0x00;
            chip->write_track_stage = 5;
            chip->intrq_active = 1;
            return;
        }
    }
    /* Stage 3: data sektoru. */
    else if (chip->write_track_stage == 3)
    {
        if (chip->write_track_counter == 1)
        {
            /* První byte = fill pattern (Sharp formát používá uniform fill). */
            chip->buffer[WD279X_BUFFER_SIZE - 1] = io_data;
        }
        if (chip->write_track_counter > chip->data_counter)
        {
            chip->write_track_stage = 4;
            chip->write_track_counter = 0;
            chip->data_counter = 0;
        }
    }
    /* Stage 4: čekání na další ID (= další sektor) nebo timeout = end of track. */
    else if (chip->write_track_stage == 4)
    {
        if (io_data == 0xFE)
        {
            /* Další sektor. */
            chip->write_track_stage = 2;
            chip->write_track_counter = 0;
            chip->buffer[5]++; /* zvýšíme sector count */
        }
        else if (chip->write_track_counter > 200)
        {
            /* End of track - flush do DSK. */
            chip->regCOMMAND = 0x00;
            chip->regSTATUS = 0x00;
            chip->write_track_stage = 5;
            chip->intrq_active = 1;

            struct st_FDDrive *drv = current_drive(chip);
            if (drv)
            {
                /* Formátování stopy = zápis na médium (media_writes.h),
                 * i když flush selže - obraz už mohl změnit hlavičku. */
                MEDIA_WRITES_NOTE();
                (void)write_track_flush(chip, drv);
            }
            return;
        }
    }
    /* Stage 5: idle - end of operation. Byty ignorujeme. */

    chip->write_track_counter++;

#ifdef FDC_DIAG
    if (prev_stage != chip->write_track_stage)
    {
        fprintf(stderr, "fdc: WRITE TRACK stage change %u -> %u "
                        "(track=%u side=%u sectors_so_far=%u)\n",
                prev_stage, chip->write_track_stage,
                chip->regTRACK, chip->SIDE & 0x01, chip->buffer[5]);
    }
#endif
}

