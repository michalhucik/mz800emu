/* Native LEP/L16 edge-stream CMT loader and recorder. */

#include "mzarch/mzarch_config.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "baseui/baseui.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/gdgclk.h"
#include "libs/cmt_stream/cmt_stream.h"

#include "cmt.h"
#include "cmt_edge.h"
#include "cmtext_block.h"
#include "cmtext_container.h"

typedef struct st_CMTEDGE_RECORD_SPEC {
    char *filepath;
    uint32_t rate;
    uint64_t last_ticks;
    int current_level;
    int have_edge;
    int write_error;
} st_CMTEDGE_RECORD_SPEC;

static char *g_cmt_edge_fileext[] = { "lep", "l16", NULL };

static st_CMTEXT_INFO g_cmt_edge_info = {
    "LEP/L16",
    g_cmt_edge_fileext,
    "Native LEP/L16 edge-stream CMT extension",
    CMTEXT_TYPE_PLAYABLE
};

static st_CMTEXT_INFO g_cmt_edge_save_info = {
    "SAVE-LEP/L16",
    g_cmt_edge_fileext,
    "Native LEP/L16 edge-stream CMT recorder",
    CMTEXT_TYPE_RECORDABLE
};

static uint32_t cmt_edge_rate_from_filename(const char *filename)
{
    const char *ext = cmtext_get_filename_extension(filename);
    if (!ext)
        return 0;
    if (strcasecmp(ext, "lep") == 0)
        return CMT_EDGE_LEP_RATE;
    if (strcasecmp(ext, "l16") == 0)
        return CMT_EDGE_L16_RATE;
    return 0;
}

static int cmt_edge_decode_slot(uint8_t raw)
{
    return raw <= INT8_MAX ? (int) raw : (int) raw - 256;
}

st_CMT_STREAM *cmt_edge_stream_from_data(const uint8_t *data,
                                         size_t size,
                                         uint32_t rate,
                                         en_CMT_STREAM_POLARITY polarity)
{
    if (!data || size == 0 || data[0] == 0 || rate == 0)
        return NULL;

    int first_slot = cmt_edge_decode_slot(data[0]);

    /*
     * LEP/L16 stores the physical level on the external CMT connector:
     * positive = HIGH, negative = LOW.  Sharp's cassette interface inverts
     * that signal before it reaches 8255 PC5, while g_cmt.output represents
     * the logical PC5 level directly.  Convert at this boundary so the
     * emulated software observes the same signal as on real hardware.
     *
     * The user-selectable CMT polarity is applied afterwards and therefore
     * remains an additional inversion, matching WAV playback behaviour.
     */
    int level = first_slot < 0;

    st_CMT_STREAM *stream = cmt_stream_new(CMT_STREAM_TYPE_VSTREAM);
    if (!stream)
        return NULL;

    stream->str.vstream = cmt_vstream_new(rate,
                                          CMT_VSTREAM_BYTELENGTH8,
                                          level,
                                          CMT_STREAM_POLARITY_NORMAL);
    if (!stream->str.vstream) {
        cmt_stream_destroy(stream);
        return NULL;
    }

    for (size_t i = 0; i < size; ++i) {
        int slot = cmt_edge_decode_slot(data[i]);
        uint32_t units;

        if (slot == 0) {
            units = 127;
        } else {
            level = slot < 0;
            /* Widen before negation: INT8_MIN is intentionally accepted as
             * the reference player's LOW run of 128 units. */
            units = (uint32_t) (slot < 0 ? -slot : slot);
        }

        if (cmt_vstream_add_value(stream->str.vstream, level, units) != EXIT_SUCCESS) {
            cmt_stream_destroy(stream);
            return NULL;
        }
    }

    cmt_stream_set_polarity(stream, polarity);
    return stream;
}

static st_CMT_STREAM *cmt_edge_stream_from_file(const char *filename,
                                                uint32_t rate,
                                                en_CMT_STREAM_POLARITY polarity)
{
    FILE *fh = baseui_tools_file_open(filename, "rb");
    if (!fh)
        return NULL;

    if (fseek(fh, 0, SEEK_END) != 0) {
        baseui_tools_file_close(fh);
        return NULL;
    }
    long length = ftell(fh);
    if (length <= 0 || fseek(fh, 0, SEEK_SET) != 0) {
        baseui_tools_file_close(fh);
        return NULL;
    }

    if ((unsigned long) length > UINT_MAX) {
        baseui_tools_file_close(fh);
        return NULL;
    }

    uint8_t *data = (uint8_t *) baseui_tools_mem_alloc((size_t) length);
    if (!data) {
        baseui_tools_file_close(fh);
        return NULL;
    }

    size_t read_count = baseui_tools_file_read(data, 1, (size_t) length, fh);
    baseui_tools_file_close(fh);
    if (read_count != (size_t) length) {
        baseui_tools_mem_free(data);
        return NULL;
    }

    st_CMT_STREAM *stream = cmt_edge_stream_from_data(data,
                                                      (size_t) length,
                                                      rate,
                                                      polarity);
    baseui_tools_mem_free(data);
    return stream;
}

int cmt_edge_encode_run(int value,
                        uint64_t units,
                        uint8_t *output,
                        size_t capacity,
                        size_t *written)
{
    if (written)
        *written = 0;
    if (!output || !written || (value != 0 && value != 1) || units == 0)
        return EXIT_FAILURE;

    /*
     * A continuation byte 0x00 extends the PRECEDING non-zero interval by
     * exactly 127 units.  Therefore a long run must be encoded as one
     * signed non-zero remainder followed only by zero continuation bytes.
     *
     * Examples:
     *   127 -> +127
     *   254 -> +127, 0
     *   255 -> +1,   0, 0
     *   300 -> +46,  0, 0
     *
     * Do not emit +127,0,+46 for 300: QDTool deliberately keeps adjacent
     * non-zero records separate, so that form would create a false run
     * boundary even though the electrical level did not change.
     */
    uint64_t continuation_count = units / 127u;
    uint8_t magnitude = (uint8_t) (units % 127u);
    if (magnitude == 0) {
        magnitude = 127;
        continuation_count--;
    }

    uint64_t needed64 = 1u + continuation_count;
    if (needed64 > SIZE_MAX || (size_t) needed64 > capacity)
        return EXIT_FAILURE;

    size_t pos = 0;
    output[pos++] = value ? magnitude : (uint8_t) (0u - magnitude);
    while (continuation_count-- != 0)
        output[pos++] = 0;

    *written = pos;
    return EXIT_SUCCESS;
}

static int cmt_edge_write_run(FILE *fh, int value, uint64_t units)
{
    if (units == 0)
        return EXIT_FAILURE;

    uint64_t continuation_count = units / 127u;
    uint8_t magnitude = (uint8_t) (units % 127u);
    if (magnitude == 0) {
        magnitude = 127;
        continuation_count--;
    }

    uint8_t byte = value ? magnitude : (uint8_t) (0u - magnitude);
    if (baseui_tools_file_write(&byte, 1, 1, fh) != 1)
        return EXIT_FAILURE;

    byte = 0;
    while (continuation_count-- != 0) {
        if (baseui_tools_file_write(&byte, 1, 1, fh) != 1)
            return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int cmt_edge_stream_write_file(st_CMT_STREAM *stream, const char *filename)
{
    if (!stream || !filename || stream->stream_type != CMT_STREAM_TYPE_VSTREAM ||
        !stream->str.vstream || stream->str.vstream->scans == 0)
        return EXIT_FAILURE;

    FILE *fh = baseui_tools_file_open(filename, "wb");
    if (!fh)
        return EXIT_FAILURE;

    st_CMT_VSTREAM *vstream = stream->str.vstream;
    cmt_vstream_read_reset(vstream);

    uint64_t units;
    int value;
    int result = EXIT_SUCCESS;
    while (cmt_vstream_read_pulse(vstream, &units, &value) == EXIT_SUCCESS) {
        if (cmt_edge_write_run(fh, value, units) != EXIT_SUCCESS) {
            result = EXIT_FAILURE;
            break;
        }
    }

    if (result == EXIT_SUCCESS && fflush(fh) != 0)
        result = EXIT_FAILURE;
    baseui_tools_file_close(fh);
    cmt_vstream_read_reset(vstream);
    return result;
}

uint64_t cmt_edge_ticks_to_units(uint64_t ticks, uint32_t rate, int *ok)
{
    const uint64_t base = (uint64_t) GDGCLK_BASE;
    if (ok)
        *ok = 0;
    if (rate == 0 || base == 0)
        return 0;

    uint64_t quotient = ticks / base;
    uint64_t remainder = ticks % base;
    if (quotient > UINT64_MAX / rate)
        return 0;

    uint64_t units = quotient * rate;
    uint64_t fraction = remainder * (uint64_t) rate;
    uint64_t rounded = (fraction + base / 2u) / base;
    if (units > UINT64_MAX - rounded)
        return 0;

    if (ok)
        *ok = 1;
    return units + rounded;
}

static void cmt_edge_block_close(st_CMTEXT_BLOCK *block, int recording)
{
    if (!block)
        return;
    if (recording && block->spec) {
        st_CMTEDGE_RECORD_SPEC *spec = (st_CMTEDGE_RECORD_SPEC *) block->spec;
        if (spec->filepath)
            baseui_tools_mem_free(spec->filepath);
        baseui_tools_mem_free(spec);
        block->spec = NULL;
    }
    /* cmtext_block_destroy historically does not release a non-NULL stream. */
    cmt_stream_destroy(block->stream);
    block->stream = NULL;
    cmtext_block_destroy(block);
}

static void cmt_edge_play_eject(void)
{
    cmt_edge_block_close(g_cmt_edge_extension.block, 0);
    g_cmt_edge_extension.block = NULL;
    cmtext_container_destroy(g_cmt_edge_extension.container);
    g_cmt_edge_extension.container = NULL;
}

static int cmt_edge_play_open(char *filename)
{
    uint32_t rate = cmt_edge_rate_from_filename(filename);
    if (!rate)
        return EXIT_FAILURE;

    cmt_edge_play_eject();
    st_CMT_STREAM *stream = cmt_edge_stream_from_file(filename, rate, g_cmt.polarity);
    if (!stream)
        return EXIT_FAILURE;

    st_CMTEXT_CONTAINER *container = cmtext_container_new(CMTEXT_CONTAINER_TYPE_SINGLE,
                                                          filename, 1,
                                                          NULL, NULL, NULL, NULL);
    st_CMTEXT_BLOCK *block = cmtext_block_new(0, CMTEXT_BLOCK_TYPE_WAV, stream,
                                              CMTEXT_BLOCK_SPEED_NONE, 0, NULL);
    if (!container || !block) {
        cmtext_container_destroy(container);
        if (block)
            cmt_edge_block_close(block, 0);
        else
            cmt_stream_destroy(stream);
        return EXIT_FAILURE;
    }

    block->cb_play = cmtext_block_play;
    block->cb_get_playname = cmtext_block_get_playname;
    block->cb_set_polarity = cmtext_block_set_polarity;

    g_cmt_edge_extension.container = container;
    g_cmt_edge_extension.block = block;
    return EXIT_SUCCESS;
}

static void cmt_edge_record_eject(void)
{
    cmt_edge_block_close(g_cmt_edge_save_extension.block, 1);
    g_cmt_edge_save_extension.block = NULL;
    cmtext_container_destroy(g_cmt_edge_save_extension.container);
    g_cmt_edge_save_extension.container = NULL;
}

static int cmt_edge_record_open(char *filename)
{
    uint32_t rate = cmt_edge_rate_from_filename(filename);
    if (!rate)
        return EXIT_FAILURE;

    cmt_edge_record_eject();

    FILE *fh = baseui_tools_file_open(filename, "wb");
    if (!fh)
        return EXIT_FAILURE;
    baseui_tools_file_close(fh);

    st_CMTEDGE_RECORD_SPEC *spec = (st_CMTEDGE_RECORD_SPEC *)
        baseui_tools_mem_alloc0(sizeof(*spec));
    if (!spec)
        return EXIT_FAILURE;
    size_t path_size = strlen(filename) + 1;
    spec->filepath = (char *) baseui_tools_mem_alloc(path_size);
    if (!spec->filepath) {
        baseui_tools_mem_free(spec);
        return EXIT_FAILURE;
    }
    memcpy(spec->filepath, filename, path_size);
    spec->rate = rate;

    st_CMTEXT_CONTAINER *container = cmtext_container_new(CMTEXT_CONTAINER_TYPE_SINGLE,
                                                          filename, 1,
                                                          NULL, NULL, NULL, NULL);
    st_CMTEXT_BLOCK *block = cmtext_block_new(0, CMTEXT_BLOCK_TYPE_WAV, NULL,
                                              CMTEXT_BLOCK_SPEED_NONE, 0, spec);
    if (!container || !block) {
        cmtext_container_destroy(container);
        if (block)
            cmt_edge_block_close(block, 1);
        else {
            baseui_tools_mem_free(spec->filepath);
            baseui_tools_mem_free(spec);
        }
        return EXIT_FAILURE;
    }

    g_cmt_edge_save_extension.container = container;
    g_cmt_edge_save_extension.block = block;
    return EXIT_SUCCESS;
}

static int cmt_edge_record_append(st_CMTEXT_BLOCK *block,
                                  st_CMTEDGE_RECORD_SPEC *spec,
                                  uint64_t end_ticks,
                                  int level)
{
    if (end_ticks <= spec->last_ticks)
        return EXIT_SUCCESS;

    /*
     * LEP/L16 is an edge-duration format, not a sampled waveform.  Quantize
     * each physical run independently to the nearest format unit.
     *
     * Do not carry quantization error into the next run: the stored width of
     * one pulse must not depend on previous pulses.  WAV is different and
     * keeps its own sample-phase/error accumulator.
     *
     * The stored physical level is still the level that existed BEFORE the
     * current edge.
     */
    uint64_t delta_ticks = end_ticks - spec->last_ticks;
    int ok = 0;
    uint64_t count = cmt_edge_ticks_to_units(delta_ticks, spec->rate, &ok);
    if (!ok)
        return EXIT_FAILURE;

    /* LEP/L16 cannot encode an empty run.  Saturation affects only intervals
     * shorter than half of the smallest format unit. */
    if (count == 0)
        count = 1;
    if (count > UINT32_MAX)
        return EXIT_FAILURE;

    if (!block->stream) {
        block->stream = cmt_stream_new(CMT_STREAM_TYPE_VSTREAM);
        if (!block->stream)
            return EXIT_FAILURE;
        block->stream->str.vstream = cmt_vstream_new(spec->rate,
                                                     CMT_VSTREAM_BYTELENGTH8,
                                                     level & 1,
                                                     CMT_STREAM_POLARITY_NORMAL);
        if (!block->stream->str.vstream) {
            cmt_stream_destroy(block->stream);
            block->stream = NULL;
            return EXIT_FAILURE;
        }
    }

    if (cmt_vstream_add_value(block->stream->str.vstream,
                              level & 1,
                              (uint32_t) count) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    spec->last_ticks = end_ticks;
    return EXIT_SUCCESS;
}

static void cmt_edge_record_write(uint64_t play_ticks, int value)
{
    st_CMTEXT_BLOCK *block = g_cmt_edge_save_extension.block;
    if (!block || !block->spec)
        return;

    st_CMTEDGE_RECORD_SPEC *spec = (st_CMTEDGE_RECORD_SPEC *) block->spec;
    if (spec->write_error)
        return;

    /*
     * cmt_write_data() invokes cb_write() AFTER PC1 changed and passes
     * the inverted PC1 level.  That value is therefore the NEW physical
     * cassette-connector level after the edge.
     *
     * LEP/L16, however, stores the duration of the level that existed
     * BEFORE the edge.  The previous implementation appended "value"
     * directly and then stored ~value as current_level.  That shifted the
     * complete waveform by one half-wave.  QDTool then paired the wrong
     * LOW/HIGH runs and could report:
     *
     *   "The tape waveform has a missing LONG byte-sync pulse."
     *
     * On the first observed edge the previous physical level is necessarily
     * the opposite of the new level (pio8255 calls cmt_write_data only on a
     * change).  On following edges we already know it in current_level.
     */
    int new_level = value & 1;
    int interval_level;

    if (!spec->have_edge) {
        interval_level = (~new_level) & 1;
    } else {
        interval_level = spec->current_level & 1;
    }

    if (cmt_edge_record_append(block, spec, play_ticks, interval_level) != EXIT_SUCCESS) {
        spec->write_error = 1;
        return;
    }

    spec->current_level = new_level;
    spec->have_edge = 1;
}

static void cmt_edge_record_stop(void)
{
    st_CMTEXT_BLOCK *block = g_cmt_edge_save_extension.block;
    if (!block || !block->spec)
        return;
    st_CMTEDGE_RECORD_SPEC *spec = (st_CMTEDGE_RECORD_SPEC *) block->spec;

    if (spec->have_edge && !spec->write_error) {
        uint64_t stop_ticks = g_cmt.paused
            ? g_cmt.paused_time
            : gdg_get_total_ticks() - g_cmt.start_time;
        if (cmt_edge_record_append(block, spec, stop_ticks,
                                   spec->current_level) != EXIT_SUCCESS)
            spec->write_error = 1;
    }

    if (!spec->write_error && block->stream) {
        if (cmt_edge_stream_write_file(block->stream, spec->filepath) != EXIT_SUCCESS)
            spec->write_error = 1;
    }
    if (spec->write_error)
        baseui_error("LEP/L16 recording: can't write file '%s'\n", spec->filepath);
}

static void cmt_edge_play_exit(void)
{
    cmt_edge_play_eject();
}

static void cmt_edge_record_exit(void)
{
    cmt_edge_record_eject();
}

st_CMTEXT g_cmt_edge_extension = {
    &g_cmt_edge_info,
    NULL,
    NULL,
    NULL,
    cmt_edge_play_exit,
    cmt_edge_play_open,
    NULL,
    cmt_edge_play_eject,
    NULL
};

st_CMTEXT g_cmt_edge_save_extension = {
    &g_cmt_edge_save_info,
    NULL,
    NULL,
    NULL,
    cmt_edge_record_exit,
    cmt_edge_record_open,
    cmt_edge_record_stop,
    cmt_edge_record_eject,
    cmt_edge_record_write
};
