/**
 * @file menu_replay.cpp
 * @brief Nabídka Replay (záznam a přehrání vstupů), souborové dialogy,
 *        rewind a seek, oznámení výsledku a indikátor REC / PLAY v obraze.
 *
 * GUI jen posílá požadavky do modulu záznamu (replay.h) a čte jeho stav;
 * záznam i přehrávání řídí emu vlákno. Výsledek požadavku (chyba startu)
 * a konec záznamu / přehrávání (uloženo, konec, rozjetí) se ohlásí
 * notifikací.
 *
 * Volá jen UI vlákno.
 */

#include "main.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <glib.h>

#include "libs/imgui/imgui.h"
#include "ui-imgui/imgui_modal.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "ui-imgui/filechooser/imgui_filechooser.h"
#include "ui-imgui/bootstrap/myimgui.h"

#include "i18n.h"

#include "emulator/replay.h"
#include "snapshot/snapshot.h"
#include "topmenu.h"
#include "ui-imgui/cmt/imgui_cmt.h"

extern "C" void snapshot_notification_show_ex(const char *message, bool is_error, const char *error_title);

/** @brief Požadavek z nabídky: otevřít dialog záznamu (uložit jako). */
static bool s_record_dialog_request = false;
/** @brief Požadavek z nabídky: otevřít dialog přehrání. */
static bool s_play_dialog_request = false;
/** @brief Otevřený dialog: 0 = žádný, 1 = záznam, 2 = přehrání. */
static int s_dialog_open = 0;
/** @brief Číslo požadavku, na jehož výsledek se čeká (0 = žádný). */
static uint32_t s_pending_seq = 0;
/** @brief Naposledy ohlášený výsledek (výsledek + snímek + text = otisk). */
static char s_last_outcome_key[REPLAY_STATUS_TEXT_MAX + 64] = "";
/** @brief Požadavek z nabídky: otevřít dialog seeku. */
static bool s_seek_dialog_request = false;
/** @brief Cílový snímek v dialogu seeku. */
static int s_seek_frame = 0;


void imgui_replay_open_record_dialog(void)
{
    s_record_dialog_request = true;
}


void imgui_replay_open_play_dialog(void)
{
    /* přehrání vloží pásku záznamu: nejdřív dotaz Virtual CMT */
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_REPLAY_PLAY);
}


/**
 * @brief Dialog přehrání záznamu vstupů po dotazu Virtual CMT (bez dalšího dotazu).
 * @pre UI vlákno.
 */
extern "C" void imgui_replay_open_play_dialog_now(void)
{
    s_play_dialog_request = true;
}


/**
 * @brief Start přehrání vybraného záznamu vstupů po dotazu Virtual CMT (výsledek hlásí replay_notify()).
 * @param path Soubor .mzr.
 * @pre UI vlákno.
 */
extern "C" void imgui_replay_start_play(const char *path)
{
    s_pending_seq = replay_request_play(path, false);
}


/**
 * @brief Zastaví záznam nebo přehrání vstupů jako položka Stop Replay (výsledek hlásí replay_notify()).
 *
 * Volá ho i dotaz Virtual CMT při ukončení aplikace během přehrávání:
 * konec přehrávání vrátí pásku uživatele (replay_media_restore()).
 *
 * @pre UI vlákno.
 */
extern "C" void imgui_replay_stop_play(void)
{
    s_pending_seq = replay_request_stop();
}


void imgui_replay_request(uint32_t seq)
{
    s_pending_seq = seq;
}


void imgui_replay_rewind(void)
{
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    if (st.mode == REPLAY_MODE_OFF || st.seeking) return;
    s_pending_seq = replay_request_rewind(0);
}


void imgui_replay_shortcuts(void)
{
    ImGuiIO &io = ImGui::GetIO();
    if (!io.KeyAlt || io.KeyCtrl || !ImGui::IsKeyPressed(ImGuiKey_J, false)) return;
    if (io.KeyShift)
    {
        imgui_replay_rewind();
    }
    else
    {
        g_gui->showReplayWindow = !g_gui->showReplayWindow;
    };
}


void imgui_menu_replay(void)
{
    if (!ImGui::BeginMenu(_L("Replay")))
    {
        return;
    };
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    bool off = (st.mode == REPLAY_MODE_OFF);

    if (ImGui::MenuItem(_L("Record Inputs..."), NULL, false, off))
    {
        s_record_dialog_request = true;
    };
    ImGui::SetItemTooltip("%s", _("Record keyboard, joystick, resets and debugger writes from a start snapshot, "
                                  "to repeat the run exactly later."));
    if (ImGui::MenuItem(_L("Play Replay..."), NULL, false, off))
    {
        imgui_replay_open_play_dialog();
    };
    if (ImGui::MenuItem(_L("Stop Replay"), NULL, false, !off))
    {
        s_pending_seq = replay_request_stop();
    };

    ImGui::Separator();
    if (ImGui::MenuItem(_L("Rewind"), IMGUI_REPLAY_SHORTCUT_REWIND, false, !off && st.checkpoints > 0 && !st.seeking))
    {
        s_pending_seq = replay_request_rewind(0);
    };
    ImGui::SetItemTooltip("%s", _("Go back to the previous checkpoint (again = further back). While recording, "
                                  "the inputs after the checkpoint are dropped and the recording goes on from "
                                  "there. Not possible past a write to a disk or the tape."));
    if (ImGui::MenuItem(_L("Seek..."), NULL, false, st.mode == REPLAY_MODE_PLAYING && !st.seeking))
    {
        s_seek_frame = (int)st.frame;
        s_seek_dialog_request = true;
    };
    ImGui::SetItemTooltip("%s", _("Jump the playback to a frame: back via a checkpoint, forward at maximum speed."));
    if (ImGui::MenuItem(_L("Replay Control..."), IMGUI_REPLAY_SHORTCUT_WINDOW, g_gui->showReplayWindow))
    {
        g_gui->showReplayWindow = !g_gui->showReplayWindow;
    };

    ImGui::Separator();
    if (st.mode == REPLAY_MODE_RECORDING)
    {
        ImGui::TextDisabled(_("Recording: frame %u, %u events"), st.frame, st.events);
        if (st.rerecord_count)
        {
            ImGui::TextDisabled(_("Rewinds: %u"), st.rerecord_count);
        };
    }
    else if (st.mode == REPLAY_MODE_PLAYING)
    {
        ImGui::TextDisabled(_("Playing: frame %u / %u"), st.frame, st.frames);
        if (st.seeking)
        {
            ImGui::TextDisabled(_("Seeking to frame %u"), st.seek_target);
        };
        if (st.deviated)
        {
            ImGui::TextDisabled(_("Deviated: %s"), st.deviation);
        };
    }
    else if (st.outcome != REPLAY_OUTCOME_NONE)
    {
        ImGui::TextDisabled(_("Last: %s"), st.outcome_message);
    }
    else
    {
        ImGui::TextDisabled("%s", _("No replay"));
    };
    if (!off)
    {
        if (st.checkpoint_interval == 0)
        {
            ImGui::TextDisabled("%s", _("Checkpoints: off"));
        }
        else if (st.checkpoints)
        {
            ImGui::TextDisabled(_("Checkpoints: %u (frames %u - %u, %u kB)"), st.checkpoints, st.checkpoint_first,
                                st.checkpoint_last, (unsigned)((st.checkpoint_bytes + 1023) / 1024));
        };
    };
    ImGui::EndMenu();
}


/**
 * @brief Dialog seeku: cílový snímek, tlačítka Seek / Cancel.
 *
 * Otevře se z nabídky (s_seek_dialog_request); zavře se sám, když
 * přehrávání mezitím skončilo.
 */
static void replay_seek_dialog(void)
{
    const std::string title = std::string(_("Seek Replay")) + "###ReplaySeekPopup";
    /* požadavek čeká, dokud je otevřený jiný popup (imgui_modal.h) */
    if (s_seek_dialog_request && imgui_modal_can_open())
    {
        s_seek_dialog_request = false;
        ImGui::OpenPopup(title.c_str());
    };
    if (!ImGui::BeginPopupModal(title.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    };
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    if (st.mode != REPLAY_MODE_PLAYING)
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    };
    ImGui::Text(_("Current frame %u of %u"), st.frame, st.frames);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputInt(_L("Frame"), &s_seek_frame, 50, 500);
    if (s_seek_frame < 0) s_seek_frame = 0;
    if (s_seek_frame > (int)st.frames) s_seek_frame = (int)st.frames;
    if (ImGui::Button(_L("Seek")))
    {
        s_pending_seq = replay_request_seek((uint32_t)s_seek_frame);
        ImGui::CloseCurrentPopup();
    };
    ImGui::SameLine();
    if (ImGui::Button(_L("Cancel")))
    {
        ImGui::CloseCurrentPopup();
    };
    ImGui::EndPopup();
}


/** @brief Otevře souborový dialog záznamu (record = true) nebo přehrání. */
static void replay_open_dialog(bool record)
{
    IGFD::FileDialogConfig config;
    config.path = g_snapshot_settings.default_directory;
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal |
                   ImGuiFileDialogFlags_DontShowHiddenFiles |
                   ImGuiFileDialogFlags_ShowDevicesButton |
                   ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering;
    if (record)
    {
        config.flags |= ImGuiFileDialogFlags_ConfirmOverwrite;
    }
    else
    {
        config.flags |= ImGuiFileDialogFlags_ReadOnlyFileNameField;
    };
    imgui_filechooser_prepare_config(BASEUI_FCHOOSER_CAT_SNAPSHOT, config);
    ImGuiFileDialog::Instance()->OpenDialog(record ? "ReplayRecordDialog" : "ReplayPlayDialog",
                                            record ? _("Record Inputs") : _("Play Replay"), ".mzr", config);
    s_dialog_open = record ? 1 : 2;
}


/** @brief Ohlásí chybu vyřízeného požadavku a nový výsledek záznamu / přehrávání. */
static void replay_notify(void)
{
    st_REPLAY_STATUS st;
    replay_get_status(&st);

    if (s_pending_seq && st.request_done >= s_pending_seq)
    {
        if (!st.request_ok)
        {
            char msg[REPLAY_STATUS_TEXT_MAX + 64];
            g_snprintf(msg, sizeof(msg), "%s: %s", _("Replay failed"), st.request_message);
            snapshot_notification_show_ex(msg, true, _("Replay Error"));
        };
        s_pending_seq = 0;
    };

    if (st.outcome == REPLAY_OUTCOME_NONE || st.mode != REPLAY_MODE_OFF) return;
    char key[sizeof(s_last_outcome_key)];
    g_snprintf(key, sizeof(key), "%d:%u:%s", (int)st.outcome, st.outcome_frame, st.outcome_message);
    if (strcmp(key, s_last_outcome_key) == 0) return;
    g_strlcpy(s_last_outcome_key, key, sizeof(s_last_outcome_key));

    char msg[REPLAY_STATUS_TEXT_MAX + 64];
    switch (st.outcome)
    {
        case REPLAY_OUTCOME_RECORDED:
            g_snprintf(msg, sizeof(msg), "%s: %s", _("Replay recorded"), st.outcome_message);
            snapshot_notification_show_ex(msg, false, NULL);
            break;
        case REPLAY_OUTCOME_ENDED:
            g_snprintf(msg, sizeof(msg), "%s: %s", _("Replay ended"), st.outcome_message);
            snapshot_notification_show_ex(msg, false, NULL);
            break;
        case REPLAY_OUTCOME_DESYNC:
            g_snprintf(msg, sizeof(msg), _("Replay differs from the recording at frame %u (%s)"),
                       st.outcome_frame, st.desync_parts);
            snapshot_notification_show_ex(msg, true, _("Replay Desync"));
            break;
        case REPLAY_OUTCOME_ERROR:
            g_snprintf(msg, sizeof(msg), "%s: %s", _("Replay failed"), st.outcome_message);
            snapshot_notification_show_ex(msg, true, _("Replay Error"));
            break;
        case REPLAY_OUTCOME_STOPPED:
        default:
            break;
    };
}


void imgui_replay_dialogs(void)
{
    if (s_record_dialog_request)
    {
        s_record_dialog_request = false;
        replay_open_dialog(true);
    };
    if (s_play_dialog_request)
    {
        s_play_dialog_request = false;
        replay_open_dialog(false);
    };

    if (s_dialog_open)
    {
        const char *key = (s_dialog_open == 1) ? "ReplayRecordDialog" : "ReplayPlayDialog";
        ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);
        if (ImGuiFileDialog::Instance()->Display(key))
        {
            if (ImGuiFileDialog::Instance()->IsOk())
            {
                std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
                if (s_dialog_open == 1)
                    s_pending_seq = replay_request_record(path.c_str(), 0, NULL);
                else /* stav pásky znovu: mezi dotazem a výběrem se mohl zhoršit */
                    imgui_cmt_tape_file_action(IMGUI_CMT_TAPE_ACTION_REPLAY_PLAY_FILE, path.c_str());

            };
            ImGuiFileDialog::Instance()->Close();
            s_dialog_open = 0;
        };
    };

    replay_seek_dialog();
    replay_notify();
}


void imgui_replay_overlay(void)
{
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    if (st.mode == REPLAY_MODE_OFF) return;

    char text[128];
    if (st.mode == REPLAY_MODE_RECORDING)
    {
        g_snprintf(text, sizeof(text), _("REPLAY REC %u"), st.frame);
    }
    else if (st.seeking)
    {
        g_snprintf(text, sizeof(text), _("REPLAY SEEK %u -> %u"), st.frame, st.seek_target);
    }
    else
    {
        g_snprintf(text, sizeof(text), _("REPLAY PLAY %u / %u%s"), st.frame, st.frames,
                   st.deviated ? _(" (deviated)") : "");
    };

    /* Levý dolní roh obrazu (horní rohy patří videu, sérii snímků a zvuku). */
    ImVec2 img_min = ImGui::GetItemRectMin();
    ImVec2 img_max = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float pad = 6.0f;
    const float margin = 10.0f;
    ImVec2 ts = ImGui::CalcTextSize(text);
    float r = ts.y * 0.4f;
    float w = pad + 2.0f * r + pad + ts.x + pad;
    float h = ts.y + 2.0f * pad;
    ImVec2 box_min(img_min.x + margin, img_max.y - margin - h);
    ImVec2 box_max(img_min.x + margin + w, img_max.y - margin);
    dl->AddRectFilled(box_min, box_max, IM_COL32(0, 0, 0, 160), 4.0f);
    ImVec2 c(box_min.x + pad + r, box_min.y + h * 0.5f);
    if (st.mode == REPLAY_MODE_RECORDING)
    {
        /* Červený kruh = záznam. */
        dl->AddCircleFilled(c, r, IM_COL32(230, 40, 40, 255));
    }
    else
    {
        /* Trojúhelník = přehrávání (oranžový při odchýlení). */
        ImU32 col = st.deviated ? IM_COL32(240, 170, 40, 255) : IM_COL32(120, 230, 120, 255);
        dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y), col);
    };
    dl->AddText(ImVec2(c.x + r + pad, box_min.y + pad), IM_COL32(255, 255, 255, 255), text);
}
