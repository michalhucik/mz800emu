/**
 * @file test_memext_map_request.c
 * @brief Přemapování MemExt z okna MemExt Map Settings provádí emu vlákno.
 *
 * Regresní test pro souběh v okně MemExt Map Settings
 * (memext_map_window.cpp): UI vlákno dřív volalo přímo
 * memext_map_pwrite() pro všech 16 address pointů - zápis g_memext.map[]
 * a memory_reconnect_ram() (přepojení memram_read/write, na MZ-800
 * přepočet RAM fast-path tabulky CPU) souběžně s přístupy CPU do paměti
 * a s OUT E7h. Nově okno volá memext_map_request(), které hodnoty jen
 * uloží; zápis provede emu vlákno v memext_map_request_process() (volá ho
 * memext_map_request_poll() z hlavní smyčky mzarch v per-event bloku
 * a z paused smyček, s debuggerem i bez něj).
 *
 * Ověřuje se:
 *  - samotný požadavek mapu ani bus pointery nezmění (bez emu vlákna se
 *    mutace neprovede),
 *  - memext_map_request_process() požadavek provede právě jednou, novější
 *    požadavek před vyzvednutím nahradí starší,
 *  - simulované emu vlákno, které volá memext_map_request_poll(), požadavek
 *    vyzvedne a provede,
 *  - výsledek je shodný se sekvencí OUT E7h (memext_map_pwrite): LUFTNER
 *    pointy 0..15, PEHU jen sudé pointy (jeden zápis na 8 KB pár),
 *  - logika okna load -> Apply bez úprav (memext_map_get_out_values +
 *    request) mapu nezmění pro PEHU i LUFTNER a editace PEHU páru
 *    odpovídá jednomu OUT E7h (regrese: okno u PEHU plnilo raw 4 KB
 *    banky a Apply mapu rozházelo).
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <string.h>

#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"


/**
 * @brief Stav simulovaného emu vlákna.
 *
 * Vlákno ve smyčce volá memext_map_request_poll() (jako per-event blok
 * hlavní smyčky mzarch), dokud `stop` není nenulové, a počítá provedené
 * požadavky. Žije na zásobníku testu, test na vlákno čeká přes
 * g_thread_join.
 */
typedef struct {
    gint stop;   /**< 1 = ukončit smyčku (atomicky). */
} st_FAKE_EMU;


/**
 * @brief Tělo simulovaného emu vlákna.
 * @param user_data st_FAKE_EMU*
 * @return NULL
 */
static gpointer fake_emu_thread(gpointer user_data)
{
    st_FAKE_EMU *fe = (st_FAKE_EMU *)user_data;
    while (!g_atomic_int_get(&fe->stop))
    {
        memext_map_request_poll();
        g_usleep(1000);
    };
    return NULL;
}


/**
 * @brief Naplní @p values hodnotami LUFTNER banků 0x10 + i.
 * @param values Pole MEMEXT_RAW_MAP_SIZE hodnot.
 */
static void fill_luftner_values(uint8_t *values)
{
    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++)
        values[i] = (uint8_t)(0x10 + i);
}


void setUp(void)
{
    /* Identity mapa po connect (viz test_memext.c). */
    g_memext.init_luftner = MEMEXT_INIT_LUFTNER_RESET;
    memext_disconnect();
    /* Případný zbytek požadavku z předchozího testu zahodit. */
    (void)memext_map_request_process();
}

void tearDown(void)
{
    (void)memext_map_request_process();
    memext_disconnect();
}


/**
 * @brief Bez emu vlákna požadavek mapu ani bus pointery nezmění.
 */
void test_request_not_applied_without_emu_thread(void)
{
    memext_connect(MEMEXT_TYPE_LUFTNER);
    uint8_t *bus_before = g_memory.memram_read[2];

    uint8_t values[MEMEXT_RAW_MAP_SIZE];
    fill_luftner_values(values);
    memext_map_request(values);

    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++)
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i, g_memext.map[i]);
    TEST_ASSERT_EQUAL_PTR(bus_before, g_memory.memram_read[2]);
    TEST_ASSERT_TRUE(g_atomic_int_get(&g_memext_map_request_pending) != 0);
}


/**
 * @brief process() provede požadavek právě jednou včetně přepojení busu.
 */
void test_process_applies_once(void)
{
    memext_connect(MEMEXT_TYPE_LUFTNER);

    uint8_t values[MEMEXT_RAW_MAP_SIZE];
    fill_luftner_values(values);
    memext_map_request(values);

    TEST_ASSERT_TRUE(memext_map_request_process());
    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++)
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(0x10 + i), g_memext.map[i]);
    TEST_ASSERT_EQUAL_PTR(&g_memext.RAM[0x12 * MEMEXT_RAW_BANK_SIZE], g_memory.memram_read[2]);
    TEST_ASSERT_EQUAL_PTR(&g_memext.RAM[0x12 * MEMEXT_RAW_BANK_SIZE], g_memory.memram_write[2]);
    TEST_ASSERT_EQUAL_INT(0, g_atomic_int_get(&g_memext_map_request_pending));

    /* Druhé vyzvednutí nemá co provést. */
    TEST_ASSERT_FALSE(memext_map_request_process());
}


/**
 * @brief Novější požadavek před vyzvednutím nahradí starší (celá tabulka).
 */
void test_latest_request_wins(void)
{
    memext_connect(MEMEXT_TYPE_LUFTNER);

    uint8_t first[MEMEXT_RAW_MAP_SIZE];
    uint8_t second[MEMEXT_RAW_MAP_SIZE];
    fill_luftner_values(first);
    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++)
        second[i] = (uint8_t)(0x40 + i);

    memext_map_request(first);
    memext_map_request(second);

    TEST_ASSERT_TRUE(memext_map_request_process());
    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++)
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(0x40 + i), g_memext.map[i]);
    TEST_ASSERT_FALSE(memext_map_request_process());
}


/**
 * @brief Simulované emu vlákno s memext_map_request_poll() požadavek provede.
 */
void test_poll_in_emu_thread_applies(void)
{
    memext_connect(MEMEXT_TYPE_LUFTNER);

    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    uint8_t values[MEMEXT_RAW_MAP_SIZE];
    fill_luftner_values(values);
    memext_map_request(values);

    /* Čekat na vyzvednutí, nejvýš 2 s. */
    gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while (g_atomic_int_get(&g_memext_map_request_pending)
           && g_get_monotonic_time() < deadline)
        g_usleep(1000);
    /* Po vynulování příznaku ještě doběhne zápis mapy - join to zaručí. */
    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    TEST_ASSERT_EQUAL_INT(0, g_atomic_int_get(&g_memext_map_request_pending));
    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++)
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(0x10 + i), g_memext.map[i]);
    TEST_ASSERT_EQUAL_PTR(&g_memext.RAM[0x12 * MEMEXT_RAW_BANK_SIZE], g_memory.memram_read[2]);
}


/**
 * @brief Výsledek požadavku = sekvence OUT E7h (memext_map_pwrite).
 *
 * LUFTNER: pointy 0..15, bajt beze změny. PEHU: jen sudé pointy (jeden
 * OUT na 8 KB pár, hodnota maskovaná na 6 bitů); hodnoty lichých pointů
 * (zde záměrně odlišné od sudých) se ignorují.
 *
 * @param type Typ MemExt.
 */
static void check_same_as_direct_sequence(en_MEMEXT_TYPE type)
{
    uint8_t values[MEMEXT_RAW_MAP_SIZE];
    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++)
        values[i] = (uint8_t)(0x05 * i + 1);

    memext_connect(type);
    int step = (type == MEMEXT_TYPE_PEHU) ? 2 : 1;
    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i += step)
        memext_map_pwrite(i, values[i]);
    uint32_t expected[MEMEXT_RAW_MAP_SIZE];
    memcpy(expected, g_memext.map, sizeof(expected));

    memext_connect(type); /* reset na identity */
    memext_map_request(values);
    TEST_ASSERT_TRUE(memext_map_request_process());

    TEST_ASSERT_EQUAL_UINT32_ARRAY(expected, g_memext.map, MEMEXT_RAW_MAP_SIZE);
}

void test_same_as_direct_sequence_pehu(void)
{
    check_same_as_direct_sequence(MEMEXT_TYPE_PEHU);
}

void test_same_as_direct_sequence_luftner(void)
{
    check_same_as_direct_sequence(MEMEXT_TYPE_LUFTNER);
}


/**
 * @brief Logika okna MemExt Map Settings: load -> Apply bez úprav.
 *
 * Stejná sekvence jako okno: memext_map_get_out_values() (load_map_state)
 * a memext_map_request() + provedení na emu vlákně (apply_map). Mapa se
 * nesmí změnit.
 *
 * @param expected Mapa před load (a očekávaná po Apply).
 */
static void check_load_apply_keeps_map(const uint32_t *expected)
{
    uint8_t values[MEMEXT_RAW_MAP_SIZE];
    memext_map_get_out_values(values);
    memext_map_request(values);
    TEST_ASSERT_TRUE(memext_map_request_process());
    TEST_ASSERT_EQUAL_UINT32_ARRAY(expected, g_memext.map, MEMEXT_RAW_MAP_SIZE);
}


/**
 * @brief PEHU po connect (identity): okno ukáže 8 KB banky 0..7 po
 *        dvojicích a Apply bez úprav mapu nezmění.
 *
 * Regrese: okno dřív plnilo editační pole raw 4 KB bankami map[i], které
 * memext_map_pwrite() u PEHU vynásobí 2 (8 KB banka), a zapisovalo i liché
 * pointy - výsledek 2,3,6,7,...,30,31 místo 0..15.
 */
void test_window_load_apply_pehu_identity(void)
{
    memext_connect(MEMEXT_TYPE_PEHU);

    uint8_t values[MEMEXT_RAW_MAP_SIZE];
    memext_map_get_out_values(values);
    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++)
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(i / 2), values[i]);

    uint32_t expected[MEMEXT_RAW_MAP_SIZE];
    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++)
        expected[i] = (uint32_t)i;
    check_load_apply_keeps_map(expected);
}


/**
 * @brief PEHU s nastavenou mapou (OUT E7h): load -> Apply bez úprav nic
 *        nezmění, ani pro banku s horními bity masky (0x3F).
 */
void test_window_load_apply_pehu_custom(void)
{
    memext_connect(MEMEXT_TYPE_PEHU);
    memext_map_pwrite(0x2, 0x3F);
    memext_map_pwrite(0x9, 0x15); /* lichý point = pár 8/9 */

    uint32_t expected[MEMEXT_RAW_MAP_SIZE];
    memcpy(expected, g_memext.map, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT32(0x7E, expected[2]);
    TEST_ASSERT_EQUAL_UINT32(0x2B, expected[9]);

    check_load_apply_keeps_map(expected);
}


/**
 * @brief PEHU editace jako v okně: sudý řádek + zrcadlení do lichého
 *        odpovídá jednomu OUT E7h na pár.
 */
void test_window_edit_pehu_pair(void)
{
    memext_connect(MEMEXT_TYPE_PEHU);

    uint8_t values[MEMEXT_RAW_MAP_SIZE];
    memext_map_get_out_values(values);
    /* Okno: editace řádku 4 nastaví i řádek 5. */
    values[4] = 0x20;
    values[5] = 0x20;
    memext_map_request(values);
    TEST_ASSERT_TRUE(memext_map_request_process());

    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++)
    {
        uint32_t want = (uint32_t)i;
        if (i == 4) want = 0x40;
        if (i == 5) want = 0x41;
        TEST_ASSERT_EQUAL_UINT32(want, g_memext.map[i]);
    };
    TEST_ASSERT_EQUAL_PTR(&g_memext.RAM[0x41 * MEMEXT_RAW_BANK_SIZE], g_memory.memram_read[5]);
}


/**
 * @brief LUFTNER (vč. FLASH banek a lichých hodnot): load -> Apply bez
 *        úprav nic nezmění - chování LUFTNER se opravou PEHU nemění.
 */
void test_window_load_apply_luftner(void)
{
    memext_connect(MEMEXT_TYPE_LUFTNER);
    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++)
        memext_map_pwrite(i, (uint8_t)(0x81 + 3 * i));

    uint8_t values[MEMEXT_RAW_MAP_SIZE];
    memext_map_get_out_values(values);
    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++)
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(0x81 + 3 * i), values[i]);

    uint32_t expected[MEMEXT_RAW_MAP_SIZE];
    memcpy(expected, g_memext.map, sizeof(expected));
    check_load_apply_keeps_map(expected);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_request_not_applied_without_emu_thread);
    RUN_TEST(test_process_applies_once);
    RUN_TEST(test_latest_request_wins);
    RUN_TEST(test_poll_in_emu_thread_applies);
    RUN_TEST(test_same_as_direct_sequence_pehu);
    RUN_TEST(test_same_as_direct_sequence_luftner);
    RUN_TEST(test_window_load_apply_pehu_identity);
    RUN_TEST(test_window_load_apply_pehu_custom);
    RUN_TEST(test_window_edit_pehu_pair);
    RUN_TEST(test_window_load_apply_luftner);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
