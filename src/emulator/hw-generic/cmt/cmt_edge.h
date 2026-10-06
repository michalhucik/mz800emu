/*
 * Native LEP/L16 edge-stream CMT support.
 *
 * LEP stores one time unit as 50 us (20 kHz), L16 as 16 us (62.5 kHz).
 * A non-zero signed byte carries the signal level in its sign and the run
 * length in its magnitude.  A zero byte extends the preceding level by
 * exactly 127 units.
 */

#ifndef CMT_EDGE_H
#define CMT_EDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "cmtext.h"

#define CMT_EDGE_LEP_RATE 20000u
#define CMT_EDGE_L16_RATE 62500u

extern st_CMTEXT g_cmt_edge_extension;
extern st_CMTEXT g_cmt_edge_save_extension;

/* Codec helpers are public so the byte-level format can be unit-tested
 * without going through the UI or the global CMT state. */
st_CMT_STREAM *cmt_edge_stream_from_data(const uint8_t *data,
                                         size_t size,
                                         uint32_t rate,
                                         en_CMT_STREAM_POLARITY polarity);
int cmt_edge_stream_write_file(st_CMT_STREAM *stream, const char *filename);
int cmt_edge_encode_run(int value,
                        uint64_t units,
                        uint8_t *output,
                        size_t capacity,
                        size_t *written);
uint64_t cmt_edge_ticks_to_units(uint64_t ticks, uint32_t rate, int *ok);

#ifdef __cplusplus
}
#endif

#endif /* CMT_EDGE_H */
