#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <vector>
#include <string>

// Lokalizace
#include "i18n.h"

#include "emulator/hw-generic/cmt/cmt.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "hw-generic/cmt/cmtext_block.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "libs/zxtape/zxtape.h"

#include "UiCmt.hpp"

std::string UiCmt::getSpeedTxt(en_CMTEXT_BLOCK_TYPE bltype, en_CMTEXT_BLOCK_SPEED blspeed, en_CMTSPEED cmtspeed, bool get_rateiospeed)
{
    uint16_t base_bdspeed;

    if (blspeed == CMTEXT_BLOCK_SPEED_NONE)
    {
        return std::string("");
    };

    if (blspeed == CMTEXT_BLOCK_SPEED_DEFAULT)
    {
        return std::string(_("Default"));
    };

    if (cmtspeed_is_custom(cmtspeed))
    {
        return std::string((get_rateiospeed) ? _("Custom pulses...") : _("Custom pulses"));
    };

    if (bltype == CMTEXT_BLOCK_TYPE_MZF)
    {
        base_bdspeed = MZTAPE_DEFAULT_BDSPEED;
    }
    else if ((bltype == CMTEXT_BLOCK_TYPE_TAPHEADER) || (bltype == CMTEXT_BLOCK_TYPE_TAPDATA))
    {
        base_bdspeed = ZXTAPE_DEFAULT_BDSPEED;
    }
    else
    {
        return std::string(_("UNKNOWN"));
    };

    char buff[50];

    if (get_rateiospeed)
    {
        cmtspeed_get_ratiospeedtxt(buff, sizeof(buff), cmtspeed, base_bdspeed);
    }
    else
    {
        cmtspeed_get_speedtxt(buff, sizeof(buff), cmtspeed, base_bdspeed);
    };

    return std::string(buff);
}

std::vector<UiCmtSpeedList_t> createSpeedList(en_CMTEXT_BLOCK_TYPE bltype)
{
    const en_CMTSPEED *src_cmtspeed = nullptr;

    if (bltype == CMTEXT_BLOCK_TYPE_MZF)
    {
        src_cmtspeed = g_mztape_speed;
    }
    else if ((bltype == CMTEXT_BLOCK_TYPE_TAPHEADER) || (bltype == CMTEXT_BLOCK_TYPE_TAPDATA))
    {
        src_cmtspeed = g_zxtape_speed;
    }
    else
    {
        return std::vector<UiCmtSpeedList_t>();
    };

    std::vector<UiCmtSpeedList_t> speed_list;
    while (*src_cmtspeed != CMTSPEED_NONE)
    {
        std::string speed_txt = UiCmt::getSpeedTxt(bltype, CMTEXT_BLOCK_SPEED_SET, *src_cmtspeed, true);
        speed_list.push_back({*src_cmtspeed, speed_txt});
        src_cmtspeed++;
    };

    /* MZ záznam umí i vlastní délky pulzů (výběr otevře editor) */
    if (bltype == CMTEXT_BLOCK_TYPE_MZF)
    {
        speed_list.push_back({CMTSPEED_CUSTOM, UiCmt::getSpeedTxt(bltype, CMTEXT_BLOCK_SPEED_SET, CMTSPEED_CUSTOM, true)});
    };

    return speed_list;
}

/**
 * @brief Naformátuje délku v µs: celé číslo bez desetin, jinak nejvýš 3 desetinná místa.
 *
 * @param us Délka v µs.
 * @return Text bez koncových nul.
 */
static std::string format_us(double us)
{
    char buff[32];
    snprintf(buff, sizeof(buff), "%.3f", us);
    std::string s(buff);
    while (!s.empty() && (s.back() == '0'))
        s.pop_back();
    if (!s.empty() && (s.back() == '.'))
        s.pop_back();
    return s;
}

std::string UiCmt::getPulsesTxt(const st_MZTAPE_PULSES_LENGTH *pulses)
{
    char ratio[32];
    snprintf(ratio, sizeof(ratio), "%.2f", mztape_pulses_get_ratio(CMTTMZ_FORMATSET, pulses));
    return format_us(pulses->long_pulse.high * 1e6) + "/" + format_us(pulses->long_pulse.low * 1e6) + "/" +
           format_us(pulses->short_pulse.high * 1e6) + "/" + format_us(pulses->short_pulse.low * 1e6) +
           " \xC2\xB5s (~" + ratio + ":1)";
}

/** @brief Předvolby vlastních délek pulzů - sady hlaviček CMTSPEED UniCMT (báze hw/25-unicmt.md). */
static const UiCmtPulsesPreset_t g_uicmt_pulses_presets[] = {
    {"UniCMT 1x", {470.0, 494.0, 240.0, 278.0}},
    {"UniCMT 2x", {235.0, 247.0, 120.0, 139.0}},
    {"UniCMT 3x", {156.0, 164.0, 80.0, 92.0}},
};

const UiCmtPulsesPreset_t *UiCmt::getPulsesPresets(int *count)
{
    *count = (int)(sizeof(g_uicmt_pulses_presets) / sizeof(g_uicmt_pulses_presets[0]));
    return g_uicmt_pulses_presets;
}

std::vector<UiCmtSpeedList_t> UiCmt::getMzSpeedList(void)
{
    return createSpeedList(CMTEXT_BLOCK_TYPE_MZF);
}

std::vector<UiCmtSpeedList_t> UiCmt::getZxSpeedList(void)
{
    return createSpeedList(CMTEXT_BLOCK_TYPE_TAPHEADER);
}

std::vector<UiCmtSpeedList_t> UiCmt::getMzSpeedListDefault(void)
{
    std::vector<UiCmtSpeedList_t> speed_list = UiCmt::getMzSpeedList();
    std::vector<UiCmtSpeedList_t> default_speed_list;

    std::string speed_txt = getSpeedTxt(CMTEXT_BLOCK_TYPE_MZF, CMTEXT_BLOCK_SPEED_DEFAULT, CMTSPEED_NONE, true);
    default_speed_list.push_back({CMTSPEED_NONE, speed_txt});

    // zbytek zkopirujeme ze speed_list
    for (const auto &item : speed_list)
    {
        default_speed_list.push_back(item);
    };

    return default_speed_list;
}
