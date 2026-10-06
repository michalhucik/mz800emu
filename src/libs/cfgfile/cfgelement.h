/**
 * @file   cfgelement.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Konfiguracni element — jedna polozka (klic=hodnota) v INI sekci
 *
 * Podporovane typy: KEYWORD, BOOL, TEXT, UNSIGNED, FLOAT.
 * Kazdy element obsahuje aktualni a vychozi hodnotu, volitelne callbacky
 * a vazby na aplikacni promenne (handler/pointer/bind).
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní revize a refaktorizace. Vytvořeny unit testy.
 *
 * @par Licence:
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef CFGELEMENT_H
#define CFGELEMENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

    /* Forward deklarace — element odkazuje na svuj nadrazeny modul */
    struct st_CFGMODULE;


    /** @brief Typ callbacku volaneho pri propagaci elementu */
    typedef void ( *cfgelement_propagate_cb_t ) (void *e, void *data );

    /** @brief Typ callbacku volaneho pri ukladani elementu */
    typedef void ( *cfgelement_save_cb_t ) (void *e, void *data );


    /** @brief Identifikator varianty hodnoty elementu (aktualni vs. vychozi) */
    typedef enum en_CFGELVAR {
        CFGELVAR_VALUE,         /**< Aktualni hodnota */
        CFGELVAR_DEFAULT_VALUE, /**< Vychozi hodnota */
        CFGELVAR_COUNT          /**< Pocet variant (strazni hodnota) */
    } en_CFGELVAR;


    /** @brief Typ konfiguracniho elementu */
    typedef enum en_CFGELEMENTTYPE {
        CFGENTYPE_KEYWORD = 0,  /**< Klicove slovo — mapovani text <-> int */
        CFGENTYPE_BOOL,         /**< Logicka hodnota 0/1 */
        CFGENTYPE_TEXT,         /**< Textovy retezec */
        CFGENTYPE_UNSIGNED,     /**< Bezznamenkove cele cislo s rozsahem min/max */
        CFGENTYPE_FLOAT,        /**< Cislo s plovouci radovou carkou s rozsahem min/max */
        CFGENTYPE_COUNT         /**< Pocet typu (strazni hodnota) */
    } en_CFGELEMENTTYPE;


    /** @brief Unie pro ulozeni hodnoty elementu (typ zavisi na en_CFGELEMENTTYPE) */
    typedef union un_CFGELVALUE {
        int int_val;            /**< Hodnota pro KEYWORD, BOOL */
        char *text_val;         /**< Hodnota pro TEXT */
        float float_val;        /**< Hodnota pro FLOAT */
    } un_CFGELVALUE;


    /** @brief Tabulka keyword <-> ciselna hodnota pro KEYWORD elementy */
    typedef struct st_CFGELPROPKEYWORD {
        char **keywords;        /**< Pole textovych keywordu */
        int *keyword_values;    /**< Pole odpovidajicich ciselnych hodnot */
        int words_count;        /**< Pocet polozek v tabulce */
    } st_CFGELPROPKEYWORD;


    /** @brief Rozsah min/max pro UNSIGNED elementy */
    typedef struct st_CFGELPROPUNSIGNED {
        unsigned min;           /**< Minimalni povolena hodnota */
        unsigned max;           /**< Maximalni povolena hodnota */
    } st_CFGELPROPUNSIGNED;


    /** @brief Rozsah min/max pro FLOAT elementy */
    typedef struct st_CFGELPROPFLOAT {
        float min;              /**< Minimalni povolena hodnota */
        float max;              /**< Maximalni povolena hodnota */
    } st_CFGELPROPFLOAT;


    /** @brief Struktura callbacku pro propagaci elementu */
    typedef struct st_CFGELPROPCB {
        cfgelement_propagate_cb_t exec; /**< Ukazatel na callback funkci */
        void *data;                     /**< Uzivatelska data predana callbacku */
    } st_CFGELPROPCB;


    /** @brief Struktura callbacku pro ukladani elementu */
    typedef struct st_CFGELSAVECB {
        cfgelement_save_cb_t exec;      /**< Ukazatel na callback funkci */
        void *data;                     /**< Uzivatelska data predana callbacku */
    } st_CFGELSAVECB;


    /** @brief Konfiguracni element — jedna polozka (klic=hodnota) v INI sekci */
    typedef struct st_CFGELEMENT {
        char *name;                     /**< Nazev klice v INI souboru */
        en_CFGELEMENTTYPE type;         /**< Typ elementu */

        struct st_CFGMODULE *parent;    /**< Ukazatel na nadrazeny modul */

        void *default_value;            /**< Vychozi hodnota (malloc'd, typ zavisi na type) */
        void *value;                    /**< Aktualni hodnota (malloc'd, typ zavisi na type) */

        /* Vlastnosti specificke pro dany typ elementu */
        st_CFGELPROPKEYWORD *pkw;       /**< KEYWORD: tabulka keyword <-> hodnota */
        st_CFGELPROPUNSIGNED *ppu;      /**< UNSIGNED: min/max rozsah */
        st_CFGELPROPFLOAT *ppf;         /**< FLOAT: min/max rozsah */

        st_CFGELPROPCB propagate_cb;    /**< Callback volany pri propagate */
        void *propagate_value_handler;  /**< Vazba na promennou: cil pro propagate (KEYWORD, BOOL, UNSIGNED, FLOAT — memcpy) */
        void *propagate_value_pointer;  /**< Vazba na textovy ukazatel: cil pro propagate (TEXT — alokace kopie) */

        st_CFGELSAVECB save_cb;         /**< Callback volany pred save */
        void *save_value_handler;       /**< Vazba na promennou: zdroj pro save (KEYWORD, BOOL, UNSIGNED, FLOAT — memcpy) */
        void *save_value_pointer;       /**< Vazba na textovy ukazatel: zdroj pro save (TEXT — cteni z ukazatele) */

        /*
         * Připnutá INI hodnota (viz cfgelement_pin_save_value()).
         * Invariant: save_pin_active != 0 jen u KEYWORD, BOOL a UNSIGNED
         * s nastaveným save_value_handler; pak save_pin_runtime != save_pin_ini.
         */
        int save_pin_active;            /**< 1 = save zapíše save_pin_ini, dokud proměnná drží save_pin_runtime */
        unsigned save_pin_ini;          /**< Hodnota, která má zůstat v INI (stav před přepsáním proměnné) */
        unsigned save_pin_runtime;      /**< Hodnota proměnné jen pro běh (např. ze snapshotu) */

    } st_CFGELEMENT;

    /** @brief Zkraceny typovy alias pro st_CFGELEMENT */
    typedef st_CFGELEMENT CFGELM;


    /**
     * @brief Registruje callback volany pri propagaci elementu
     * @param e Ukazatel na element
     * @param cbexec Ukazatel na callback funkci
     * @param cbdata Uzivatelska data predana callbacku
     */
    extern void cfgelement_set_propagate_cb ( st_CFGELEMENT *e, cfgelement_propagate_cb_t cbexec, void *cbdata );

    /**
     * @brief Registruje callback volany pred ulozenim elementu
     * @param e Ukazatel na element
     * @param cbexec Ukazatel na callback funkci
     * @param cbdata Uzivatelska data predana callbacku
     */
    extern void cfgelement_set_save_cb ( st_CFGELEMENT *e, cfgelement_save_cb_t cbexec, void *cbdata );

    /**
     * @brief Nastavi handlery pro vazbu na promennou (KEYWORD, BOOL, UNSIGNED, FLOAT)
     *
     * POZNAMKA: pro novy kod doporucujeme pouzit cfgelement_bind(), ktery
     * automaticky rozlisi typ elementu a zavola spravnou funkci.
     *
     * @param e Ukazatel na element
     * @param propagate_handler Cilova promenna pro propagate (nebo NULL)
     * @param save_handler Zdrojova promenna pro save (nebo NULL)
     */
    extern void cfgelement_set_handlers ( st_CFGELEMENT *e, void *propagate_handler, void *save_handler );

    /**
     * @brief Nastavi ukazatele pro vazbu na textovou promennou (TEXT)
     *
     * POZNAMKA: pro novy kod doporucujeme pouzit cfgelement_bind(), ktery
     * automaticky rozlisi typ elementu a zavola spravnou funkci.
     *
     * @param e Ukazatel na element
     * @param propagate_pointer Cilovy char** pro propagate (nebo NULL)
     * @param save_pointer Zdrojovy char** pro save (nebo NULL)
     */
    extern void cfgelement_set_pointers ( st_CFGELEMENT *e, void *propagate_pointer, void *save_pointer );

    /**
     * @brief Sjednocena vazba na promennou (obousmerny sync)
     *
     * Pro KEYWORD, BOOL, UNSIGNED, FLOAT: variable ukazuje na int/unsigned/float.
     * Pro TEXT: variable ukazuje na char* (ukazatel na retezec).
     * Stejna promenna se pouzije pro propagate i save.
     *
     * @param e Ukazatel na element
     * @param variable Ukazatel na aplikacni promennou
     */
    extern void cfgelement_bind ( st_CFGELEMENT *e, void *variable );

    /**
     * @brief Připne INI hodnotu elementu, jehož proměnná byla přepsána jen pro běh
     *
     * Řeší situaci, kdy aplikace přepíše proměnnou svázanou se save
     * handlerem hodnotou, která se nemá dostat do INI (typicky načtení
     * snapshotu). Bez připnutí by ji další cfgelement_save() zapsal.
     *
     * Po připnutí cfgelement_save() zapisuje @p ini_value, dokud proměnná
     * drží hodnotu, kterou měla v okamžiku volání. Jakmile ji aplikace
     * změní (např. uživatel v menu), připnutí se při nejbližším save zruší
     * a zapíše se aktuální hodnota proměnné.
     *
     * Opakované volání (další snapshot) zachová původní INI hodnotu, pokud
     * proměnná mezitím držela hodnotu z předchozího připnutí.
     *
     * @param e Ukazatel na element (KEYWORD, BOOL nebo UNSIGNED)
     * @param ini_value Hodnota proměnné PŘED přepsáním (= co má zůstat v INI)
     *
     * @pre Element má nastavený save_value_handler (cfgelement_set_handlers
     *      nebo cfgelement_bind) a proměnná už obsahuje novou hodnotu.
     * @post Je-li nová hodnota proměnné rovna INI hodnotě, připnutí se
     *       zruší (není co chránit).
     * @note Rozlišuje se jen hodnotou: když uživatel proměnnou změní a před
     *       uložením vrátí zpět na hodnotu jen pro běh, zůstane INI hodnota.
     * @note Pro TEXT a FLOAT, nebo bez save handleru, se nic nestane (assert
     *       v debug buildu).
     */
    extern void cfgelement_pin_save_value ( st_CFGELEMENT *e, unsigned ini_value );

    /**
     * @brief Vazba s oddelenymi promennymi pro propagate a save
     *
     * Pouziva se pri asymetricke vazbe (napr. propagate do jine promenne
     * nez save, nebo NULL pro propagate pri save-only vazbe).
     *
     * @param e Ukazatel na element
     * @param propagate_var Cilova promenna pro propagate (nebo NULL)
     * @param save_var Zdrojova promenna pro save (nebo NULL)
     */
    extern void cfgelement_bind_separate ( st_CFGELEMENT *e, void *propagate_var, void *save_var );

    /**
     * @brief Resetuje element na vychozi hodnotu
     * @param e Ukazatel na element
     */
    extern void cfgelement_reset ( st_CFGELEMENT *e );

    /**
     * @brief Nastavi aktualni hodnotu elementu typu UNSIGNED
     * @param e Ukazatel na element
     * @param unsigned_value Nova hodnota
     */
    extern void cfgelement_set_unsigned_value ( st_CFGELEMENT *e, unsigned unsigned_value );

    /**
     * @brief Nastavi aktualni hodnotu elementu typu TEXT
     * @param e Ukazatel na element
     * @param text_value Novy textovy retezec
     */
    extern void cfgelement_set_text_value ( st_CFGELEMENT *e, const char *text_value );

    /**
     * @brief Nastavi aktualni hodnotu elementu typu BOOL
     * @param e Ukazatel na element
     * @param bool_value Nova hodnota (nenulova se normalizuje na 1)
     */
    extern void cfgelement_set_bool_value ( st_CFGELEMENT *e, int bool_value );

    /**
     * @brief Nastavi aktualni hodnotu elementu typu KEYWORD
     * @param e Ukazatel na element
     * @param key_value Nova ciselna hodnota keyword
     */
    extern void cfgelement_set_keyword_value ( st_CFGELEMENT *e, int key_value );

    /**
     * @brief Nastavi aktualni hodnotu elementu typu FLOAT
     * @param e Ukazatel na element
     * @param float_value Nova hodnota
     */
    extern void cfgelement_set_float_value ( st_CFGELEMENT *e, float float_value );

    /**
     * @brief Nastavi vychozi hodnotu elementu typu UNSIGNED
     * @param e Ukazatel na element
     * @param unsigned_value Nova vychozi hodnota
     */
    extern void cfgelement_set_unsigned_default_value ( st_CFGELEMENT *e, unsigned unsigned_value );

    /**
     * @brief Nastavi vychozi hodnotu elementu typu TEXT
     * @param e Ukazatel na element
     * @param text_value Novy vychozi textovy retezec
     */
    extern void cfgelement_set_text_default_value ( st_CFGELEMENT *e, char *text_value );

    /**
     * @brief Nastavi vychozi hodnotu elementu typu BOOL
     * @param e Ukazatel na element
     * @param bool_value Nova vychozi hodnota
     */
    extern void cfgelement_set_bool_default_value ( st_CFGELEMENT *e, int bool_value );

    /**
     * @brief Nastavi vychozi hodnotu elementu typu KEYWORD
     * @param e Ukazatel na element
     * @param key_value Nova vychozi ciselna hodnota keyword
     */
    extern void cfgelement_set_keyword_default_value ( st_CFGELEMENT *e, int key_value );

    /**
     * @brief Nastavi vychozi hodnotu elementu typu FLOAT
     * @param e Ukazatel na element
     * @param float_value Nova vychozi hodnota
     */
    extern void cfgelement_set_float_default_value ( st_CFGELEMENT *e, float float_value );

    /**
     * @brief Vrati aktualni hodnotu elementu typu UNSIGNED
     * @param e Ukazatel na element
     * @return Aktualni unsigned hodnota
     */
    extern unsigned cfgelement_get_unsigned_value ( st_CFGELEMENT *e );

    /**
     * @brief Vrati aktualni hodnotu elementu typu TEXT
     * @param e Ukazatel na element
     * @return Ukazatel na aktualni textovy retezec (vlastneny elementem)
     */
    extern char* cfgelement_get_text_value ( st_CFGELEMENT *e );

    /**
     * @brief Vrati aktualni hodnotu elementu typu BOOL
     * @param e Ukazatel na element
     * @return Aktualni hodnota (0 nebo 1)
     */
    extern int cfgelement_get_bool_value ( st_CFGELEMENT *e );

    /**
     * @brief Vrati aktualni ciselnou hodnotu elementu typu KEYWORD
     * @param e Ukazatel na element
     * @return Aktualni ciselna hodnota keyword
     */
    extern int cfgelement_get_keyword_value ( st_CFGELEMENT *e );

    /**
     * @brief Vrati aktualni hodnotu elementu typu FLOAT
     * @param e Ukazatel na element
     * @return Aktualni float hodnota
     */
    extern float cfgelement_get_float_value ( st_CFGELEMENT *e );

    /**
     * @brief Vrati vychozi hodnotu elementu typu UNSIGNED
     * @param e Ukazatel na element
     * @return Vychozi unsigned hodnota
     */
    extern unsigned cfgelement_get_unsigned_default_value ( st_CFGELEMENT *e );

    /**
     * @brief Vrati vychozi hodnotu elementu typu TEXT
     * @param e Ukazatel na element
     * @return Ukazatel na vychozi textovy retezec (vlastneny elementem)
     */
    extern char* cfgelement_get_text_default_value ( st_CFGELEMENT *e );

    /**
     * @brief Vrati vychozi hodnotu elementu typu BOOL
     * @param e Ukazatel na element
     * @return Vychozi hodnota (0 nebo 1)
     */
    extern int cfgelement_get_bool_default_value ( st_CFGELEMENT *e );

    /**
     * @brief Vrati vychozi ciselnou hodnotu elementu typu KEYWORD
     * @param e Ukazatel na element
     * @return Vychozi ciselna hodnota keyword
     */
    extern int cfgelement_get_keyword_default_value ( st_CFGELEMENT *e );

    /**
     * @brief Vrati vychozi hodnotu elementu typu FLOAT
     * @param e Ukazatel na element
     * @return Vychozi float hodnota
     */
    extern float cfgelement_get_float_default_value ( st_CFGELEMENT *e );

    /**
     * @brief Vrati textovy keyword odpovidajici aktualni hodnote
     * @param e Ukazatel na element typu KEYWORD
     * @return Textovy keyword, nebo NULL pokud hodnota nema prirazeny keyword
     */
    extern char* cfgelement_get_keyword_by_value ( st_CFGELEMENT *e );

    /**
     * @brief Vrati textovy keyword odpovidajici vychozi hodnote
     * @param e Ukazatel na element typu KEYWORD
     * @return Textovy keyword, nebo NULL pokud hodnota nema prirazeny keyword
     */
    extern char* cfgelement_get_keyword_by_default_value ( st_CFGELEMENT *e );

    /**
     * @brief Vyhleda ciselnou hodnotu podle textoveho keyword (case-insensitive)
     * @param pkw Ukazatel na tabulku keyword
     * @param key_word Hledany textovy keyword
     * @return Ciselna hodnota, nebo -1 pokud keyword nebyl nalezen
     */
    extern int cfgelement_property_get_value_by_keyword ( st_CFGELPROPKEYWORD *pkw, char *key_word );

    /**
     * @brief Prida novy par keyword-hodnota do tabulky
     * @param pkw Ukazatel na tabulku keyword
     * @param key_value Ciselna hodnota
     * @param key_word Textovy keyword
     * @return 1 pri uspechu, 0 pokud keyword jiz existuje (duplicita)
     */
    extern int cfgelement_property_add_keyword ( st_CFGELPROPKEYWORD *pkw, int key_value, char *key_word );

    /**
     * @brief Otestuje, zda unsigned hodnota lezi v povolenem rozsahu min/max
     * @param ppu Ukazatel na strukturu s rozsahem
     * @param value Testovana hodnota
     * @return 0 pokud je v rozsahu, -1 pokud je mimo
     */
    extern int cfgelement_property_test_unsigned_value ( st_CFGELPROPUNSIGNED *ppu, unsigned value );

    /**
     * @brief Otestuje, zda float hodnota lezi v povolenem rozsahu min/max
     * @param ppf Ukazatel na strukturu s rozsahem
     * @param value Testovana hodnota
     * @return 0 pokud je v rozsahu, -1 pokud je mimo
     */
    extern int cfgelement_property_test_float_value ( st_CFGELPROPFLOAT *ppf, float value );

    /**
     * @brief Prenese hodnotu elementu do aplikace (pres handler/pointer/callback)
     * @param e Ukazatel na element
     */
    extern void cfgelement_propagate ( st_CFGELEMENT *e );

    /**
     * @brief Nacte hodnotu z aplikace a zapise element do INI souboru
     * @param e Ukazatel na element
     */
    extern void cfgelement_save ( st_CFGELEMENT *e );


#ifdef __cplusplus
}
#endif

#endif /* CFGELEMENT_H */
