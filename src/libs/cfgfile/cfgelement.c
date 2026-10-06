/**
 * @file   cfgelement.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Implementace konfiguracniho elementu (klic=hodnota)
 *
 * Obsahuje logiku pro cteni/zapis hodnot, reset, propagate/save cyklus,
 * vazbu na aplikacni promenne a spravu keyword tabulek.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>


#include "cfgelement.h"
#include "cfgcommon.h"
#include "cfgfile_internal.h"
#include "cfgmodule.h"
#include "cfgroot.h"


/**
 * @brief Vrati ukazatel na ukazatel hodnoty elementu (value nebo default_value)
 * @param e Ukazatel na element
 * @param vartype Varianta hodnoty (CFGELVAR_VALUE nebo CFGELVAR_DEFAULT_VALUE)
 * @return Ukazatel na void* (adresa pole value/default_value ve strukture)
 */
static void* cfgelement_get_variable_pointer ( st_CFGELEMENT *e, en_CFGELVAR vartype ) {

    assert ( e != NULL );
    assert ( vartype < CFGELVAR_COUNT );

    if ( vartype == CFGELVAR_VALUE ) {
        return &e->value;
    }
    if ( vartype == CFGELVAR_DEFAULT_VALUE ) {
        return &e->default_value;
    };
    return NULL;
}


/**
 * @brief Vyhleda textovy keyword podle ciselne hodnoty v tabulce
 * @param pkw Ukazatel na tabulku keyword
 * @param value Hledana ciselna hodnota
 * @return Textovy keyword, nebo NULL pokud hodnota nebyla nalezena
 */
static char* cfgelement_property_get_keyword_by_value ( st_CFGELPROPKEYWORD *pkw, int value ) {

    assert ( pkw != NULL );

    if ( pkw->keywords == NULL ) return NULL;

    int i;
    for ( i = 0; i < pkw->words_count; i++ ) {
        if ( pkw->keyword_values[i] == value ) return pkw->keywords[i];
    };
    return NULL;
}


/**
 * @brief Vyhleda ciselnou hodnotu podle textoveho keyword (case-insensitive)
 * @param pkw Ukazatel na tabulku keyword
 * @param key_word Hledany textovy keyword
 * @return Ciselna hodnota, nebo -1 pokud keyword nebyl nalezen
 */
int cfgelement_property_get_value_by_keyword ( st_CFGELPROPKEYWORD *pkw, char *key_word ) {

    assert ( pkw != NULL );

    if ( pkw->keywords == NULL ) return -1;

    int i;
    for ( i = 0; i < pkw->words_count; i++ ) {
        if ( 0 == strcasecmp ( pkw->keywords[i], key_word ) ) return pkw->keyword_values[i];
    };
    return -1;
}


/**
 * @brief Otestuje, zda unsigned hodnota lezi v povolenem rozsahu min/max
 * @param ppu Ukazatel na strukturu s rozsahem
 * @param value Testovana hodnota
 * @return 0 pokud je v rozsahu, -1 pokud je mimo
 */
int cfgelement_property_test_unsigned_value ( st_CFGELPROPUNSIGNED *ppu, unsigned value ) {

    assert ( ppu != NULL );
    if ( value < ppu->min ) return -1;
    if ( value > ppu->max ) return -1;
    return 0;
}


/**
 * @brief Otestuje, zda float hodnota lezi v povolenem rozsahu min/max
 * @param ppf Ukazatel na strukturu s rozsahem
 * @param value Testovana hodnota
 * @return 0 pokud je v rozsahu, -1 pokud je mimo
 */
int cfgelement_property_test_float_value ( st_CFGELPROPFLOAT *ppf, float value ) {

    assert ( ppf != NULL );
    if ( value < ppf->min ) return -1;
    if ( value > ppf->max ) return -1;
    return 0;
}


/*
 * Cteni aktualni hodnoty
 */

/**
 * @brief Vrati aktualni hodnotu elementu typu UNSIGNED
 * @param e Ukazatel na element
 * @return Aktualni unsigned hodnota
 */
unsigned cfgelement_get_unsigned_value ( st_CFGELEMENT *e ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_UNSIGNED );
    void **var_poi = cfgelement_get_variable_pointer ( e, CFGELVAR_VALUE );
    return *(unsigned*) *var_poi;
}


/**
 * @brief Vrati aktualni hodnotu elementu typu TEXT
 * @param e Ukazatel na element
 * @return Ukazatel na aktualni textovy retezec (vlastneny elementem)
 */
char* cfgelement_get_text_value ( st_CFGELEMENT *e ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_TEXT );
    void **var_poi = cfgelement_get_variable_pointer ( e, CFGELVAR_VALUE );
    return (char*) *var_poi;
}


/**
 * @brief Vrati aktualni hodnotu elementu typu BOOL
 * @param e Ukazatel na element
 * @return Aktualni hodnota (0 nebo 1)
 */
int cfgelement_get_bool_value ( st_CFGELEMENT *e ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_BOOL );
    void **var_poi = cfgelement_get_variable_pointer ( e, CFGELVAR_VALUE );
    return *(int*) *var_poi;
}


/**
 * @brief Vrati aktualni ciselnou hodnotu elementu typu KEYWORD
 * @param e Ukazatel na element
 * @return Aktualni ciselna hodnota keyword
 */
int cfgelement_get_keyword_value ( st_CFGELEMENT *e ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_KEYWORD );
    void **var_poi = cfgelement_get_variable_pointer ( e, CFGELVAR_VALUE );
    return *(int*) *var_poi;
}


/**
 * @brief Vrati aktualni hodnotu elementu typu FLOAT
 * @param e Ukazatel na element
 * @return Aktualni float hodnota
 */
float cfgelement_get_float_value ( st_CFGELEMENT *e ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_FLOAT );
    void **var_poi = cfgelement_get_variable_pointer ( e, CFGELVAR_VALUE );
    return *(float*) *var_poi;
}


/*
 * Cteni vychozi hodnoty
 */

/**
 * @brief Vrati vychozi hodnotu elementu typu UNSIGNED
 * @param e Ukazatel na element
 * @return Vychozi unsigned hodnota
 */
unsigned cfgelement_get_unsigned_default_value ( st_CFGELEMENT *e ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_UNSIGNED );
    void **var_poi = cfgelement_get_variable_pointer ( e, CFGELVAR_DEFAULT_VALUE );
    return *(unsigned*) *var_poi;
}


/**
 * @brief Vrati vychozi hodnotu elementu typu TEXT
 * @param e Ukazatel na element
 * @return Ukazatel na vychozi textovy retezec (vlastneny elementem)
 */
char* cfgelement_get_text_default_value ( st_CFGELEMENT *e ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_TEXT );
    void **var_poi = cfgelement_get_variable_pointer ( e, CFGELVAR_DEFAULT_VALUE );
    return (char*) *var_poi;
}


/**
 * @brief Vrati vychozi hodnotu elementu typu BOOL
 * @param e Ukazatel na element
 * @return Vychozi hodnota (0 nebo 1)
 */
int cfgelement_get_bool_default_value ( st_CFGELEMENT *e ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_BOOL );
    void **var_poi = cfgelement_get_variable_pointer ( e, CFGELVAR_DEFAULT_VALUE );
    return *(int*) *var_poi;
}


/**
 * @brief Vrati vychozi ciselnou hodnotu elementu typu KEYWORD
 * @param e Ukazatel na element
 * @return Vychozi ciselna hodnota keyword
 */
int cfgelement_get_keyword_default_value ( st_CFGELEMENT *e ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_KEYWORD );
    void **var_poi = cfgelement_get_variable_pointer ( e, CFGELVAR_DEFAULT_VALUE );
    return *(int*) *var_poi;
}


/**
 * @brief Vrati vychozi hodnotu elementu typu FLOAT
 * @param e Ukazatel na element
 * @return Vychozi float hodnota
 */
float cfgelement_get_float_default_value ( st_CFGELEMENT *e ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_FLOAT );
    void **var_poi = cfgelement_get_variable_pointer ( e, CFGELVAR_DEFAULT_VALUE );
    return *(float*) *var_poi;
}


/*
 * KEYWORD: prevod hodnoty na textovy keyword
 */

/**
 * @brief Vrati textovy keyword odpovidajici aktualni hodnote
 * @param e Ukazatel na element typu KEYWORD
 * @return Textovy keyword, nebo NULL pokud hodnota nema prirazeny keyword
 */
char* cfgelement_get_keyword_by_value ( st_CFGELEMENT *e ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_KEYWORD );
    int value = cfgelement_get_keyword_value ( e );
    return cfgelement_property_get_keyword_by_value ( e->pkw, value );
}


/**
 * @brief Vrati textovy keyword odpovidajici vychozi hodnote
 * @param e Ukazatel na element typu KEYWORD
 * @return Textovy keyword, nebo NULL pokud hodnota nema prirazeny keyword
 */
char* cfgelement_get_keyword_by_default_value ( st_CFGELEMENT *e ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_KEYWORD );
    int value = cfgelement_get_keyword_default_value ( e );
    return cfgelement_property_get_keyword_by_value ( e->pkw, value );
}


/**
 * @brief Alokuje a nastavi value, nebo default value elementu
 * @param e Ukazatel na element
 * @param vartype Varianta hodnoty (CFGELVAR_VALUE nebo CFGELVAR_DEFAULT_VALUE)
 * @param set_value Ukazatel na novou hodnotu (typ zavisi na typu elementu)
 * @return 1 pri uspechu, 0 pri chybe (napr. nenalezen keyword)
 */
static int cfgelement_set_variable ( st_CFGELEMENT *e, en_CFGELVAR vartype, void *set_value ) {

    assert ( e != NULL );
    assert ( vartype < CFGELVAR_COUNT );

    void **var_poi = cfgelement_get_variable_pointer ( e, vartype );

    if ( e->type == CFGENTYPE_KEYWORD ) {

        if ( NULL == cfgelement_property_get_keyword_by_value ( e->pkw, *(int*) set_value ) ) return 0;

        if ( *var_poi == NULL ) {
            *var_poi = malloc ( sizeof ( int ) );
            CFGCOMMON_MALLOC_ERROR ( *var_poi == NULL );
        };
        int *int_poi = *var_poi;
        *int_poi = *(int*) set_value;

    } else if ( e->type == CFGENTYPE_BOOL ) {
        if ( *var_poi == NULL ) {
            *var_poi = malloc ( sizeof ( int ) );
            CFGCOMMON_MALLOC_ERROR ( *var_poi == NULL );
        };
        int *int_poi = *var_poi;
        *int_poi = ( *(int*) set_value ) ? 1 : 0;

    } else if ( e->type == CFGENTYPE_TEXT ) {

        cfgcommon_set_text ( (char**) var_poi, *(char**) set_value );

    } else if ( e->type == CFGENTYPE_UNSIGNED ) {
        if ( *var_poi == NULL ) {
            *var_poi = malloc ( sizeof ( unsigned ) );
            CFGCOMMON_MALLOC_ERROR ( *var_poi == NULL );
        };
        unsigned *uint_poi = *var_poi;
        *uint_poi = *(unsigned*) set_value;

    } else if ( e->type == CFGENTYPE_FLOAT ) {
        if ( *var_poi == NULL ) {
            *var_poi = malloc ( sizeof ( float ) );
            CFGCOMMON_MALLOC_ERROR ( *var_poi == NULL );
        };
        float *float_poi = *var_poi;
        *float_poi = *(float*) set_value;
    };

    return 1;
}


/*
 * Nastaveni aktualni hodnoty
 */

/**
 * @brief Nastavi aktualni hodnotu elementu typu UNSIGNED
 * @param e Ukazatel na element
 * @param unsigned_value Nova hodnota
 */
void cfgelement_set_unsigned_value ( st_CFGELEMENT *e, unsigned unsigned_value ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_UNSIGNED );
    cfgelement_set_variable ( e, CFGELVAR_VALUE, (void *) &unsigned_value );
}


/**
 * @brief Nastavi aktualni hodnotu elementu typu TEXT
 * @param e Ukazatel na element
 * @param text_value Novy textovy retezec
 */
void cfgelement_set_text_value ( st_CFGELEMENT *e, const char *text_value ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_TEXT );
    cfgelement_set_variable ( e, CFGELVAR_VALUE, (void *) &text_value );
}


/**
 * @brief Nastavi aktualni hodnotu elementu typu BOOL
 * @param e Ukazatel na element
 * @param bool_value Nova hodnota (nenulova se normalizuje na 1)
 */
void cfgelement_set_bool_value ( st_CFGELEMENT *e, int bool_value ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_BOOL );
    cfgelement_set_variable ( e, CFGELVAR_VALUE, (void *) &bool_value );
}


/**
 * @brief Nastavi aktualni hodnotu elementu typu KEYWORD
 * @param e Ukazatel na element
 * @param key_value Nova ciselna hodnota keyword
 */
void cfgelement_set_keyword_value ( st_CFGELEMENT *e, int key_value ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_KEYWORD );
    int set_key_value = cfgelement_set_variable ( e, CFGELVAR_VALUE, (void *) &key_value );
    assert ( set_key_value == 1 );
    (void) set_key_value;
}


/**
 * @brief Nastavi aktualni hodnotu elementu typu FLOAT
 * @param e Ukazatel na element
 * @param float_value Nova hodnota
 */
void cfgelement_set_float_value ( st_CFGELEMENT *e, float float_value ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_FLOAT );
    cfgelement_set_variable ( e, CFGELVAR_VALUE, (void *) &float_value );
}


/*
 * Nastaveni vychozi hodnoty
 */

/**
 * @brief Nastavi vychozi hodnotu elementu typu UNSIGNED
 * @param e Ukazatel na element
 * @param unsigned_value Nova vychozi hodnota
 */
void cfgelement_set_unsigned_default_value ( st_CFGELEMENT *e, unsigned unsigned_value ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_UNSIGNED );
    cfgelement_set_variable ( e, CFGELVAR_DEFAULT_VALUE, (void *) &unsigned_value );
}


/**
 * @brief Nastavi vychozi hodnotu elementu typu TEXT
 * @param e Ukazatel na element
 * @param text_value Novy vychozi textovy retezec
 */
void cfgelement_set_text_default_value ( st_CFGELEMENT *e, char *text_value ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_TEXT );
    cfgelement_set_variable ( e, CFGELVAR_DEFAULT_VALUE, (void *) &text_value );
}


/**
 * @brief Nastavi vychozi hodnotu elementu typu BOOL
 * @param e Ukazatel na element
 * @param bool_value Nova vychozi hodnota
 */
void cfgelement_set_bool_default_value ( st_CFGELEMENT *e, int bool_value ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_BOOL );
    cfgelement_set_variable ( e, CFGELVAR_DEFAULT_VALUE, (void *) &bool_value );
}


/**
 * @brief Nastavi vychozi hodnotu elementu typu KEYWORD
 * @param e Ukazatel na element
 * @param key_value Nova vychozi ciselna hodnota keyword
 */
void cfgelement_set_keyword_default_value ( st_CFGELEMENT *e, int key_value ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_KEYWORD );
    int set_key_default_value = cfgelement_set_variable ( e, CFGELVAR_DEFAULT_VALUE, (void *) &key_value );
    assert ( set_key_default_value == 1 );
    (void) set_key_default_value;
}


/**
 * @brief Nastavi vychozi hodnotu elementu typu FLOAT
 * @param e Ukazatel na element
 * @param float_value Nova vychozi hodnota
 */
void cfgelement_set_float_default_value ( st_CFGELEMENT *e, float float_value ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_FLOAT );
    cfgelement_set_variable ( e, CFGELVAR_DEFAULT_VALUE, (void *) &float_value );
}


/**
 * @brief Prida novy par keyword-hodnota do tabulky
 * @param pkw Ukazatel na tabulku keyword
 * @param key_value Ciselna hodnota
 * @param key_word Textovy keyword
 * @return 1 pri uspechu, 0 pokud keyword jiz existuje (duplicita)
 */
int cfgelement_property_add_keyword ( st_CFGELPROPKEYWORD *pkw, int key_value, char *key_word ) {

    assert ( pkw != NULL );

    if ( -1 != cfgelement_property_get_value_by_keyword ( pkw, key_word ) ) return 0;

    unsigned words_count = pkw->words_count + 1;

    pkw->keywords = realloc ( pkw->keywords, words_count * sizeof ( char * ) );
    pkw->keyword_values = realloc ( pkw->keyword_values, words_count * sizeof ( int ) );
    CFGCOMMON_MALLOC_ERROR ( pkw->keywords == NULL );
    CFGCOMMON_MALLOC_ERROR ( pkw->keyword_values == NULL );

    pkw->keyword_values[pkw->words_count] = key_value;
    pkw->keywords[pkw->words_count] = NULL;
    cfgcommon_set_text ( (char**) &pkw->keywords[pkw->words_count], key_word );
    pkw->words_count++;

    return 1;
}


/**
 * @brief Nastavi minimalni a maximalni hranici pro element typu UNSIGNED
 * @param p Ukazatel na strukturu s rozsahem
 * @param min Minimalni povolena hodnota
 * @param max Maximalni povolena hodnota
 */
static void cfgelement_property_set_unsigned ( st_CFGELPROPUNSIGNED *p, unsigned min, unsigned max ) {
    assert ( p != NULL );
    p->min = min;
    p->max = max;
}


/**
 * @brief Nastavi minimalni a maximalni hranici pro element typu FLOAT
 * @param p Ukazatel na strukturu s rozsahem
 * @param min Minimalni povolena hodnota
 * @param max Maximalni povolena hodnota
 */
static void cfgelement_property_set_float ( st_CFGELPROPFLOAT *p, float min, float max ) {
    assert ( p != NULL );
    p->min = min;
    p->max = max;
}


/**
 * @brief Vytvori novy konfiguracni element
 *
 * Variabilni parametry:
 * - pro CFGENTYPE_KEYWORD nasleduje seznam hodnot a keywordu, hodnota -1 znaci konec seznamu
 * - pro CFGENTYPE_UNSIGNED nasleduje min a max hodnota
 * - pro CFGENTYPE_FLOAT nasleduje min a max hodnota (variadic promotion: double)
 *
 * @param parent Ukazatel na nadrazeny modul
 * @param element_name Nazev elementu (klic v INI)
 * @param type Typ elementu (KEYWORD, BOOL, TEXT, UNSIGNED, FLOAT)
 * @param args Variabilni parametry podle typu
 * @return Ukazatel na novy element (nikdy NULL — pri selhani alokace abort)
 */
st_CFGELEMENT* cfgcommon_new_element ( struct st_CFGMODULE *parent, char *element_name, en_CFGELEMENTTYPE type, va_list args ) {

    assert ( type < CFGENTYPE_COUNT );

    void *default_value_pointer = NULL;

    int default_int_value;
    char *default_text_value;
    float default_float_value;
    st_CFGELPROPKEYWORD *pkw = NULL;
    st_CFGELPROPUNSIGNED *ppu = NULL;
    st_CFGELPROPFLOAT *ppf = NULL;

    if ( type == CFGENTYPE_KEYWORD ) {

        default_int_value = va_arg ( args, int );
        default_value_pointer = &default_int_value;

        pkw = malloc ( sizeof ( st_CFGELPROPKEYWORD ) );
        CFGCOMMON_MALLOC_ERROR ( pkw == NULL );
        memset ( pkw, 0x00, sizeof ( st_CFGELPROPKEYWORD ) );

        int key_value;

        while ( -1 != ( key_value = va_arg ( args, int ) ) ) {
            char *key_word = va_arg ( args, char * );
            int add_keyword = cfgelement_property_add_keyword ( pkw, key_value, key_word );
            assert ( add_keyword == 1 );
            (void) add_keyword;
        };

    } else if ( type == CFGENTYPE_BOOL ) {

        default_int_value = va_arg ( args, int );
        default_value_pointer = &default_int_value;

    } else if ( type == CFGENTYPE_TEXT ) {

        default_text_value = va_arg ( args, char * );
        default_value_pointer = &default_text_value;

    } else if ( type == CFGENTYPE_UNSIGNED ) {

        default_int_value = va_arg ( args, int );
        default_value_pointer = &default_int_value;

        ppu = malloc ( sizeof ( st_CFGELPROPUNSIGNED ) );
        CFGCOMMON_MALLOC_ERROR ( ppu == NULL );
        memset ( ppu, 0x00, sizeof ( st_CFGELPROPUNSIGNED ) );

        unsigned min = va_arg ( args, int );
        unsigned max = va_arg ( args, int );

        cfgelement_property_set_unsigned ( ppu, min, max );

    } else if ( type == CFGENTYPE_FLOAT ) {

        /* C variadic promotion: float se automaticky konvertuje na double */
        default_float_value = (float) va_arg ( args, double );
        default_value_pointer = &default_float_value;

        ppf = malloc ( sizeof ( st_CFGELPROPFLOAT ) );
        CFGCOMMON_MALLOC_ERROR ( ppf == NULL );
        memset ( ppf, 0x00, sizeof ( st_CFGELPROPFLOAT ) );

        float min = (float) va_arg ( args, double );
        float max = (float) va_arg ( args, double );

        cfgelement_property_set_float ( ppf, min, max );

    } else {
        fprintf ( stderr, "%s():%d - Unknown element type: %d\n", __func__, __LINE__, type );
    };

    st_CFGELEMENT *e = malloc ( sizeof ( st_CFGELEMENT ) );
    CFGCOMMON_MALLOC_ERROR ( e == NULL );
    memset ( e, 0x00, sizeof ( st_CFGELEMENT ) );

    cfgcommon_set_text ( &e->name, element_name );
    e->type = type;
    e->parent = parent;

    if ( pkw != NULL ) {
        e->pkw = pkw;
    };
    if ( ppu != NULL ) {
        e->ppu = ppu;
    };
    if ( ppf != NULL ) {
        e->ppf = ppf;
    };

    int set_variable;
    set_variable = cfgelement_set_variable ( e, CFGELVAR_DEFAULT_VALUE, default_value_pointer );
    assert ( set_variable == 1 );
    (void) set_variable;
    cfgelement_set_variable ( e, CFGELVAR_VALUE, default_value_pointer );

    return e;
}


/**
 * @brief Znici tabulku keyword a uvolni vsechny jeji prostredky
 * @param pkw Ukazatel na tabulku keyword (bezpecne pro NULL)
 */
static void cfgelement_property_keyword_destroy ( st_CFGELPROPKEYWORD *pkw ) {

    if ( pkw == NULL ) return;

    if ( pkw->keyword_values != NULL ) {
        free ( pkw->keyword_values );
    };

    int i;
    for ( i = 0; i < pkw->words_count; i++ ) {
        if ( pkw->keywords[i] != NULL ) {
            free ( pkw->keywords[i] );
        };
    };

    if ( pkw->keywords != NULL ) {
        free ( pkw->keywords );
    };

    free ( pkw );
}


/**
 * @brief Znici konfiguracni element a uvolni vsechny jeho prostredky
 * @param e Ukazatel na element k zniceni (bezpecne pro NULL)
 */
void cfgcommon_destroy_element ( st_CFGELEMENT *e ) {

    if ( e == NULL ) return;

    if ( e->name != NULL ) {
        free ( e->name );
    };

    if ( e->value != NULL ) {
        free ( e->value );
    };

    if ( e->default_value != NULL ) {
        free ( e->default_value );
    };

    if ( e->type == CFGENTYPE_KEYWORD ) {
        cfgelement_property_keyword_destroy ( e->pkw );
    };

    if ( e->ppu != NULL ) {
        free ( e->ppu );
    };

    if ( e->ppf != NULL ) {
        free ( e->ppf );
    };

    free ( e );
}


/**
 * @brief Resetuje element na vychozi hodnotu
 * @param e Ukazatel na element
 */
void cfgelement_reset ( st_CFGELEMENT *e ) {
    assert ( e != NULL );
    void **default_value_pointer = cfgelement_get_variable_pointer ( e, CFGELVAR_DEFAULT_VALUE );
    if ( e->type == CFGENTYPE_TEXT ) {
        /* Pro TEXT: set_variable ocekava char** (ukazatel na ukazatel na retezec) */
        cfgelement_set_variable ( e, CFGELVAR_VALUE, default_value_pointer );
    } else {
        /* Pro ostatni typy: set_variable ocekava primy ukazatel na hodnotu (int*, unsigned*, float*) */
        cfgelement_set_variable ( e, CFGELVAR_VALUE, *default_value_pointer );
    };
}


/**
 * @brief Registruje callback volany pri propagaci elementu
 * @param e Ukazatel na element
 * @param cbexec Ukazatel na callback funkci
 * @param cbdata Uzivatelska data predana callbacku
 */
void cfgelement_set_propagate_cb ( st_CFGELEMENT *e, cfgelement_propagate_cb_t cbexec, void *cbdata ) {
    assert ( e != NULL );
    e->propagate_cb.exec = cbexec;
    e->propagate_cb.data = cbdata;
}


/**
 * @brief Registruje callback volany pred ulozenim elementu
 * @param e Ukazatel na element
 * @param cbexec Ukazatel na callback funkci
 * @param cbdata Uzivatelska data predana callbacku
 */
void cfgelement_set_save_cb ( st_CFGELEMENT *e, cfgelement_save_cb_t cbexec, void *cbdata ) {
    assert ( e != NULL );
    e->save_cb.exec = cbexec;
    e->save_cb.data = cbdata;
}


/**
 * @brief Nastavi handlery pro vazbu na promennou (KEYWORD, BOOL, UNSIGNED, FLOAT)
 * @param e Ukazatel na element
 * @param propagate_handler Cilova promenna pro propagate (nebo NULL)
 * @param save_handler Zdrojova promenna pro save (nebo NULL)
 */
void cfgelement_set_handlers ( st_CFGELEMENT *e, void *propagate_handler, void *save_handler ) {
    assert ( e != NULL );
    if ( propagate_handler != NULL ) {
        assert ( e->type != CFGENTYPE_TEXT );
    };
    e->propagate_value_handler = propagate_handler;
    e->save_value_handler = save_handler;
}


/**
 * @brief Nastavi ukazatele pro vazbu na textovou promennou (TEXT)
 * @param e Ukazatel na element
 * @param propagate_pointer Cilovy char** pro propagate (nebo NULL)
 * @param save_pointer Zdrojovy char** pro save (nebo NULL)
 */
void cfgelement_set_pointers ( st_CFGELEMENT *e, void *propagate_pointer, void *save_pointer ) {
    assert ( e != NULL );
    assert ( e->type == CFGENTYPE_TEXT );
    e->propagate_value_pointer = propagate_pointer;
    e->save_value_pointer = save_pointer;
}


/**
 * @brief Sjednocena vazba na promennou (obousmerny sync)
 *
 * Pro TEXT: nastavi pointers (propagate_value_pointer + save_value_pointer).
 * Pro ostatni typy: nastavi handlers (propagate_value_handler + save_value_handler).
 * Stejna promenna se pouzije pro oba smery (propagate i save).
 *
 * @param e Ukazatel na element
 * @param variable Ukazatel na aplikacni promennou
 */
void cfgelement_bind ( st_CFGELEMENT *e, void *variable ) {
    assert ( e != NULL );
    if ( e->type == CFGENTYPE_TEXT ) {
        cfgelement_set_pointers ( e, variable, variable );
    } else {
        cfgelement_set_handlers ( e, variable, variable );
    };
}


/**
 * @brief Vazba s oddelenymi promennymi pro propagate a save
 *
 * Umoznuje pouzit jinou promennou pro propagate a jinou pro save,
 * pripadne NULL pro jeden smer (napr. save-only vazba).
 *
 * @param e Ukazatel na element
 * @param propagate_var Cilova promenna pro propagate (nebo NULL)
 * @param save_var Zdrojova promenna pro save (nebo NULL)
 */
void cfgelement_bind_separate ( st_CFGELEMENT *e, void *propagate_var, void *save_var ) {
    assert ( e != NULL );
    if ( e->type == CFGENTYPE_TEXT ) {
        cfgelement_set_pointers ( e, propagate_var, save_var );
    } else {
        cfgelement_set_handlers ( e, propagate_var, save_var );
    };
}


/**
 * @brief Připne INI hodnotu elementu (viz deklarace v cfgelement.h)
 * @param e Ukazatel na element (KEYWORD, BOOL nebo UNSIGNED)
 * @param ini_value Hodnota proměnné před přepsáním
 */
void cfgelement_pin_save_value ( st_CFGELEMENT *e, unsigned ini_value ) {
    assert ( e != NULL );
    assert ( ( e->type == CFGENTYPE_KEYWORD ) || ( e->type == CFGENTYPE_BOOL ) || ( e->type == CFGENTYPE_UNSIGNED ) );
    assert ( e->save_value_handler != NULL );
    if ( ( e->save_value_handler == NULL ) || ( e->type == CFGENTYPE_TEXT ) || ( e->type == CFGENTYPE_FLOAT ) ) {
        return;
    };

    /* KEYWORD a BOOL čte cfgelement_save() jako int, UNSIGNED jako unsigned -
     * obojí má stejnou šířku, porovnává se bitová hodnota. */
    unsigned runtime_value = *(unsigned *) e->save_value_handler;

    /* Proměnná dosud držela hodnotu z předchozího připnutí (uživatel ji
     * nezměnil) -> v INI má zůstat původní hodnota, ne ta z minulého běhu. */
    if ( e->save_pin_active && ( ini_value == e->save_pin_runtime ) ) {
        ini_value = e->save_pin_ini;
    };

    e->save_pin_ini = ini_value;
    e->save_pin_runtime = runtime_value;
    e->save_pin_active = ( runtime_value != ini_value ) ? 1 : 0;
}


/**
 * @brief Prenese hodnotu elementu do aplikace (pres handler/pointer/callback)
 * @param e Ukazatel na element
 */
void cfgelement_propagate ( st_CFGELEMENT *e ) {
    assert ( e != NULL );

    if ( NULL != e->propagate_cb.exec ) {
        e->propagate_cb.exec ( e, e->propagate_cb.data );
    };

    if ( NULL != e->propagate_value_handler ) {
        void **value_pointer = cfgelement_get_variable_pointer ( e, CFGELVAR_VALUE );
        if ( ( e->type == CFGENTYPE_KEYWORD ) || ( e->type == CFGENTYPE_BOOL ) ) {
            memcpy ( e->propagate_value_handler, *value_pointer, sizeof ( int ) );
        } else if ( e->type == CFGENTYPE_UNSIGNED ) {
            memcpy ( e->propagate_value_handler, *value_pointer, sizeof ( unsigned ) );
        } else if ( e->type == CFGENTYPE_FLOAT ) {
            memcpy ( e->propagate_value_handler, *value_pointer, sizeof ( float ) );
        } else if ( e->type == CFGENTYPE_TEXT ) {
            fprintf ( stderr, "%s():%d - Can't propagate TEXT type element by handler\n", __func__, __LINE__ );
        } else {
            fprintf ( stderr, "%s():%d - Unknown element type: %d\n", __func__, __LINE__, e->type );
        };
    };

    if ( NULL != e->propagate_value_pointer ) {
        void **value_pointer = cfgelement_get_variable_pointer ( e, CFGELVAR_VALUE );
        if ( e->type == CFGENTYPE_TEXT ) {
            unsigned length = strlen ( (char*) *value_pointer ) + 1;
            char **dst = e->propagate_value_pointer;
            *dst = malloc ( length );
            CFGCOMMON_MALLOC_ERROR ( *dst == NULL );
            if ( length > 1 ) {
                strncpy ( *dst, (char*) *value_pointer, length );
            } else {
                (*dst)[0] = 0x00;
            };
        } else {
            fprintf ( stderr, "%s():%d - Unknown element type: %d\n", __func__, __LINE__, e->type );
        };
    };
}


/**
 * @brief Nacte hodnotu z aplikace a zapise element do INI souboru
 *
 * Je-li element připnutý (cfgelement_pin_save_value()) a proměnná stále
 * drží hodnotu jen pro běh, zapíše se připnutá INI hodnota. Změnila-li
 * se proměnná mezitím, připnutí se zruší a zapíše se její hodnota.
 *
 * @param e Ukazatel na element
 */
void cfgelement_save ( st_CFGELEMENT *e ) {
    assert ( e != NULL );

    /*
     * Priprava na save — nacteni hodnoty z aplikacni promenne
     */
    if ( NULL != e->save_cb.exec ) {
        e->save_cb.exec ( e, e->save_cb.data );
    };

    if ( NULL != e->save_value_handler ) {
        void **value_pointer = cfgelement_get_variable_pointer ( e, CFGELVAR_VALUE );

        /* Připnutá INI hodnota platí, dokud proměnná drží hodnotu jen pro
         * běh; jakmile ji aplikace změnila, připnutí se ruší. */
        int use_pin = 0;
        if ( e->save_pin_active ) {
            if ( *(unsigned *) e->save_value_handler == e->save_pin_runtime ) {
                use_pin = 1;
            } else {
                e->save_pin_active = 0;
            };
        };

        if ( ( e->type == CFGENTYPE_KEYWORD ) || ( e->type == CFGENTYPE_BOOL ) ) {
            int *dst = *value_pointer;
            int src = use_pin ? (int) e->save_pin_ini : *(int *) e->save_value_handler;
            *dst = src;

        } else if ( e->type == CFGENTYPE_UNSIGNED ) {
            unsigned *dst = *value_pointer;
            unsigned src = use_pin ? e->save_pin_ini : *(unsigned *) e->save_value_handler;
            *dst = src;

        } else if ( e->type == CFGENTYPE_FLOAT ) {
            float *dst = *value_pointer;
            float src = *(float *) e->save_value_handler;
            *dst = src;

        } else if ( e->type == CFGENTYPE_TEXT ) {
            cfgcommon_set_text ( (char**) value_pointer, (char*) e->save_value_handler );

        } else {
            fprintf ( stderr, "%s():%d - Unknown element type: %d\n", __func__, __LINE__, e->type );
            int save_unknown_element_type = 1;
            assert ( save_unknown_element_type == 0 );
            (void) save_unknown_element_type;
        };
    };

    if ( NULL != e->save_value_pointer ) {
        void **value_pointer = cfgelement_get_variable_pointer ( e, CFGELVAR_VALUE );
        if ( e->type == CFGENTYPE_TEXT ) {
            char **src = e->save_value_pointer;
            cfgcommon_set_text ( (char**) value_pointer, *src );

        } else {
            fprintf ( stderr, "%s():%d - Unknown element type: %d\n", __func__, __LINE__, e->type );
            int save_unknown_element_type = 1;
            assert ( save_unknown_element_type == 0 );
            (void) save_unknown_element_type;
        };
    };

    /*
     * Samotny zapis do INI souboru
     */
    st_CFGMODULE *m = e->parent;
    st_CFGROOT *r = m->parent;

    if ( e->type == CFGENTYPE_KEYWORD ) {

        fprintf ( r->ini_fp, "%s = %s\n", e->name, cfgelement_get_keyword_by_value ( e ) );

    } else if ( e->type == CFGENTYPE_BOOL ) {

        fprintf ( r->ini_fp, "%s = %d\n", e->name, cfgelement_get_bool_value ( e ) );

    } else if ( e->type == CFGENTYPE_TEXT ) {

        fprintf ( r->ini_fp, "%s = %s\n", e->name, cfgelement_get_text_value ( e ) );

    } else if ( e->type == CFGENTYPE_UNSIGNED ) {

        fprintf ( r->ini_fp, "%s = 0x%02x\n", e->name, cfgelement_get_unsigned_value ( e ) );

    } else if ( e->type == CFGENTYPE_FLOAT ) {

        fprintf ( r->ini_fp, "%s = %g\n", e->name, cfgelement_get_float_value ( e ) );

    } else {

        fprintf ( stderr, "%s():%d - Unknown element type: %d\n", __func__, __LINE__, e->type );
        int save_unknown_element_type = 1;
        assert ( save_unknown_element_type == 0 );
        (void) save_unknown_element_type;
    };
}
