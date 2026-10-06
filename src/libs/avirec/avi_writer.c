/**
 * @file   avi_writer.c
 * @brief  Implementace jednoduchého RIFF AVI zapisovače (ZMBV video + PCM audio).
 *
 * Rozložení souboru:
 * RIFF 'AVI ' { LIST 'hdrl' { avih, LIST 'strl' {strh vids, strf BITMAPINFOHEADER},
 * LIST 'strl' {strh auds, strf WAVEFORMATEX} }, LIST 'movi' { ('00dc', '01wb')* }, idx1 }.
 * Velikosti a počty v hlavičkách se doplní při avi_writer_close().
 *
 * Všechna vícebajtová pole se zapisují po bajtech v pořadí little-endian,
 * jedinou výjimkou jsou audio vzorky (fwrite přímo, předpoklad LE hostitele).
 *
 * @par Licence: GPLv3
 */

#include "avi_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <wchar.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

/** Příznak avih: soubor má index idx1. */
#define AVIF_HASINDEX       0x00000010u
/** Příznak avih: chunky jsou prokládané. */
#define AVIF_ISINTERLEAVED  0x00000100u
/** Příznak záznamu idx1: klíčový chunk. */
#define AVIIF_KEYFRAME      0x00000010u
/** Doporučená velikost vyrovnávací paměti čtečky (avih/strh dwSuggestedBufferSize). */
#define AVI_SUGGESTED_BUF   (1u << 20)

/** Záznam o jednom zapsaném snímku (video chunk + navazující audio chunk). */
typedef struct st_AVI_FRAME_REC {
    uint32_t video_off;   /**< offset hlavičky '00dc' od začátku souboru */
    uint32_t video_size;  /**< velikost video dat (bez paddingu) */
    uint32_t audio_size;  /**< velikost audio dat v bajtech */
    bool keyframe;        /**< true = video chunk je klíčový snímek */
} st_AVI_FRAME_REC;

/**
 * @brief Stav zapisovače.
 *
 * Invarianty: f je otevřený pro zápis a pozice je na konci posledního chunku;
 * recs[0..count) odpovídá chunkům v souboru v pořadí; off_* jsou offsety polí
 * k doplnění při zavření; count <= cap.
 *
 * Ownership: struktura vlastní f i recs; oboje se uvolní v avi_writer_close().
 */
struct st_AVI_WRITER {
    FILE *f;                     /**< otevřený výstupní soubor ("w+b") */
    st_AVI_WRITER_PARAMS p;      /**< kopie parametrů z avi_writer_open() */
    long off_riff_size;          /**< offset velikosti kořenového RIFF */
    long off_avih_frames;        /**< offset avih.dwTotalFrames */
    long off_vstrh_len;          /**< offset strh.dwLength video streamu */
    long off_astrh_len;          /**< offset strh.dwLength audio streamu */
    long off_movi_size;          /**< offset velikosti LIST 'movi' */
    long off_movi_fcc;           /**< offset typu 'movi'; základ pro offsety v idx1 */
    uint64_t max_bytes;          /**< efektivní limit velikosti souboru (<= AVI_WRITER_MAX_BYTES) */
    st_AVI_FRAME_REC *recs;      /**< záznamy snímků pro idx1 (dynamické pole) */
    size_t count;                /**< počet platných záznamů */
    size_t cap;                  /**< kapacita pole recs */
};

/** Zapíše 32bitové číslo little-endian. Chyby zápisu se zjišťují přes ferror(). */
static void w32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

/** Zapíše 16bitové číslo little-endian. Chyby zápisu se zjišťují přes ferror(). */
static void w16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

/** Zapíše FourCC (4 bajty z s). */
static void wfcc(FILE *f, const char *s) { fwrite(s, 1, 4, f); }

/** Zapíše hlavičku LIST a vrátí offset jeho velikosti k pozdějšímu doplnění. */
static long list_begin(FILE *f, const char *type)
{
    wfcc(f, "LIST");
    long off = ftell(f);
    w32(f, 0);
    wfcc(f, type);
    return off;
}

/** Doplní velikost LIST/RIFF podle aktuální pozice a vrátí se na ni. */
static void size_patch(FILE *f, long size_off)
{
    long end = ftell(f);
    fseek(f, size_off, SEEK_SET);
    w32(f, (uint32_t)(end - size_off - 4));
    fseek(f, end, SEEK_SET);
}

/** Přepíše 32bitové číslo na offsetu off a vrátí se na původní pozici. */
static void put_at(FILE *f, long off, uint32_t v)
{
    long end = ftell(f);
    fseek(f, off, SEEK_SET);
    w32(f, v);
    fseek(f, end, SEEK_SET);
}

/**
 * Zkrátí soubor na off bajtů (po vyprázdnění bufferu).
 * @return 0 při úspěchu, nenulové při chybě.
 */
static int file_truncate(FILE *f, long off)
{
    fflush(f);
#ifdef _WIN32
    return _chsize_s(_fileno(f), (__int64)off) == 0 ? 0 : -1;
#else
    return ftruncate(fileno(f), (off_t)off);
#endif
}

/**
 * Zapíše všechny hlavičky (RIFF, hdrl, strl video + audio, začátek LIST movi)
 * a uloží do w offsety polí k doplnění při zavření. Velikosti listů hdrl a strl
 * se doplní hned; velikost RIFF a movi až v avi_writer_close().
 * Precondition: w->f je čerstvě otevřený a w->p vyplněné. Chyby se zjišťují ferror().
 */
static void write_headers(st_AVI_WRITER *w)
{
    FILE *f = w->f;
    const st_AVI_WRITER_PARAMS *p = &w->p;
    uint16_t block_align = (uint16_t)(p->audio_channels * 2);

    wfcc(f, "RIFF"); w->off_riff_size = ftell(f); w32(f, 0); wfcc(f, "AVI ");
    long hdrl = list_begin(f, "hdrl");

    wfcc(f, "avih"); w32(f, 56);
    w32(f, (uint32_t)(1000000ull * p->fps_den / p->fps_num));
    w32(f, 0); w32(f, 0);
    w32(f, AVIF_HASINDEX | AVIF_ISINTERLEAVED);
    w->off_avih_frames = ftell(f); w32(f, 0);
    w32(f, 0); w32(f, 2); w32(f, AVI_SUGGESTED_BUF);
    w32(f, p->width); w32(f, p->height);
    w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0);

    long vstrl = list_begin(f, "strl");
    wfcc(f, "strh"); w32(f, 56);
    wfcc(f, "vids"); wfcc(f, "ZMBV"); w32(f, 0); w16(f, 0); w16(f, 0); w32(f, 0);
    w32(f, p->fps_den); w32(f, p->fps_num); w32(f, 0);
    w->off_vstrh_len = ftell(f); w32(f, 0);
    w32(f, AVI_SUGGESTED_BUF); w32(f, 0xFFFFFFFFu); w32(f, 0);
    w16(f, 0); w16(f, 0); w16(f, (uint16_t)p->width); w16(f, (uint16_t)p->height);
    wfcc(f, "strf"); w32(f, 40);
    w32(f, 40); w32(f, p->width); w32(f, p->height); w16(f, 1); w16(f, 8);
    wfcc(f, "ZMBV"); w32(f, p->width * p->height); w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0);
    size_patch(f, vstrl);

    long astrl = list_begin(f, "strl");
    wfcc(f, "strh"); w32(f, 56);
    wfcc(f, "auds"); w32(f, 0); w32(f, 0); w16(f, 0); w16(f, 0); w32(f, 0);
    w32(f, 1); w32(f, p->audio_rate); w32(f, 0);
    w->off_astrh_len = ftell(f); w32(f, 0);
    w32(f, AVI_SUGGESTED_BUF); w32(f, 0xFFFFFFFFu); w32(f, block_align);
    w16(f, 0); w16(f, 0); w16(f, 0); w16(f, 0);
    wfcc(f, "strf"); w32(f, 16);
    w16(f, 1); w16(f, (uint16_t)p->audio_channels); w32(f, p->audio_rate);
    w32(f, p->audio_rate * block_align); w16(f, block_align); w16(f, 16);
    size_patch(f, astrl);

    size_patch(f, hdrl);
    w->off_movi_size = list_begin(f, "movi");
    w->off_movi_fcc = w->off_movi_size + 4;
}

/**
 * @brief Otevře soubor pro zápis ("w+b") s cestou v UTF-8.
 *
 * Na Windows se cesta převede na UTF-16 a otevře přes `_wfopen`: úzké `fopen`
 * by cestu interpretovalo podle kódové stránky / locale procesu (ANSI, nebo
 * UTF-8 při UTF-8 locale), takže by adresář s diakritikou selhal nebo vznikl
 * soubor se zkomoleným jménem. Knihovna nesmí záviset na GLib, proto vlastní
 * převod přes MultiByteToWideChar. Není-li cesta platné UTF-8, použije se
 * `fopen` s cestou beze změny (zachování dřívějšího chování).
 *
 * @param path Cesta (nesmí být NULL).
 * @return Otevřený soubor, nebo NULL (errno podle CRT).
 */
static FILE *open_utf8(const char *path)
{
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (n > 0) {
        wchar_t *wpath = malloc((size_t)n * sizeof(wchar_t));
        if (!wpath) return NULL;
        FILE *f = NULL;
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, n) == n) {
            f = _wfopen(wpath, L"w+b");
        }
        free(wpath);
        return f;
    }
#endif
    return fopen(path, "w+b");
}

st_AVI_WRITER *avi_writer_open(const char *path, const st_AVI_WRITER_PARAMS *p)
{
    if (!path || !p || !p->width || !p->height || !p->fps_num || !p->fps_den ||
        !p->audio_rate || !p->audio_channels)
        return NULL;
    st_AVI_WRITER *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->p = *p;
    w->max_bytes = (p->max_bytes && p->max_bytes < AVI_WRITER_MAX_BYTES) ? p->max_bytes : AVI_WRITER_MAX_BYTES;
    w->f = open_utf8(path);
    if (!w->f) { free(w); return NULL; }
    write_headers(w);
    if (ferror(w->f)) { fclose(w->f); free(w); return NULL; }
    return w;
}

en_AVI_WRITER_RESULT avi_writer_write_frame(st_AVI_WRITER *w, const uint8_t *video, size_t video_size,
                                            bool keyframe, const int16_t *audio, size_t audio_frames)
{
    if (!w || !video) return AVI_WRITER_ERR_ARG;
    size_t audio_size = audio_frames * w->p.audio_channels * 2;
    uint64_t now = (uint64_t)ftell(w->f);
    /* rezerva: oba chunky s hlavičkami + padding, index všech snímků včetně tohoto a hlavička idx1 */
    uint64_t need = 8 + video_size + 1 + 8 + audio_size + (uint64_t)(w->count + 1) * 32 + 8;
    if (now + need > w->max_bytes) return AVI_WRITER_ERR_FULL;

    if (w->count == w->cap) {
        size_t ncap = w->cap ? w->cap * 2 : 1024;
        st_AVI_FRAME_REC *n = realloc(w->recs, ncap * sizeof(*n));
        if (!n) return AVI_WRITER_ERR_IO;
        w->recs = n; w->cap = ncap;
    }
    st_AVI_FRAME_REC *r = &w->recs[w->count];
    r->video_off = (uint32_t)now;
    r->video_size = (uint32_t)video_size;
    r->audio_size = (uint32_t)audio_size;
    r->keyframe = keyframe;

    wfcc(w->f, "00dc"); w32(w->f, (uint32_t)video_size);
    fwrite(video, 1, video_size, w->f);
    if (video_size & 1) fputc(0, w->f);
    wfcc(w->f, "01wb"); w32(w->f, (uint32_t)audio_size);
    if (audio_size) fwrite(audio, 1, audio_size, w->f);
    if (ferror(w->f)) return AVI_WRITER_ERR_IO;
    w->count++;
    return AVI_WRITER_OK;
}

en_AVI_WRITER_RESULT avi_writer_truncate_to_frame(st_AVI_WRITER *w, uint64_t frame_count)
{
    if (!w) return AVI_WRITER_ERR_ARG;
    if (frame_count >= w->count) return AVI_WRITER_OK;
    long off = (long)w->recs[frame_count].video_off;
    if (file_truncate(w->f, off) != 0) return AVI_WRITER_ERR_IO;
    fseek(w->f, off, SEEK_SET);
    w->count = (size_t)frame_count;
    return AVI_WRITER_OK;
}

uint64_t avi_writer_frame_count(const st_AVI_WRITER *w) { return w ? w->count : 0; }

uint64_t avi_writer_bytes(const st_AVI_WRITER *w)
{
    if (!w) return 0;
    long pos = ftell(w->f);
    return (pos > 0) ? (uint64_t)pos : 0;
}

en_AVI_WRITER_RESULT avi_writer_close(st_AVI_WRITER *w)
{
    if (!w) return AVI_WRITER_ERR_ARG;
    FILE *f = w->f;
    uint64_t audio_samples = 0;

    size_patch(f, w->off_movi_size);
    wfcc(f, "idx1"); w32(f, (uint32_t)(w->count * 2 * 16));
    for (size_t i = 0; i < w->count; i++) {
        const st_AVI_FRAME_REC *r = &w->recs[i];
        uint32_t aoff = r->video_off + 8 + r->video_size + (r->video_size & 1);
        wfcc(f, "00dc"); w32(f, r->keyframe ? AVIIF_KEYFRAME : 0);
        w32(f, r->video_off - (uint32_t)w->off_movi_fcc); w32(f, r->video_size);
        wfcc(f, "01wb"); w32(f, AVIIF_KEYFRAME);
        w32(f, aoff - (uint32_t)w->off_movi_fcc); w32(f, r->audio_size);
        audio_samples += r->audio_size / (w->p.audio_channels * 2);
    }
    size_patch(f, w->off_riff_size);
    put_at(f, w->off_avih_frames, (uint32_t)w->count);
    put_at(f, w->off_vstrh_len, (uint32_t)w->count);
    put_at(f, w->off_astrh_len, (uint32_t)audio_samples);

    en_AVI_WRITER_RESULT res = ferror(f) ? AVI_WRITER_ERR_IO : AVI_WRITER_OK;
    if (fclose(f) != 0) res = AVI_WRITER_ERR_IO;
    free(w->recs);
    free(w);
    return res;
}
