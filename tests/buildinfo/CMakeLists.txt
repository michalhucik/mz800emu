# tests/buildinfo/CMakeLists.txt
#
# Testy informací o buildu:
#
# build_revision_gen - generátor tools/generate_build_revision_git.sh ve
#   scénářích bez gitu, bez remotů, s nas1 remotem, s řetězem lokálních
#   klonů, s GitHub upstreamem (revize 250 + počet commitů) a s escapováním
#   jména větve. Bez bash nebo git vrací 77 (SKIP).
#
# version_cli - volba --version všech postavených binárek (exit 0, verze,
#   platforma, shoda s vygenerovaným build_revision.c). Cesty binárek
#   předává generator expression $<TARGET_FILE:...>, takže test nepotřebuje
#   kopii binárek v kořeni repozitáře.
#
# Bez interpretu Python3 se testy nezaregistrují (jen hláška při configure).

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(
        NAME    build_revision_gen
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_build_revision_gen.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(build_revision_gen PROPERTIES
        LABELS           "tools;buildinfo;python"
        TIMEOUT          300
        SKIP_RETURN_CODE 77
    )

    set(_mz_version_exes "")
    if(MZ_BUILD_MZ800)
        list(APPEND _mz_version_exes "mz800emu=$<TARGET_FILE:mz800emu>")
    endif()
    if(MZ_BUILD_MZ700_PAL)
        list(APPEND _mz_version_exes "mz700emu-pal=$<TARGET_FILE:mz700emu-pal>")
    endif()
    if(MZ_BUILD_MZ700_NTSC)
        list(APPEND _mz_version_exes "mz700emu-ntsc=$<TARGET_FILE:mz700emu-ntsc>")
    endif()
    if(MZ_BUILD_MZ1500)
        list(APPEND _mz_version_exes "mz1500emu=$<TARGET_FILE:mz1500emu>")
    endif()

    if(_mz_version_exes)
        add_test(
            NAME    version_cli
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/test_version_cli.py
                    ${_mz_version_exes}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
        set_tests_properties(version_cli PROPERTIES
            LABELS  "e2e;buildinfo;python"
            TIMEOUT 180
        )
    endif()
    message(STATUS "  tests/buildinfo: registered build_revision_gen, version_cli "
                   "(Python ${Python3_VERSION})")
else()
    message(STATUS "  tests/buildinfo: SKIPPED (Python3 interpreter not found)")
endif()
