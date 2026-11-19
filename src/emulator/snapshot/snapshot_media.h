/**
 * @file snapshot_media.h
 * @brief Vložená média při načtení snapshotu: režim záznamu vstupů a zpráva o rozdílu.
 *
 * Vložené médium (disketa, Quick Disk) je vlastnost hostitele, ne stavu
 * stroje: běžné načtení snapshotu ho nemění (snap_fdc.c, snap_qdisk.c).
 * Tento modul drží to, co mají handlery médií společné:
 *  - příznak "načítá se snapshot záznamu vstupů", při kterém se handlery
 *    chovají jako před zavedením pravidla (hash `periph` záznamu zahrnuje
 *    pole média a spoléhá na to, že je načtení snapshotu vrátí);
 *  - jednu souhrnnou zprávu uživateli o tom, čím se vložená média liší od
 *    médií z doby uložení snapshotu. Řádky do ní přidávají handlery, ukazuje
 *    ji správce snapshotů po načtení všech komponent.
 *
 * Vše jen z vlákna, které načítá snapshot (vlákno emulátoru); modul nemá
 * vlastní zámky.
 *
 * License: GPLv3.
 */

#ifndef SNAPSHOT_MEDIA_H
#define SNAPSHOT_MEDIA_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Režim načtení médií ze snapshotu pro záznam vstupů.
 *
 * Výchozí (false): load komponent "fdc" a "qdisk" nechává mechanikám
 * vložená média (obraz ze souboru i prázdnou mechaniku) a při rozdílu proti
 * snapshotu se ukáže souhrnná zpráva; jen mechanika s obrazem nasazeným
 * z paměti pro přehrání si bere pole média ze snapshotu.
 * S replay_load = true se komponenty chovají jako před zavedením pravidla
 * (snap_fdc.c: `mounted` a `filename` u každé mechaniky, `readonly`
 * u mechaniky bez obrazu ze souboru; snap_qdisk.c: `status`, `readonly`
 * a `filename`) a zpráva se neukazuje. Záznam vstupů ho zapíná jen na dobu
 * snapshot_load_from_buffer() svých snapshotů.
 *
 * @param replay_load true = snapshot záznamu vstupů; false = výchozí chování.
 * @pre Vlákno emulátoru (stejné jako snapshot_load_from_buffer()).
 * @post Platí do dalšího volání; volající ho po načtení vrátí na false.
 */
extern void snapshot_media_set_replay_load(bool replay_load);

/**
 * @brief Načítá se právě snapshot záznamu vstupů?
 * @return Hodnota z posledního snapshot_media_set_replay_load().
 * @pre Vlákno emulátoru.
 */
extern bool snapshot_media_is_replay_load(void);

/**
 * @brief Začátek načítání snapshotu: zahodí řádky rozdílu, které zbyly z nedokončeného načtení.
 *
 * Volá správce snapshotů před load handlery komponent.
 *
 * @pre Vlákno emulátoru.
 * @post Rozdíl právě načítaného snapshotu je prázdný; paměť minulého
 *       rozdílu (snapshot_media_get_notice()) se nemění.
 */
extern void snapshot_media_notice_begin(void);

/**
 * @brief Vybere jména, pod kterými se médium ze snapshotu a vložené médium ukážou ve zprávě.
 *
 * Jména jsou bez cest; když se obě jména shodují a liší se jen adresář,
 * vrátí celé cesty (jinak by řádek rozdíl neukázal).
 *
 * @param snap_path Cesta ze snapshotu, NULL = mechanika byla prázdná.
 * @param now_path  Cesta vloženého média, NULL = mechanika je prázdná.
 * @param[out] was  Jméno média ze snapshotu, NULL = prázdná; volající uvolní g_free().
 * @param[out] now  Jméno vloženého média, NULL = prázdná; volající uvolní g_free().
 * @pre `was` a `now` nejsou NULL.
 */
extern void snapshot_media_notice_names(const char *snap_path, const char *now_path, char **was, char **now);

/**
 * @brief Připojí k rozdílu právě načítaného snapshotu jeden řádek.
 *
 * @param line_en Řádek anglicky (stderr, klíč pro rozpoznání opakování,
 *                snapshot_media_get_notice()); bez konce řádku.
 * @param line_tr Tentýž řádek přeložený pro okno se zprávou (baseui_translate()).
 * @pre Vlákno emulátoru, mezi snapshot_media_notice_begin() a
 *      snapshot_media_notice_show(). Řetězce se zkopírují.
 */
extern void snapshot_media_notice_add(const char *line_en, const char *line_tr);

/**
 * @brief Ukáže uživateli souhrnnou zprávu o rozdílu médií, pokud nějaký je a je jiný než minule.
 *
 * Zpráva jde do okna se zprávou (baseui_show_message_tr(), přeložená) a
 * anglicky na stderr; má jeden řádek na mechaniku s rozdílem. Stejný rozdíl
 * jako při minulém načtení se neopakuje (fronta zpráv slučuje jen shodné
 * zprávy, které ještě čekají; po potvrzení by se zpráva při Quick Load ve
 * smyčce ukazovala pořád). Načtení bez rozdílu paměť minulého rozdílu smaže.
 *
 * Volá správce snapshotů po load handlerech komponent, při načtení
 * snapshotu záznamu vstupů (snapshot_media_is_replay_load()) ne.
 *
 * @pre Vlákno emulátoru.
 * @post Rozdíl právě načítaného snapshotu je prázdný; paměť minulého
 *       rozdílu drží rozdíl tohoto načtení (NULL = bez rozdílu).
 */
extern void snapshot_media_notice_show(void);

/**
 * @brief Rozdíl médií zjištěný při posledním načtení snapshotu.
 *
 * Řádky "FDD n: <jméno ze snapshotu> (now: <jméno nyní>)" a
 * "Quick Disk: <jméno ze snapshotu> (now: <jméno nyní>)" anglicky, oddělené
 * koncem řádku; jméno bez cesty, "empty" = prázdná mechanika. Slouží testům
 * a diagnostice; uživatel dostává zprávu sám.
 *
 * @param serial Výstup: kolikrát byla zpráva o rozdílu ukázána od startu
 *               (opakované načtení se stejným rozdílem ho nezvyšuje); smí být NULL.
 * @return Rozdíl, nebo NULL, když poslední načtení (mimo záznam vstupů)
 *         žádný nenašlo. Řetězec patří modulu a platí do dalšího načtení.
 * @pre Vlákno emulátoru.
 */
extern const char *snapshot_media_get_notice(unsigned *serial);

#ifdef __cplusplus
}
#endif

#endif /* SNAPSHOT_MEDIA_H */
