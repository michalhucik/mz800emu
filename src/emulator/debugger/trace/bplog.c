/**
 * @file   bplog.c
 * @brief  Záznam výstupu akcí breakpointů - ring v paměti (viz bplog.h).
 *
 * @par Licence: GPLv3
 */

#include "bplog.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(st_BPLOG_REC) == 296, "st_BPLOG_REC musí mít 296 B (kanál bplog trace_read)");
_Static_assert(offsetof(st_BPLOG_REC, name) == 24, "rozložení st_BPLOG_REC čte trace_reader");
_Static_assert(offsetof(st_BPLOG_REC, text) == 56, "rozložení st_BPLOG_REC čte trace_reader");

/**
 * @brief Stav modulu (jen emu vlákno).
 *
 * @invariant ring == NULL => next_seq == 0.
 * @invariant segment >= 1 (kanál běží vždy).
 * @invariant first_seq = (next_seq > BPLOG_CAPACITY) ? next_seq - BPLOG_CAPACITY : 0.
 */
static struct {
    st_BPLOG_REC *ring;       /**< Ring (BPLOG_CAPACITY položek), NULL = ještě nic. */
    uint64_t next_seq;        /**< Příští seq. */
    unsigned segment;         /**< Aktuální segment (od 1). */
} s_bl = { NULL, 0, 1 };

/** @brief Čítač segmentů (monotónní i přes bplog_free(), aby starý kurzor neplatil). */
static unsigned s_segment_counter = 1;


/** @brief Nejstarší seq v ringu. */
static uint64_t bl_first(void)
{
    return (s_bl.next_seq > BPLOG_CAPACITY) ? s_bl.next_seq - BPLOG_CAPACITY : 0;
}


/**
 * @brief Zkopíruje řetězec do pole pevné délky, zbytek vynuluje.
 * @return true = řetězec se vešel celý.
 */
static bool bl_copy_field(char *dst, size_t size, const char *src, size_t *out_len)
{
    size_t n = src ? strlen(src) : 0;
    bool fits = (n < size);
    if (!fits) n = size - 1;
    memset(dst, 0, size);
    if (n) memcpy(dst, src, n);
    if (out_len) *out_len = n;
    return fits;
}


void bplog_record(en_BPLOG_KIND kind, int32_t bp_id, const char *name, uint64_t t,
                  uint32_t frame, uint16_t insn_pc, uint16_t pc, const char *text)
{
    if (!s_bl.ring) {
        /* Segment se nemění: kurzor z prázdného kanálu platí dál. */
        s_bl.ring = (st_BPLOG_REC *) malloc((size_t) BPLOG_CAPACITY * sizeof(st_BPLOG_REC));
        if (!s_bl.ring) return;
        s_bl.next_seq = 0;
    }
    st_BPLOG_REC *r = &s_bl.ring[s_bl.next_seq % BPLOG_CAPACITY];
    r->t = t;
    r->frame = frame;
    r->insn_pc = insn_pc;
    r->pc = pc;
    r->bp_id = bp_id;
    r->kind = (uint8_t) kind;
    r->flags = 0;
    size_t len = 0;
    if (!bl_copy_field(r->name, sizeof(r->name), name, NULL)) r->flags |= BPLOG_FLAG_NAME_TRUNCATED;
    if (!bl_copy_field(r->text, sizeof(r->text), text, &len)) r->flags |= BPLOG_FLAG_TEXT_TRUNCATED;
    r->text_len = (uint16_t) len;
    s_bl.next_seq++;
}


void bplog_clear(void)
{
    if (!s_bl.ring) return;
    s_bl.next_seq = 0;
    s_bl.segment = ++s_segment_counter;
}


void bplog_free(void)
{
    if (s_bl.ring) s_bl.segment = ++s_segment_counter;
    free(s_bl.ring);
    s_bl.ring = NULL;
    s_bl.next_seq = 0;
}


void bplog_get_status(st_BPLOG_STATUS *out)
{
    memset(out, 0, sizeof(*out));
    out->allocated = (s_bl.ring != NULL);
    out->segment = s_bl.segment;
    out->capacity = BPLOG_CAPACITY;
    out->first_seq = bl_first();
    out->next_seq = s_bl.next_seq;
}


uint64_t bplog_copy(uint64_t from_seq, uint64_t count, st_BPLOG_REC *dst, uint64_t *out_seq)
{
    uint64_t first = bl_first();
    uint64_t lo = (from_seq < first) ? first : from_seq;
    *out_seq = lo;
    if (!s_bl.ring || lo >= s_bl.next_seq) return 0;
    uint64_t n = s_bl.next_seq - lo;
    if (n > count) n = count;
    for (uint64_t i = 0; i < n; i++) dst[i] = s_bl.ring[(lo + i) % BPLOG_CAPACITY];
    return n;
}


const char *bplog_kind_name(unsigned kind)
{
    static const char *const names[BPLOG_KIND_COUNT] = { "log", "stop" };
    return (kind < BPLOG_KIND_COUNT) ? names[kind] : "?";
}


int bplog_kind_from_name(const char *name)
{
    if (!name) return -1;
    for (unsigned i = 0; i < BPLOG_KIND_COUNT; i++) {
        if (strcmp(name, bplog_kind_name(i)) == 0) return (int) i;
    }
    return -1;
}
