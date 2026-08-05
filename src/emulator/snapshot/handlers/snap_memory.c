/**
 * @file snap_memory.c
 * @brief Snapshot handler: RAM, VRAM, EXVRAM (MZ-800) / PCG (MZ-1500)
 */
#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "memory/memory.h"
#include "mzarch/mzhal.h"

/* Zmrazená on-disk hodnota bitu 2 v <map> elementu MZ-700 snapshotu
 * (mzhal 11e): odpovídá MEMORY_MZ700_MAP_FLAG_PROHIBITED (1 << 2)
 * z mz700_memory.h - per-arch hlavičku sem nesmíme (compile-once TU),
 * hodnota je součást snapshot formátu a NESMÍ se měnit. */
#define SNAP_MZ700_MAP_FLAG_PROHIBITED (1 << 2)

static en_SNAPSHOT_RESULT snap_memory_save(st_SNAPSHOT_CONTEXT *ctx)
{
    en_SNAPSHOT_RESULT res;

    /* Uložení hlavní RAM (64 KB) */
    res = snapshot_io_write_bin(ctx->io, "memory/ram.bin",
                                g_memory.RAM, MEMORY_SIZE_RAM);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("memory", "Cannot write memory/ram.bin");
        return res;
    }

    /* Uložení VRAM - velikost bloku je zmrazený on-disk kontrakt,
     * runtime z g_mzhal (mzhal 9d; hodnoty per EXE beze změny). */
    res = snapshot_io_write_bin(ctx->io, "memory/vram.bin",
                                g_memory.VRAM, g_mzhal.mem_vram_size);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("memory", "Cannot write memory/vram.bin");
        return res;
    }

    /* MZ-800: rozšířená VRAM (banky III, IV); mem_exvram_size == 0 na
     * platformách bez EXVRAM = entry se nezapisuje (množina entries
     * per arch beze změny). */
    if (g_mzhal.mem_exvram_size > 0) {
        res = snapshot_io_write_bin(ctx->io, "memory/exvram.bin",
                                    g_memory.EXVRAM, g_mzhal.mem_exvram_size);
        if (res != SNAPSHOT_OK) {
            SNAP_ERR("memory", "Cannot write memory/exvram.bin");
            return res;
        }
    }

    /* MZ-1500: PCG banky (analogicky mem_pcg_size). */
    if (g_mzhal.mem_pcg_size > 0) {
        res = snapshot_io_write_bin(ctx->io, "memory/pcg.bin",
                                    g_memory.PCG, g_mzhal.mem_pcg_size);
        if (res != SNAPSHOT_OK) {
            SNAP_ERR("memory", "Cannot write memory/pcg.bin");
            return res;
        }
    }

    /* Uložení stavu paměťové mapy */
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "memory_state");
    /* MZ-700 snapshot kompatibilita (runtime, mzhal 11e): bit 2 byl
     * puvodne ROM_E800_mapped (1=mapped), po prejmenovani na PROHIBITED
     * ma invertovany vyznam (1=Prohibited active). Snapshot format
     * zachovan v puvodni semantice ROM_E800 - pri save invertujeme. */
    if (g_mzhal.arch == 700) {
        snapshot_xml_write_hex8(w, "map", g_memory.map ^ SNAP_MZ700_MAP_FLAG_PROHIBITED);
    } else {
        snapshot_xml_write_hex8(w, "map", g_memory.map);
    }
    snapshot_xml_close_element(w);

    char *xml = snapshot_xml_writer_finish(w);
    res = snapshot_io_write_xml(ctx->io, "memory/memory_state.xml", xml);
    g_free(xml);

    return res;
}

static en_SNAPSHOT_RESULT snap_memory_load(st_SNAPSHOT_CONTEXT *ctx)
{
    en_SNAPSHOT_RESULT res;

    /* Načtení hlavní RAM */
    res = snapshot_io_read_bin_into(ctx->io, "memory/ram.bin",
                                    g_memory.RAM, MEMORY_SIZE_RAM);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("memory", "Cannot load memory/ram.bin");
        return res;
    }

    /* Načtení VRAM - očekávaná velikost bloku runtime z g_mzhal
     * (zmrazený on-disk kontrakt, hodnoty per EXE beze změny). */
    res = snapshot_io_read_bin_into(ctx->io, "memory/vram.bin",
                                    g_memory.VRAM, g_mzhal.mem_vram_size);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("memory", "Cannot load memory/vram.bin");
        return res;
    }

    /* MZ-800: rozšířená VRAM (jen platformy s mem_exvram_size > 0). */
    if (g_mzhal.mem_exvram_size > 0) {
        res = snapshot_io_read_bin_into(ctx->io, "memory/exvram.bin",
                                        g_memory.EXVRAM, g_mzhal.mem_exvram_size);
        if (res != SNAPSHOT_OK) {
            SNAP_ERR("memory", "Cannot load memory/exvram.bin");
            return res;
        }
    }

    /* MZ-1500: PCG banky (jen platformy s mem_pcg_size > 0). */
    if (g_mzhal.mem_pcg_size > 0) {
        res = snapshot_io_read_bin_into(ctx->io, "memory/pcg.bin",
                                        g_memory.PCG, g_mzhal.mem_pcg_size);
        if (res != SNAPSHOT_OK) {
            SNAP_ERR("memory", "Cannot load memory/pcg.bin");
            return res;
        }
    }

    /* Načtení stavu paměťové mapy */
    char *xml = NULL;
    res = snapshot_io_read_xml(ctx->io, "memory/memory_state.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("memory", "Cannot load memory/memory_state.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("memory", "Parse error in memory/memory_state.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (snapshot_xml_enter_element(r, "memory_state")) {
        uint8_t map_val;
        if (snapshot_xml_read_hex8(r, "map", &map_val)) {
            /* Snapshot kompatibilita - viz save: invertujeme bit 2 zpet
             * z puvodni semantiky ROM_E800_mapped na novou PROHIBITED. */
            if (g_mzhal.arch == 700) {
                map_val ^= SNAP_MZ700_MAP_FLAG_PROHIBITED;
            }
            g_memory.map = map_val;
        }
        snapshot_xml_leave_element(r);
    }

    snapshot_xml_reader_free(r);

    /* Přepojení paměťových bank dle načtené mapy */
    memory_reconnect_ram();

    return SNAPSHOT_OK;
}

void snap_memory_register(void)
{
    snapshot_register_component("memory",
                                SNAPSHOT_PRIORITY_MEMORY,
                                snap_memory_save,
                                snap_memory_load,
                                false);
}
