/**
 * @file   devbus_vout.c
 * @brief  Sekundární video výstupy zařízení devbus (druhý monitor).
 *
 * Trojitý buffer bez zámku: emu vlákno kreslí do bufferu `draw`, hotový
 * snímek atomicky vymění s prostředním (`mid`, s příznakem "nový"), čtenář
 * si prostřední atomicky vymění se svým bufferem `read`. Emu vlákno tak na
 * čtenáře nikdy nečeká a čtenář čte buffer, na který emu vlákno nesahá.
 * (Plán D1 počítal s krátkým mutexem jako iface_video.h; atomická výměna
 * splní totéž bez čekání.)
 *
 * Druhý, nezávislý čtenář je emu vlákno: devbus_vout_read_last() kopíruje
 * buffer posledního předaného snímku (`last`) bez výměny bufferů, takže
 * nesahá do kanálu fetch() (okno GUI) a smí ho použít např. MCP screenshot.
 *
 * Výstup patří hostiteli: není ve snapshotu ani v hashi stavu, po načtení
 * snapshotu ho zařízení překreslí v after_load().
 *
 * @par Licence: GPLv3
 */

#include "devbus_internal.h"

#include <string.h>

#include <glib.h>


/** @brief Příznak v `mid`: prostřední buffer obsahuje snímek, který čtenář ještě nevzal. */
#define DEVBUS_VOUT_MID_NEW 4u
/** @brief Maska indexu bufferu v `mid`. */
#define DEVBUS_VOUT_MID_IDX 3u

/**
 * @brief Sekundární výstup.
 * @invariant draw, read a (mid & DEVBUS_VOUT_MID_IDX) jsou navzájem různé
 *            indexy 0..2; buf[i] má width * height bajtů.
 *            last != draw (po prvním present()); bufseq[last] = seq.
 */
struct st_DEVBUS_VOUT {
    const st_DEVBUS_VOUT_DESC *desc; /**< Popis (statický). */
    int slot;                        /**< Slot zařízení. */
    size_t size;                     /**< width * height. */
    uint8_t *buf[3];                 /**< Buffery (vlastní výstup). */
    uint32_t bufseq[3];              /**< Pořadí snímku v bufferu (0 = žádný). */
    unsigned draw;                   /**< Buffer emu vlákna. */
    unsigned mid;                    /**< Prostřední buffer | DEVBUS_VOUT_MID_NEW (atomicky). */
    unsigned read;                   /**< Buffer čtenáře. */
    unsigned last;                   /**< Buffer posledního předaného snímku (pro devbus_vout_read_last(), emu vlákno). */
    uint32_t seq;                    /**< Pořadí posledního předaného snímku (emu vlákno). */
};


/** @brief Platný identifikátor výstupu [a-z][a-z0-9_]{0,14}? */
static bool devbus_vout_valid_id(const char *s)
{
    if (!s || !(s[0] >= 'a' && s[0] <= 'z') || strlen(s) > DEVBUS_ID_MAX) return false;
    for (const char *p = s; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    }
    return true;
}


st_DEVBUS_VOUT *devbus_vout_create(int slot, const st_DEVBUS_VOUT_DESC *d)
{
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    if (!s || !d || g_devbus.started) return NULL;
    if (!devbus_vout_valid_id(d->id) || !d->title || !d->width || !d->height ||
        !d->palette || !d->palette_size)
        return NULL;
    if (s->vout_count >= DEVBUS_MAX_VOUTS) return NULL;

    st_DEVBUS_VOUT *o = g_new0(st_DEVBUS_VOUT, 1);
    o->desc = d;
    o->slot = slot;
    o->size = (size_t) d->width * d->height;
    for (int i = 0; i < 3; i++) o->buf[i] = g_malloc0(o->size);
    o->draw = 0;
    o->mid = 1;
    o->read = 2;
    o->last = 0; /* bufseq[0] = 0: zatím žádný snímek */
    s->vout[s->vout_count++] = o;
    return o;
}


void devbus_vout_release_slot(st_DEVBUS_SLOT *s)
{
    for (unsigned i = 0; i < s->vout_count; i++) {
        st_DEVBUS_VOUT *o = s->vout[i];
        for (int b = 0; b < 3; b++) g_free(o->buf[b]);
        g_free(o);
        s->vout[i] = NULL;
    }
    s->vout_count = 0;
}


uint8_t *devbus_vout_draw_buffer(st_DEVBUS_VOUT *o)
{
    return o ? o->buf[o->draw] : NULL;
}


void devbus_vout_present(st_DEVBUS_VOUT *o)
{
    if (!o) return;
    unsigned presented = o->draw;
    o->seq++;
    if (o->seq == 0) o->seq = 1; /* 0 je vyhrazená pro "žádný snímek" */
    o->bufseq[presented] = o->seq;
    o->last = presented;
    unsigned old = __atomic_exchange_n(&o->mid, presented | DEVBUS_VOUT_MID_NEW, __ATOMIC_ACQ_REL);
    o->draw = old & DEVBUS_VOUT_MID_IDX;
    /* Nový buffer kreslení začne s obsahem právě předaného snímku (jen čtení
     * bufferu, který si čtenář mezitím smí vzít). */
    memcpy(o->buf[o->draw], o->buf[presented], o->size);
}


bool devbus_vout_fetch(st_DEVBUS_VOUT *o, uint8_t *dst, size_t dst_size, uint32_t *io_seen)
{
    if (!o || !dst || !io_seen || dst_size < o->size) return false;
    if (__atomic_load_n(&o->mid, __ATOMIC_ACQUIRE) & DEVBUS_VOUT_MID_NEW) {
        unsigned old = __atomic_exchange_n(&o->mid, o->read, __ATOMIC_ACQ_REL);
        o->read = old & DEVBUS_VOUT_MID_IDX;
    }
    uint32_t seq = o->bufseq[o->read];
    if (seq == 0 || seq == *io_seen) return false;
    memcpy(dst, o->buf[o->read], o->size);
    *io_seen = seq;
    return true;
}


unsigned devbus_vout_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < g_devbus.count; i++) n += g_devbus.slot[i].vout_count;
    return n;
}


st_DEVBUS_VOUT *devbus_vout_at(unsigned i)
{
    for (unsigned s = 0; s < g_devbus.count; s++) {
        if (i < g_devbus.slot[s].vout_count) return g_devbus.slot[s].vout[i];
        i -= g_devbus.slot[s].vout_count;
    }
    return NULL;
}


const st_DEVBUS_VOUT_DESC *devbus_vout_desc(const st_DEVBUS_VOUT *o)
{
    return o ? o->desc : NULL;
}


int devbus_vout_slot(const st_DEVBUS_VOUT *o)
{
    return o ? o->slot : DEVBUS_ERR_NOT_FOUND;
}


bool devbus_vout_read_last(const st_DEVBUS_VOUT *o, uint8_t *dst, size_t dst_size, uint32_t *seq_out)
{
    if (!o || !dst || dst_size < o->size) return false;
    /* Buffer `last` emu vlákno do příštího present() nepřepisuje (kreslí do
     * `draw`, který je od něj různý) a čtenář fetch() z bufferů jen čte;
     * kopie je tedy bez závodu, pokud ji dělá emu vlákno. */
    uint32_t seq = o->bufseq[o->last];
    if (seq == 0) return false;
    memcpy(dst, o->buf[o->last], o->size);
    if (seq_out) *seq_out = seq;
    return true;
}
