#ifndef IMGUI_CMT_H
#define IMGUI_CMT_H

#ifdef __cplusplus
extern "C"
{
#endif

    void imgui_cmt(bool *p_open);
    void imgui_cmt_fix_mzfsize_popup(bool *p_open);
    void imgui_cmt_fix_mzfsize_popup_activate(const char *filename, int valid_size, int file_size);
    void imgui_cmt_tape_index_window(bool *p_open);

    /**
     * @brief Označí seznam bloků pásky jako zastaralý (hook jádra kazety).
     * @note Libovolné vlákno; seznam přestaví imgui_cmt_tape_filelist_refresh().
     */
    void imgui_cmt_tape_update_filelist(void);

    /**
     * @brief Přestaví seznam bloků pásky, je-li označený jako zastaralý.
     * @pre UI vlákno, drží zámek stavu zařízení (device_state_lock()).
     */
    void imgui_cmt_tape_filelist_refresh(void);

    /**
     * @brief Začátek zákazu ovládání kazety během přehrávání vstupů.
     *
     * Při přehrávání ovládá kazetu záznam, okno je celé zamčené (vypíše
     * vysvětlení a zahájí ImGui::BeginDisabled()). Při záznamu se zamyká
     * jen to, co záznam nezopakuje (viz imgui_cmt_replay_recording()); tady
     * se jen vypíše vysvětlení.
     *
     * @param whole_window true = zamknout celé okno i při záznamu (varianta
     *        okna s aktivním CMT hackem, kde jde jen o přepínač hacku).
     * @return true = zákaz platí, volající musí zavolat imgui_cmt_replay_lock_end(true).
     * @pre UI vlákno, uvnitř ImGui::Begin() okna.
     */
    bool imgui_cmt_replay_lock_begin(bool whole_window);

    /**
     * @brief true = běží záznam vstupů (ne přehrávání).
     *
     * Transport kazety (play, stop, pauza, výběr bloku, nahrávání, vysunutí)
     * jde přes replay_host_cmt() a záznam ho zapíše; vložení jiné pásky,
     * rychlost, vlastní pulzy, polarita a CMT hack se při záznamu zamykají.
     */
    bool imgui_cmt_replay_recording(void);

    /**
     * @brief Konec zákazu z imgui_cmt_replay_lock_begin() (před ImGui::End()).
     * @param locked Návratová hodnota imgui_cmt_replay_lock_begin().
     */
    void imgui_cmt_replay_lock_end(bool locked);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_CMT_H */
