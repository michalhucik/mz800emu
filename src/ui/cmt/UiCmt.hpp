#ifndef UI_CMT_HPP
#define UI_CMT_HPP

#include <vector>
#include <string>

#include "libs/cmtspeed/cmtspeed.h"
#include "libs/mztape/mztape.h"
#include "cmt/cmtext_block.h"

typedef struct UiCmtSpeedList_t
{
    en_CMTSPEED cmtspeed;
    std::string cmtspeed_txt;
} UiCmtSpeedList_t;

/**
 * @brief Předvolba vlastních délek pulzů (sada hlavičky CMTSPEED zařízení UniCMT).
 *
 * Délky v µs v pořadí hlavičky CMTSPEED: LONG high, LONG low, SHORT high,
 * SHORT low. Hodnoty souborů 1x/2x/3xspeed.mzf z USB disku UniCMT FW 0.5
 * (báze hw/25-unicmt.md, kap. 2.1).
 */
typedef struct UiCmtPulsesPreset_t
{
    const char *name; /**< zobrazovaný název (není lokalizovaný - jméno zařízení a sady) */
    double us[4];     /**< délky pulzů v µs */
} UiCmtPulsesPreset_t;

class UiCmt
{
public:
    /// Zakázání vytváření instancí.
    UiCmt() = delete;

    /// Zakázání kopírování a přiřazování.
    UiCmt(const UiCmt &) = delete;
    UiCmt &operator=(const UiCmt &) = delete;

    /**
     * @brief Text rychlosti bloku pro seznam bloků a výběr rychlosti.
     *
     * Pro CMTSPEED_CUSTOM vrátí jen obecný název ("Custom pulses"); text
     * s konkrétními délkami dává getPulsesTxt().
     */
    static std::string getSpeedTxt(en_CMTEXT_BLOCK_TYPE bltype, en_CMTEXT_BLOCK_SPEED blspeed, en_CMTSPEED cmtspeed, bool get_rateiospeed);

    /**
     * @brief Text vlastních délek pulzů, např. "156/164/80/92 µs (~3.03:1)".
     *
     * @param pulses Délky pulzů (nesmí být NULL).
     * @return Délky LONG high/low, SHORT high/low v µs (celá čísla bez
     *         desetin, jinak nejvýš 3 desetinná místa) a ekvivalentní poměr.
     */
    static std::string getPulsesTxt(const st_MZTAPE_PULSES_LENGTH *pulses);

    /**
     * @brief Předvolby vlastních délek pulzů (UniCMT 1x, 2x, 3x).
     *
     * @param[out] count Počet předvoleb.
     * @return Statické pole předvoleb.
     */
    static const UiCmtPulsesPreset_t *getPulsesPresets(int *count);

    // ziskani pole s rychlostmi pro MZ (posledni polozka je CMTSPEED_CUSTOM = "Custom pulses...")
    static std::vector<UiCmtSpeedList_t> getMzSpeedList(void);
    static std::vector<UiCmtSpeedList_t> getMzSpeedListDefault(void);

    // ziskani pole s rychlostmi pro ZX
    static std::vector<UiCmtSpeedList_t> getZxSpeedList(void);
};

#endif // UI_CMT_HPP
