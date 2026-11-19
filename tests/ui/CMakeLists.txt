# UI testy - smoke test infrastruktury (SDL3 + ImGui + OpenGL inicializace).
# Hodnota nízká (testuje vendored knihovny), ale rychlé a užitečné při ImGui
# upgrade - detekuje rozbitý backend.

mz_add_test(ui-smoke
    TYPE     UI
    SOURCES  test_ui_smoke.cpp
    LIBS     mzlib_imgui mzlib_igfd mzlib_imwidgets
    LABELS   "ui"
)

# Memory Browser V0 hex MVP - membrowser mutant
add_subdirectory(membrowser)

# Okno Bus Map (plán D6): kompilace bus_window.cpp s reálným jádrem (devbus
# s falešným adaptérem a dummy zařízením), tři snímky bez pádu, párování
# Begin/End, kontrola kolizí ID, klik na buňku mění vybranou adresu,
# persistence klíčů. UI helpery (dbgapi, I/O Ports, BP panel) stubuje test.
mz_add_test(ui-bus-window
    TYPE     UI
    SOURCES  test_bus_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/busmap/bus_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/auto_layout.cpp
             ${CMAKE_SOURCE_DIR}/tests/peripherals/devbus_dummy.c
    LIBS     mzlib_imgui mzlib_igfd mzlib_imwidgets
    LABELS   "ui;debugger;devbus"
)
# D8: písmo aplikace (testy geometrie s 28 px) a katalogy překladů (běh v němčině).
if(TARGET test_ui-bus-window)
    target_compile_definitions(test_ui-bus-window PRIVATE
        MZTEST_FONT_DIR="${CMAKE_SOURCE_DIR}/ui_resources/imgui/fonts"
        MZTEST_LOCALE_DIR="${CMAKE_SOURCE_DIR}/src/locale"
    )
    # Druhý běh téže binárky: jen test přirozené šířky v němčině (nejdelší
    # dostupný překlad). Bez katalogu de nebo bez podpory LANGUAGE se test
    # uvnitř přeskočí (Unity IGNORE).
    add_test(NAME ui-bus-window-de
        COMMAND test_ui-bus-window
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/build-tests
    )
    if(WIN32)
        get_filename_component(_bw_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
        set_tests_properties(ui-bus-window-de PROPERTIES
            ENVIRONMENT "MZ_BUS_TEST_LANG=de;PATH=${_bw_cc_dir};$ENV{PATH}"
        )
    else()
        set_tests_properties(ui-bus-window-de PROPERTIES ENVIRONMENT "MZ_BUS_TEST_LANG=de")
    endif()
    set_tests_properties(ui-bus-window-de PROPERTIES LABELS "ui;debugger;devbus;i18n")
endif()

# Funkční testy UI logiky - video záznam (menu, dialog nastavení, REC indikátor,
# notifikace událostí, okno dálkového ovládání a tabulka zkratek), okno Replay
# Control (zkratky, vykreslení bez záznamu, při záznamu, rewindu a přehrávání)
# a okna Screenshot Control a Audio Capture Control (zkratky, vykreslení).
# Linkuje UI zdroje přímo; notifikace stubuje test.
mz_add_test(ui-functional
    TYPE     UI
    SOURCES  test_ui_functional.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/videorec/videorec_menu.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/videorec/videorec_setup_dialog.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/videorec/videorec_remote_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/topmenu/menu_replay.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/topmenu/replay_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/screenshot/screenshot_ui.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/audiocap/audio_capture_ui.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/topmenu/menu_snapshot.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/snapshot/snapshot_quicksave_handler.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/snapshot/snapshot_request_ui.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/filechooser/igfd_ToggleButton.cpp
    LIBS     mzlib_imgui mzlib_igfd mzlib_imwidgets
    LABELS   "ui;videorec"
)

# Dotazy Virtual CMT a okno zpráv: skutečné ImGui ve skrytém
# okně SDL + GL, dialog neuložených změn a dialog probíhajícího nahrávání se
# zprávou zařazenou ve stejném snímku, dialog zavřený jiným modálem, návrat
# souborového dialogu se zhoršeným stavem pásky, slučování stejných zpráv;
# výsledky úprav pásky cestou imgui_device_request_results().
mz_add_test(ui-cmt-dialogs
    TYPE     UI
    SOURCES  test_cmt_dialogs.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_tape_actions.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/devices/device_request_ui.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_view.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/message/message_window.cpp
    LIBS     mzlib_imgui mzlib_igfd mzlib_imwidgets
    LABELS   "ui;cmt"
)
# Bez SDL/GL (CI bez displeje) test vrátí 77: ctest ho ukáže jako přeskočený,
# ne jako úspěch.
set_tests_properties(ui-cmt-dialogs PROPERTIES SKIP_RETURN_CODE 77)

# Okno Virtual CMT: kopie stavu pro okno (cmt_view.cpp) nad skutečným
# jádrem kazety - bez pásky, MZT ve STOP / PLAY / pauze, záznam (Unsaved,
# Recorded), značka pásky, jména (Sharp MZ ASCII -> UTF-8, prázdné, 16
# znaků, cesta 200 znaků), vysunutí mezi plněními, sestavení seznamu jen
# při změně, délky bloků (otevření bez výpočtu, zásahy jen ve STOP a nejvýš
# jednou za 10 snímků, opakování odmítnutého zásahu, levná délka bloků TZX
# a 0x18 proti streamu, otisk stroje beze změny), testovací stavy podle
# konceptů v2, kreslení okna nad skutečným jádrem, deck, tabulka, režimy
# načítání, zkratky a řádek zpráv. S proměnnou MZ_CMT_SHOT_DIR snímky okna
# z testovacích stavů (mimo CTest).
mz_add_test(ui-cmt-window
    TYPE     UI
    SOURCES  test_cmt_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_view.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_analyze_dialog.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_tapeinfo_dialog.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_convert_dialog.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_check_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_tape_actions.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/devices/device_request_ui.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/ImGuiCmt.cpp
             ${CMAKE_SOURCE_DIR}/src/ui/cmt/UiCmt.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/auto_layout.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/message/message_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/topmenu/devices/menu_cmt.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/topmenu/menu_recent.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/mywidgets/imgui_MenuRadioItem.cpp
    LIBS     mzlib_imgui mzlib_igfd mzlib_imwidgets
    LABELS   "ui;cmt"
)
# Bez SDL/GL (CI bez displeje) proběhnou jen testy kopie a test vrátí 77:
# ctest ho ukáže jako přeskočený, ne jako úspěch (jako ui-cmt-dialogs).
set_tests_properties(ui-cmt-window PROPERTIES SKIP_RETURN_CODE 77)
# RUN_SERIAL: test_w3_draw_time_300_blocks měří čas kreslení na hostiteli,
# souběh s jinými testy by ho zkresloval.
set_tests_properties(ui-cmt-window PROPERTIES RUN_SERIAL TRUE)
# Okno Virtual CMT (cmt_window.cpp) - geometrie decku se
# skutečným písmem aplikace (DroidSans + Cousine 28 px) a druhý běh v němčině.
if(TARGET test_ui-cmt-window)
    target_compile_definitions(test_ui-cmt-window PRIVATE
        MZTEST_FONT_DIR="${CMAKE_SOURCE_DIR}/ui_resources/imgui/fonts"
        MZTEST_LOCALE_DIR="${CMAKE_SOURCE_DIR}/src/locale"
    )
    # Druhý běh téže binárky: jen geometrie nového okna v němčině (všechny
    # testovací stavy, měřítka 1,0 a 1,75, texty běžné i o 40 % delší).
    add_test(NAME ui-cmt-window-de
        COMMAND test_ui-cmt-window
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/build-tests
    )
    if(WIN32)
        get_filename_component(_cw_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
        set_tests_properties(ui-cmt-window-de PROPERTIES
            ENVIRONMENT "MZ_CMT_TEST_LANG=de;PATH=${_cw_cc_dir};$ENV{PATH}"
        )
    else()
        set_tests_properties(ui-cmt-window-de PROPERTIES ENVIRONMENT "MZ_CMT_TEST_LANG=de")
    endif()
    set_tests_properties(ui-cmt-window-de PROPERTIES LABELS "ui;cmt;i18n" SKIP_RETURN_CODE 77)
    # MZ_CMT_SHOT_DIR exportovaná ve shellu vývojáře by v CTest přepnula obě
    # registrace do režimu snímků (testy by neběžely a vrátily 0): prázdná
    # hodnota = režim snímků vypnutý.
    set_property(TEST ui-cmt-window ui-cmt-window-de APPEND PROPERTY ENVIRONMENT "MZ_CMT_SHOT_DIR=")
endif()

# Dialog Analyze recording okna Virtual CMT: okno nad skutečným
# jádrem kazety a frontou zásahů, analýza v řízeném "vlákně" testu
# (syntetický výsledek analyzátoru), vstupy a zákazy, texty a rozvržení
# podle SPEC, Add / Replace se skutečným uplatněním, zastaralé výsledky,
# opuštění běžící analýzy (Cancel, Escape, zavření okna, ukončení aplikace),
# změna pásky, nic nenalezeno, chyba, histogram, data zásahu uplatnění po
# zavření dialogu, skutečné vlákno GLib, geometrie při 100 % a 175 %.
# S proměnnou MZ_CMT_SHOT_DIR snímky sady j4 (mimo CTest).
mz_add_test(ui-cmt-analyze
    TYPE     UI
    SOURCES  test_cmt_analyze.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_analyze_dialog.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_tapeinfo_dialog.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_convert_dialog.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_check_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_view.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_tape_actions.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/devices/device_request_ui.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/ImGuiCmt.cpp
             ${CMAKE_SOURCE_DIR}/src/ui/cmt/UiCmt.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/auto_layout.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/message/message_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/topmenu/devices/menu_cmt.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/topmenu/menu_recent.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/mywidgets/imgui_MenuRadioItem.cpp
    LIBS     mzlib_imgui mzlib_igfd mzlib_imwidgets
    LABELS   "ui;cmt"
)
set_tests_properties(ui-cmt-analyze PROPERTIES SKIP_RETURN_CODE 77)
if(TARGET test_ui-cmt-analyze)
    target_compile_definitions(test_ui-cmt-analyze PRIVATE
        MZTEST_FONT_DIR="${CMAKE_SOURCE_DIR}/ui_resources/imgui/fonts"
    )
    set_property(TEST ui-cmt-analyze APPEND PROPERTY ENVIRONMENT "MZ_CMT_SHOT_DIR=")
endif()

# Nástroje nad páskou okna Virtual CMT: modál Tape information,
# nemodální okno Tape check, modál Convert to profile a nápověda po otevření
# pásky s jediným surovým blokem, nad skutečným jádrem kazety a frontou
# zásahů; vstupy a zákazy, formulář (OK beze změny nic nepošle, zápis,
# varování, změna pásky), nálezy kontroly (klik vybere blok, zastaralé
# nálezy), převod (rychlost jen u profilu, který ji přijímá), geometrie při
# 100 % a 175 %. S proměnnou MZ_CMT_SHOT_DIR snímky sady j5 (mimo CTest).
mz_add_test(ui-cmt-tools
    TYPE     UI
    SOURCES  test_cmt_tools.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_analyze_dialog.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_tapeinfo_dialog.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_convert_dialog.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_check_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_view.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_tape_actions.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/devices/device_request_ui.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/ImGuiCmt.cpp
             ${CMAKE_SOURCE_DIR}/src/ui/cmt/UiCmt.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/auto_layout.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/message/message_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/topmenu/devices/menu_cmt.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/topmenu/menu_recent.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/mywidgets/imgui_MenuRadioItem.cpp
    LIBS     mzlib_imgui mzlib_igfd mzlib_imwidgets
    LABELS   "ui;cmt"
)
set_tests_properties(ui-cmt-tools PROPERTIES SKIP_RETURN_CODE 77)
if(TARGET test_ui-cmt-tools)
    target_compile_definitions(test_ui-cmt-tools PRIVATE
        MZTEST_FONT_DIR="${CMAKE_SOURCE_DIR}/ui_resources/imgui/fonts"
    )
    set_property(TEST ui-cmt-tools APPEND PROPERTY ENVIRONMENT "MZ_CMT_SHOT_DIR=")
endif()

# Overlay menu (src/ui-imgui/overlay/): kopie stavu nad skutečným jádrem,
# otevření a zavření, tři snímky bez pádu, klávesy (F10, Esc, číslice,
# Ctrl+Tab) a myš, pravidlo klávesnice emulovaného stroje s náhradou
# hlavního okna, pauza se zapamatováním stavu (dbg_ui_pause / dbg_ui_run
# stubuje test); vizuální systém a widgety: jednotka, kontrasty z tokenů,
# geometrie ve třech rozměrech okna, ovládání myší a klávesnicí, výpustka,
# náhradní písmo; tab Media a tabulka akcí: kopie stavu nad skutečným
# jádrem (mechaniky, kazeta přes cmt_view.cpp, Quick Disk), testovací stavy,
# geometrie, požadavky tlačítek a zkratek ve frontách jádra, zakázané karty,
# obnova pauzy při selhání; nedávná média a výsuvný list výměny média:
# kopie historie jádra, geometrie listu, číslice, Enter, filtr, Esc,
# vypnuté zkratky, vložení do správné mechaniky, chybějící soubor, pruh
# nedávných médií, modální dialog nad overlayem; krok emulace mezi dvěma
# snímky (číslo běhu jádra) a toast potvrzení nad overlayem; taby Machine
# a Display: kopie stavu, geometrie, prvky volají funkce klasického menu,
# rozbalovací výběr myší i klávesnicí, nové zkratky. S proměnnou
# MZ_OVERLAY_SHOT_DIR snímky tabů a galerie widgetů, s
# MZ_OVERLAY_SHOT_SET=media stavy tabu Media a s MZ_OVERLAY_SHOT_SET=sheet
# výsuvný list (mimo CTest).
mz_add_test(ui-overlay
    TYPE     UI
    SOURCES  test_overlay.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/overlay/overlay.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/overlay/overlay_view.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/overlay/overlay_style.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/overlay/overlay_widgets.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/overlay/overlay_actions.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/overlay/overlay_tab_media.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/overlay/overlay_tab_machine.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/overlay/overlay_tab_display.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/overlay/overlay_tab_util.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/overlay/overlay_tab_sound.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/overlay/overlay_tab_input.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/overlay/overlay_tab_capture.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/overlay/overlay_insert_sheet.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/cmt/cmt_view.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/snapshot/snapshot_notification.cpp
    LIBS     mzlib_imgui mzlib_igfd mzlib_imwidgets
    LABELS   "ui;overlay"
)
# Bez SDL/GL (CI bez displeje) proběhnou jen testy bez kreslení a test vrátí
# 77: ctest ho ukáže jako přeskočený, ne jako úspěch.
set_tests_properties(ui-overlay PROPERTIES SKIP_RETURN_CODE 77)
if(TARGET test_ui-overlay)
    target_compile_definitions(test_ui-overlay PRIVATE
        MZTEST_FONT_DIR="${CMAKE_SOURCE_DIR}/ui_resources/imgui/fonts"
        MZTEST_LOCALE_DIR="${CMAKE_SOURCE_DIR}/src/locale"
    )
    # Další běhy téže binárky: jen geometrie rámce, tabů Media, Machine a
    # Display, výsuvného listu a rozbalovacího výběru v jiném jazyce.
    # Němčina má dlouhé texty; čeština hlídá horní lištu v okně 640 x 480
    # (popisek aktivního tabu a štítek pauzy se v ní překrývaly se šestým
    # tabem, než lišta dostala další stupně ústupu).
    foreach(_ov_lang de cs)
        add_test(NAME ui-overlay-${_ov_lang}
            COMMAND test_ui-overlay
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/build-tests
        )
        if(WIN32)
            get_filename_component(_ov_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
            set_tests_properties(ui-overlay-${_ov_lang} PROPERTIES
                ENVIRONMENT "MZ_OVERLAY_TEST_LANG=${_ov_lang};PATH=${_ov_cc_dir};$ENV{PATH}"
            )
        else()
            set_tests_properties(ui-overlay-${_ov_lang} PROPERTIES ENVIRONMENT "MZ_OVERLAY_TEST_LANG=${_ov_lang}")
        endif()
        set_tests_properties(ui-overlay-${_ov_lang} PROPERTIES LABELS "ui;overlay;i18n" SKIP_RETURN_CODE 77)
    endforeach()
    # MZ_OVERLAY_SHOT_DIR exportovaná ve shellu vývojáře by v CTest přepnula
    # test do režimu snímků (testy by neběžely): prázdná hodnota = vypnuto.
    set_property(TEST ui-overlay ui-overlay-de ui-overlay-cs APPEND PROPERTY ENVIRONMENT "MZ_OVERLAY_SHOT_DIR=")
endif()
