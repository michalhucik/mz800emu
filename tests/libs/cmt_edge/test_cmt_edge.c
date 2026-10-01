#include "mztest.h"

#include <stdio.h>
#include <stdlib.h>

#include "emulator/hw-generic/cmt/cmt_edge.h"
#include "emulator/hw-generic/cmt/cmt_save.h"
#include "emulator/hw-generic/cmt/cmtext.h"
#include "hw-generic/gdg/gdgclk.h"

static const char *g_temp_path = "test_cmt_edge_roundtrip.lep";
static const char *g_temp_l16_path = "test_cmt_edge_drift.l16";
static const char *g_temp_wav_path = "test_cmt_save_drift.wav";

void setUp(void)
{
    remove(g_temp_path);
    remove(g_temp_l16_path);
    remove(g_temp_wav_path);
}

void tearDown(void)
{
    remove(g_temp_path);
    remove(g_temp_l16_path);
    remove(g_temp_wav_path);
}

static void assert_pulse(st_CMT_STREAM *stream, uint64_t expected_units, int expected_value)
{
    uint64_t units = 0;
    int value = -1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS,
                          cmt_vstream_read_pulse(stream->str.vstream, &units, &value));
    TEST_ASSERT_EQUAL_UINT64(expected_units, units);
    TEST_ASSERT_EQUAL_INT(expected_value, value);
}

static void test_decode_lep_runs_and_continuation(void)
{
    const uint8_t bytes[] = { 5, (uint8_t) (int8_t) -3, 0, 2 };
    st_CMT_STREAM *stream = cmt_edge_stream_from_data(bytes, sizeof(bytes),
                                                      CMT_EDGE_LEP_RATE,
                                                      CMT_STREAM_POLARITY_NORMAL);
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_EQUAL_UINT32(CMT_EDGE_LEP_RATE, cmt_stream_get_rate(stream));
    TEST_ASSERT_EQUAL_UINT64(137, cmt_stream_get_count_scans(stream));

    cmt_vstream_read_reset(stream->str.vstream);
    /* LEP/L16 signs are physical connector levels.  Sharp's interface
     * inverts them before 8255 PC5, represented here by the stream value. */
    assert_pulse(stream, 5, 0);
    assert_pulse(stream, 130, 1);
    assert_pulse(stream, 2, 0);
    uint64_t units;
    int value;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE,
                          cmt_vstream_read_pulse(stream->str.vstream, &units, &value));
    cmt_stream_destroy(stream);
}

static void test_decode_rejects_empty_and_initial_zero(void)
{
    const uint8_t zero[] = { 0 };
    TEST_ASSERT_NULL(cmt_edge_stream_from_data(NULL, 0, CMT_EDGE_LEP_RATE,
                                               CMT_STREAM_POLARITY_NORMAL));
    TEST_ASSERT_NULL(cmt_edge_stream_from_data(zero, sizeof(zero), CMT_EDGE_LEP_RATE,
                                               CMT_STREAM_POLARITY_NORMAL));
}

static void test_decode_accepts_reference_int8_min(void)
{
    const uint8_t bytes[] = { 0x80 };
    st_CMT_STREAM *stream = cmt_edge_stream_from_data(bytes, sizeof(bytes),
                                                      CMT_EDGE_L16_RATE,
                                                      CMT_STREAM_POLARITY_NORMAL);
    TEST_ASSERT_NOT_NULL(stream);
    cmt_vstream_read_reset(stream->str.vstream);
    assert_pulse(stream, 128, 1);
    cmt_stream_destroy(stream);
}

static void test_decode_applies_inverted_polarity(void)
{
    const uint8_t bytes[] = { 4, (uint8_t) (int8_t) -2 };
    st_CMT_STREAM *stream = cmt_edge_stream_from_data(bytes, sizeof(bytes),
                                                      CMT_EDGE_LEP_RATE,
                                                      CMT_STREAM_POLARITY_INVERTED);
    TEST_ASSERT_NOT_NULL(stream);
    cmt_vstream_read_reset(stream->str.vstream);
    assert_pulse(stream, 4, 1);
    assert_pulse(stream, 2, 0);
    cmt_stream_destroy(stream);
}

static void assert_encoded_run(int value, uint64_t units,
                               const uint8_t *expected, size_t expected_size)
{
    uint8_t output[8] = { 0 };
    size_t written = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS,
                          cmt_edge_encode_run(value, units, output,
                                              sizeof(output), &written));
    TEST_ASSERT_EQUAL_UINT(expected_size, written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, output, expected_size);
    TEST_ASSERT_NOT_EQUAL(0, output[0]);
}

static void test_encode_canonical_long_runs(void)
{
    const uint8_t high_300[] = { 46, 0, 0 };
    const uint8_t low_300[] = {
        (uint8_t) (int8_t) -46, 0, 0
    };
    const uint8_t high_254[] = { 127, 0 };
    const uint8_t high_255[] = { 1, 0, 0 };
    assert_encoded_run(1, 300, high_300, sizeof(high_300));
    assert_encoded_run(0, 300, low_300, sizeof(low_300));
    assert_encoded_run(1, 254, high_254, sizeof(high_254));
    assert_encoded_run(1, 255, high_255, sizeof(high_255));
}

static void test_encode_rejects_invalid_arguments(void)
{
    uint8_t output[1];
    size_t written = 99;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE,
                          cmt_edge_encode_run(1, 0, output, sizeof(output), &written));
    TEST_ASSERT_EQUAL_UINT(0, written);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE,
                          cmt_edge_encode_run(1, 128, output, sizeof(output), &written));
}

static void test_file_roundtrip_preserves_pulses(void)
{
    const uint8_t canonical[] = { 46, 0, 0, (uint8_t) (int8_t) -7, 9 };
    st_CMT_STREAM *stream = cmt_stream_new(CMT_STREAM_TYPE_VSTREAM);
    TEST_ASSERT_NOT_NULL(stream);
    stream->str.vstream = cmt_vstream_new(CMT_EDGE_LEP_RATE,
                                          CMT_VSTREAM_BYTELENGTH8,
                                          1,
                                          CMT_STREAM_POLARITY_NORMAL);
    TEST_ASSERT_NOT_NULL(stream->str.vstream);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS,
                          cmt_vstream_add_value(stream->str.vstream, 1, 300));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS,
                          cmt_vstream_add_value(stream->str.vstream, 0, 7));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS,
                          cmt_vstream_add_value(stream->str.vstream, 1, 9));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS,
                          cmt_edge_stream_write_file(stream, g_temp_path));

    FILE *fh = fopen(g_temp_path, "rb");
    TEST_ASSERT_NOT_NULL(fh);
    uint8_t saved[16] = { 0 };
    size_t saved_size = fread(saved, 1, sizeof(saved), fh);
    fclose(fh);
    TEST_ASSERT_EQUAL_UINT(sizeof(canonical), saved_size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(canonical, saved, sizeof(canonical));

    st_CMT_STREAM *loaded = cmt_edge_stream_from_data(saved, saved_size,
                                                      CMT_EDGE_LEP_RATE,
                                                      CMT_STREAM_POLARITY_NORMAL);
    TEST_ASSERT_NOT_NULL(loaded);
    cmt_vstream_read_reset(loaded->str.vstream);
    assert_pulse(loaded, 300, 0);
    assert_pulse(loaded, 7, 1);
    assert_pulse(loaded, 9, 0);
    cmt_stream_destroy(loaded);
    cmt_stream_destroy(stream);
}

static void test_tick_conversion_has_no_cumulative_drift(void)
{
    int ok = 0;
    uint64_t seconds = 60u * 60u * 24u;
    uint64_t ticks = (uint64_t) GDGCLK_BASE * seconds;
    TEST_ASSERT_EQUAL_UINT64(seconds * CMT_EDGE_LEP_RATE,
                             cmt_edge_ticks_to_units(ticks, CMT_EDGE_LEP_RATE, &ok));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT64(seconds * CMT_EDGE_L16_RATE,
                             cmt_edge_ticks_to_units(ticks, CMT_EDGE_L16_RATE, &ok));
    TEST_ASSERT_TRUE(ok);
}

static void test_recorder_uses_native_rate_and_edge_intervals(void)
{
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS,
                          g_cmt_edge_save_extension.cb_open((char *) g_temp_path));
    TEST_ASSERT_NOT_NULL(g_cmt_edge_save_extension.block);

    g_cmt_edge_save_extension.cb_write((uint64_t) GDGCLK_BASE, 1);
    g_cmt_edge_save_extension.cb_write((uint64_t) GDGCLK_BASE * 2u, 0);

    st_CMT_STREAM *stream = g_cmt_edge_save_extension.block->stream;
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_EQUAL_UINT32(CMT_EDGE_LEP_RATE, cmt_stream_get_rate(stream));
    cmt_vstream_read_reset(stream->str.vstream);
    assert_pulse(stream, CMT_EDGE_LEP_RATE, 0);
    assert_pulse(stream, CMT_EDGE_LEP_RATE, 1);

    g_cmt_edge_save_extension.cb_eject();
}

static uint64_t rounded_rational_ticks(uint64_t edge,
                                       uint64_t units_numerator,
                                       uint64_t units_denominator,
                                       uint32_t rate)
{
    uint64_t denominator = units_denominator * (uint64_t) rate;
    uint64_t numerator = edge * units_numerator * (uint64_t) GDGCLK_BASE;
    return (numerator + denominator / 2u) / denominator;
}

static void read_and_check_alternating_pulses(st_CMT_STREAM *stream,
                                               unsigned pulse_count,
                                               uint64_t minimum_units,
                                               uint64_t maximum_units,
                                               uint64_t *total_out)
{
    uint64_t total = 0;
    cmt_vstream_read_reset(stream->str.vstream);
    for (unsigned pulse = 0; pulse < pulse_count; ++pulse) {
        uint64_t units = 0;
        int value = -1;
        TEST_ASSERT_EQUAL_INT(
            EXIT_SUCCESS,
            cmt_vstream_read_pulse(stream->str.vstream, &units, &value));
        TEST_ASSERT_GREATER_THAN_UINT64(0, units);
        TEST_ASSERT_TRUE(units >= minimum_units);
        TEST_ASSERT_TRUE(units <= maximum_units);
        TEST_ASSERT_EQUAL_INT((int) (pulse & 1u), value);
        total += units;
    }
    {
        uint64_t units = 0;
        int value = -1;
        TEST_ASSERT_EQUAL_INT(
            EXIT_FAILURE,
            cmt_vstream_read_pulse(stream->str.vstream, &units, &value));
    }
    *total_out = total;
}

static void test_recorder_quantizes_each_halfwave_independently(void)
{
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS,
                          g_cmt_edge_save_extension.cb_open((char *) g_temp_path));
    TEST_ASSERT_NOT_NULL(g_cmt_edge_save_extension.block);

    /* 37/8 = 4.625 LEP units per half-wave.  LEP is an edge-duration
     * format, so every identical physical interval independently rounds to
     * five units; no error is carried into the following pulse. */
    uint64_t delta_ticks = rounded_rational_ticks(
        1u, 37u, 8u, CMT_EDGE_LEP_RATE);
    for (uint64_t edge = 1; edge <= 8; ++edge)
        g_cmt_edge_save_extension.cb_write(
            delta_ticks * edge,
            (int) (edge & 1u));

    st_CMT_STREAM *stream = g_cmt_edge_save_extension.block->stream;
    TEST_ASSERT_NOT_NULL(stream);
    uint64_t recorded = 0;
    read_and_check_alternating_pulses(stream, 8, 5u, 5u, &recorded);
    TEST_ASSERT_EQUAL_UINT64(40u, recorded);

    g_cmt_edge_save_extension.cb_eject();
}

static void assert_edge_recorder_independent_rounding(
    const char *path,
    uint32_t rate,
    uint64_t units_numerator,
    uint64_t units_denominator,
    unsigned edge_count,
    uint64_t expected_units)
{
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS,
                          g_cmt_edge_save_extension.cb_open((char *) path));

    uint64_t delta_ticks = rounded_rational_ticks(
        1u, units_numerator, units_denominator, rate);
    TEST_ASSERT_EQUAL_UINT64(
        expected_units, cmt_edge_ticks_to_units(delta_ticks, rate, NULL));

    for (uint64_t edge = 1; edge <= edge_count; ++edge)
        g_cmt_edge_save_extension.cb_write(
            delta_ticks * edge, (int) (edge & 1u));

    st_CMT_STREAM *stream = g_cmt_edge_save_extension.block->stream;
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_EQUAL_UINT32(rate, cmt_stream_get_rate(stream));
    uint64_t recorded = 0;
    read_and_check_alternating_pulses(
        stream, edge_count, expected_units, expected_units, &recorded);
    TEST_ASSERT_EQUAL_UINT64(expected_units * edge_count, recorded);

    g_cmt_edge_save_extension.cb_eject();
}

static void test_lep_469_always_rounds_to_5(void)
{
    assert_edge_recorder_independent_rounding(
        g_temp_path, CMT_EDGE_LEP_RATE, 469u, 100u, 1000u, 5u);
}

static void test_lep_528_always_rounds_to_5(void)
{
    assert_edge_recorder_independent_rounding(
        g_temp_path, CMT_EDGE_LEP_RATE, 528u, 100u, 1000u, 5u);
}

static void test_lep_551_always_rounds_to_6(void)
{
    assert_edge_recorder_independent_rounding(
        g_temp_path, CMT_EDGE_LEP_RATE, 551u, 100u, 1000u, 6u);
}

static void test_l16_469_always_rounds_to_5(void)
{
    assert_edge_recorder_independent_rounding(
        g_temp_l16_path, CMT_EDGE_L16_RATE, 469u, 100u, 1000u, 5u);
}

static void assert_one_second_recording(uint32_t rate, const char *path)
{
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS,
                          g_cmt_edge_save_extension.cb_open((char *) path));
    g_cmt_edge_save_extension.cb_write((uint64_t) GDGCLK_BASE, 1);

    st_CMT_STREAM *stream = g_cmt_edge_save_extension.block->stream;
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_EQUAL_UINT32(rate, cmt_stream_get_rate(stream));
    cmt_vstream_read_reset(stream->str.vstream);
    assert_pulse(stream, rate, 0);
    {
        uint64_t units = 0;
        int value = -1;
        TEST_ASSERT_EQUAL_INT(
            EXIT_FAILURE,
            cmt_vstream_read_pulse(stream->str.vstream, &units, &value));
    }
    g_cmt_edge_save_extension.cb_eject();
}

static void assert_one_second_continuation_roundtrip(uint32_t rate)
{
    size_t capacity = ((size_t) rate + 126u) / 127u;
    uint8_t *encoded = (uint8_t *) malloc(capacity);
    TEST_ASSERT_NOT_NULL(encoded);

    size_t written = 0;
    TEST_ASSERT_EQUAL_INT(
        EXIT_SUCCESS,
        cmt_edge_encode_run(1, rate, encoded, capacity, &written));
    TEST_ASSERT_EQUAL_UINT(capacity, written);
    TEST_ASSERT_NOT_EQUAL(0, encoded[0]);
    for (size_t i = 1; i < written; ++i)
        TEST_ASSERT_EQUAL_UINT8(0, encoded[i]);

    st_CMT_STREAM *stream = cmt_edge_stream_from_data(
        encoded, written, rate, CMT_STREAM_POLARITY_NORMAL);
    TEST_ASSERT_NOT_NULL(stream);
    cmt_vstream_read_reset(stream->str.vstream);
    assert_pulse(stream, rate, 0);

    cmt_stream_destroy(stream);
    free(encoded);
}

static void test_one_second_pulses_and_continuations(void)
{
    assert_one_second_recording(CMT_EDGE_LEP_RATE, g_temp_path);
    assert_one_second_recording(CMT_EDGE_L16_RATE, g_temp_l16_path);
    assert_one_second_continuation_roundtrip(CMT_EDGE_LEP_RATE);
    assert_one_second_continuation_roundtrip(CMT_EDGE_L16_RATE);
}

static void test_subunit_interval_saturates_to_one(void)
{
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS,
                          g_cmt_edge_save_extension.cb_open((char *) g_temp_path));

    /* LEP/L16 has no zero-length token; an independently rounded zero is
     * therefore saturated to the smallest representable run. */
    g_cmt_edge_save_extension.cb_write(1u, 1);
    st_CMT_STREAM *stream = g_cmt_edge_save_extension.block->stream;
    TEST_ASSERT_NOT_NULL(stream);
    cmt_vstream_read_reset(stream->str.vstream);
    assert_pulse(stream, 1u, 0);

    g_cmt_edge_save_extension.cb_eject();
}

static void test_wav_recorder_has_no_long_term_drift(void)
{
    const unsigned edge_count = 1000;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS,
                          g_cmt_save_extension.cb_open((char *) g_temp_wav_path));

    uint64_t final_ticks = 0;
    for (uint64_t edge = 1; edge <= edge_count; ++edge) {
        final_ticks = rounded_rational_ticks(
            edge, 37u, 8u, CMTSAVE_DEFAULT_SAMPLERATE);
        g_cmt_save_extension.cb_write(final_ticks, (int) (edge & 1u));
    }

    st_CMT_STREAM *stream = g_cmt_save_extension.block->stream;
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_EQUAL_UINT32(CMTSAVE_DEFAULT_SAMPLERATE,
                             cmt_stream_get_rate(stream));
    uint64_t expected = cmt_edge_ticks_to_units(
        final_ticks, CMTSAVE_DEFAULT_SAMPLERATE, NULL);
    TEST_ASSERT_EQUAL_UINT64(expected, cmt_stream_get_count_scans(stream));

    g_cmt_save_extension.cb_eject();
}

static void test_recording_extension_selection(void)
{
    st_CMTEXT *wav = cmtext_get_recording_extension_for_filename("capture.wav");
    st_CMTEXT *lep = cmtext_get_recording_extension_for_filename("capture.LEP");
    st_CMTEXT *l16 = cmtext_get_recording_extension_for_filename("capture.l16");
    TEST_ASSERT_NOT_NULL(wav);
    TEST_ASSERT_NOT_NULL(lep);
    TEST_ASSERT_NOT_NULL(l16);
    TEST_ASSERT_EQUAL_STRING("SAVE-WAV", cmtext_get_name(wav));
    TEST_ASSERT_EQUAL_STRING("SAVE-LEP/L16", cmtext_get_name(lep));
    TEST_ASSERT_EQUAL_PTR(lep, l16);
    TEST_ASSERT_EQUAL_PTR(wav, cmtext_get_recording_extension_for_filename("capture"));
    TEST_ASSERT_NULL(cmtext_get_recording_extension_for_filename("capture.unknown"));
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_decode_lep_runs_and_continuation);
    RUN_TEST(test_decode_rejects_empty_and_initial_zero);
    RUN_TEST(test_decode_accepts_reference_int8_min);
    RUN_TEST(test_decode_applies_inverted_polarity);
    RUN_TEST(test_encode_canonical_long_runs);
    RUN_TEST(test_encode_rejects_invalid_arguments);
    RUN_TEST(test_file_roundtrip_preserves_pulses);
    RUN_TEST(test_tick_conversion_has_no_cumulative_drift);
    RUN_TEST(test_recorder_uses_native_rate_and_edge_intervals);
    RUN_TEST(test_recorder_quantizes_each_halfwave_independently);
    RUN_TEST(test_lep_469_always_rounds_to_5);
    RUN_TEST(test_lep_528_always_rounds_to_5);
    RUN_TEST(test_lep_551_always_rounds_to_6);
    RUN_TEST(test_l16_469_always_rounds_to_5);
    RUN_TEST(test_one_second_pulses_and_continuations);
    RUN_TEST(test_subunit_interval_saturates_to_one);
    RUN_TEST(test_wav_recorder_has_no_long_term_drift);
    RUN_TEST(test_recording_extension_selection);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
