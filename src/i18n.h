/*
 * i18n.h — centrální header pro lokalizaci (gettext)
 *
 * Nahrazuje lokální #define _() ve všech souborech.
 * Textová doména se odvozuje z MZARCH_NAME: "mz800emu", "mz700emu", "mz1500emu".
 */

#ifndef I18N_H
#define I18N_H

#include <libintl.h>
#include <locale.h>

/* hlavní překladové makro */
#define _(STRING) gettext(STRING)

/* značkovací makro — string se nepřekládá okamžitě, ale registruje se do .pot */
#define N_(STRING) STRING

/* kontextový překlad */
#define C_(CTX, STR) pgettext(CTX, STR)

/* pgettext() — kontextový překlad (emulace, gettext ho nemá jako makro) */
#ifndef pgettext
#define pgettext(context, msgid) \
    pgettext_impl(context "\004" msgid, msgid)

static inline const char *pgettext_impl(const char *context_msgid, const char *msgid)
{
    const char *translated = gettext(context_msgid);
    return (translated == context_msgid) ? msgid : translated;
}
#endif

/*
 * _L() — lokalizace se stabilním ImGui ID
 *
 * Aktivní ImGui prvky (Button, MenuItem, Checkbox, ...) generují interní ID
 * z textového labelu. Lokalizace mění label → mění ID → kolize a nestabilita.
 *
 * _L("Cancel") vrátí "Zrušit##Cancel" — zobrazí přeložený text, ale ID zůstane
 * stabilní (odvozené z anglického originálu).
 *
 * Použití:
 *   _L() — aktivní prvky: Button, MenuItem, Checkbox, Begin, BeginMenu, ...
 *   _()  — pasivní texty: Text, Tooltip, format stringy, ...
 *
 * Viz devdoc/lokalizace/imgui_id_stabilita.md
 */
#ifdef __cplusplus
#include <cstdio>
static inline const char* _L(const char* text) {
    /* kruhový buffer — UI je jednovláknové, 16 slotů stačí */
    static char buffers[16][512];
    static int idx = 0;
    char* buf = buffers[idx & 15];
    idx++;
    std::snprintf(buf, 512, "%s##%s", _(text), text);
    return buf;
}
#else
#include <stdio.h>
static inline const char* _L(const char* text) {
    /* kruhový buffer — UI je jednovláknové, 16 slotů stačí */
    static char buffers[16][512];
    static int idx = 0;
    char* buf = buffers[idx & 15];
    idx++;
    snprintf(buf, 512, "%s##%s", _(text), text);
    return buf;
}
#endif

/* Textová doména se odvozuje RUNTIME z g_mzhal.arch_name v i18n_init()
 * ("mz800" -> "mz800emu"). Dřívější compile-time I18N_TEXTDOMAIN makro
 * (s tichým fallbackem na "mz800emu") odstraněno - mzhal krok 7. */

/* inicializace gettext — volat jednou v main() PO sdlapp_new (potřebuje paths).
 * Implementace v src/i18n.c (potřebuje glib + sdlapp_paths). */
#ifdef __cplusplus
extern "C"
#endif
void i18n_init(void);

#endif /* I18N_H */
