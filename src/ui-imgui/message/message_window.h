#ifndef IMGUI_MESSAGE_WINDOW_H
#define IMGUI_MESSAGE_WINDOW_H

#include <stdarg.h>

typedef enum ui_message_type_t
{
    MESSAGE_TYPE_INFO = 0,
    MESSAGE_TYPE_ERROR,
    MESSAGE_TYPE_WARNING,
} ui_message_type_t;

#ifdef __cplusplus
extern "C"
{
#endif
    void imgui_message_create(ui_message_type_t type, const char *message);
    void imgui_message_vprintf(ui_message_type_t type, const char *format, va_list args);
    void imgui_message_window(void);

#ifdef MZTEST_HEADLESS
    /** @brief Jen pro testy: je popup zprávy otevřený? @return true = otevřený. */
    bool imgui_message_test_popup_open(void);
    /** @brief Jen pro testy: počet zpráv ve frontě. @return Počet. */
    unsigned imgui_message_test_queued(void);
    /** @brief Jen pro testy: počet opakování první zprávy fronty (0 = prázdná). @return Počet. */
    unsigned imgui_message_test_front_count(void);
    /**
     * @brief Jen pro testy: typ a text zprávy na pozici @p index fronty.
     * @param index Pozice (0 = zobrazovaná / první). @param[out] type Typ (smí být NULL).
     * @param[out] text Kopie textu (ukončená nulou, smí být NULL). @param size Velikost @p text.
     * @return true = pozice existuje.
     */
    bool imgui_message_test_at(unsigned index, ui_message_type_t *type, char *text, size_t size);
    /** @brief Jen pro testy: vyprázdní frontu a zapomene otevřený popup. */
    void imgui_message_test_clear(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_MESSAGE_WINDOW_H */
