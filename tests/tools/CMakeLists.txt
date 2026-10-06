# tests/tools/CMakeLists.txt
#
# Testy pomocných skriptů z docs/tools/. Samotné testy nesmí ležet v docs/
# (docs/ jde celý do `make dist`), proto jsou tady; testovaný skript se
# importuje z docs/tools/ přes sys.path relativně k umístění testu.
#
# videorec_export_py - unit testy čistých funkcí exportu videa
#   (docs/tools/videorec_export.py) + end-to-end testy, které se bez ffmpeg
#   přeskočí (unittest skip, test i tak projde). Bez interpretu Python3 se
#   test nezaregistruje (jen hláška při configure), nikdy neselže.
#   PYTHONDONTWRITEBYTECODE=1 (a sys.dont_write_bytecode v testu) brání
#   vzniku docs/tools/__pycache__.

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(
        NAME    videorec_export_py
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_videorec_export.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(videorec_export_py PROPERTIES
        LABELS      "tools;videorec;python"
        TIMEOUT     300
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    )
    message(STATUS "  tests/tools: registered videorec_export_py "
                   "(Python ${Python3_VERSION})")
else()
    message(STATUS "  tests/tools: videorec_export_py SKIPPED "
                   "(Python3 interpreter not found)")
endif()
