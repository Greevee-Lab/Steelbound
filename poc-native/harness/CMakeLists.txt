cmake_minimum_required(VERSION 3.24)
project(sb_harness C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "sb_harness braucht 32-bit: -A Win32")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- W116 EDITIONS: private (Standard, Hauptlinie) | community (Clean-Export) ----
# community: SB_EDITION_COMMUNITY=1 (keine privaten Standardpfade, s. src/sb_edition.h),
# PDB nur mit Dateinamen in den Binaries (/PDBALTPATH, keine Buildpfade). private:
# Kommandozeile unveraendert (kein Zusatz-Define), Verhalten bitgleich.
# Eigenes Build-Verzeichnis je Edition, z. B. -B poc-native/harness/build-community.
set(SB_EDITION "private" CACHE STRING "Edition: private | community")
set_property(CACHE SB_EDITION PROPERTY STRINGS private community)
if(SB_EDITION STREQUAL "community")
  set(SB_EDITION_COMMUNITY 1)
elseif(SB_EDITION STREQUAL "private")
  set(SB_EDITION_COMMUNITY 0)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/sb_private_paths.h)
    message(FATAL_ERROR "SB_EDITION=private braucht src/sb_private_paths.h (nicht im Community-Export) - -DSB_EDITION=community verwenden")
  endif()
else()
  message(FATAL_ERROR "SB_EDITION muss private oder community sein (ist: ${SB_EDITION})")
endif()
message(STATUS "SB_EDITION=${SB_EDITION}")


# ---- Loader: winzig, kein CRT, fix auf 0x10000 (Vorbild: Cxbx cxbxr-ldr) ----
add_executable(sb_ldr src/ldr.c)
target_link_options(sb_ldr PRIVATE
  "/NODEFAULTLIB" "/DYNAMICBASE:NO" "/BASE:0x10000" "/FIXED"
  "/LARGEADDRESSAWARE" "/SUBSYSTEM:CONSOLE" "/ENTRY:rawMain")
target_link_libraries(sb_ldr PRIVATE kernel32)
target_compile_options(sb_ldr PRIVATE /W3 /GS-)

# ---- W142 L10N-EN: eingebautes Englisch (sb_lang_en.inc aus en.lang) ----
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(SB_LANG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../lang)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SB_LANG_DIR}/en.lang)
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/lang_gen.py
    ${SB_LANG_DIR}/en.lang ${CMAKE_CURRENT_BINARY_DIR}/sb_lang_en.inc
  RESULT_VARIABLE SB_LANG_GEN_RC)
if(NOT SB_LANG_GEN_RC EQUAL 0)
  message(FATAL_ERROR "lang_gen.py scheiterte (en.lang pruefen)")
endif()

# ---- Emu-DLL: der eigentliche Harness ----
add_library(sb_emu SHARED
  src/sb_emu.def
  src/emu_main.c
  src/sb_relaunch.c
  src/hlog.c
  src/hcfg.c
  src/sb_lang.c
  src/xbe_loader.c
  src/kernel_hle.c
  src/krnl_sync.c
  src/krnl_file.c
  src/krnl_mem.c
  src/xdk_hooks.c
  src/hooks_d3d.c
  src/hooks_timing.c
  src/hooks_audio.c
  src/hooks_xapi.c
  src/hooks_input.c
  src/hooks_bots.c
  src/fs_emu.c
  src/sctrace.c
  src/d3d11_render.c
  src/render_im.c
  src/render_shader.c
  src/render_state.c
  src/render_rtt.c
  src/render_diag.c
  src/render_fx.c
  src/render_mfx.c
  src/render_shfx.c
  src/render_fx_rem.c
  src/render_bloom.c
  src/render_visq.c
  src/render_interp.c
  src/render_warm.c
  src/render_edge.c
  src/render_cwiden.c
  src/render_winmode.c
  src/render_combiner.c
  src/render_shade.c
  src/render_vsh.c
  src/render_vshgen.c
  src/render_psgen.c
  src/sb_tex.c
  src/sb_remaster.c
  src/sb_packscan.c
  src/sb_sha256.c
  src/iso_vfs.c
  src/sb_gamever.c
  src/sb_userdata.c
  src/sb_overlay.c
  src/sb_perfhud.c
  src/audio_out.c
  src/audio_fmt.c
  src/audio_mix.c
  src/audio_3d.c
  src/diag_states.c
  src/diag_campaign.c
  src/diag_input.c
  src/watch_dr.c
  src/input_sbc_usb.c
  src/sbc_fixture.c
  src/sbc_wire.c
  src/input_map.c
  src/input_mouse.c
  src/input_tap.c
  src/hlock.c
  src/vr.c
)
target_include_directories(sb_emu PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(sb_emu PRIVATE d3d11 dxgi dxguid dbghelp windowscodecs ole32 oleaut32 uuid winmm setupapi winusb synchronization)
target_compile_options(sb_emu PRIVATE /Zi)
target_link_options(sb_emu PRIVATE /DEBUG:FASTLINK)
target_link_options(sb_emu PRIVATE "/BASE:0x30000000" "/DYNAMICBASE:NO" "/STACK:8388608" "/MAP")
target_compile_options(sb_emu PRIVATE /W3)

# ---- sbc_probe: USB-Testprogramm zum Original-Controller (W49, CTRL-SBC) ----
# Eigene Exe, nutzt nur input_sbc_usb.* (kein Harness-Code). sb_emu bleibt
# unveraendert (Verdrahtung folgt in Welle 4, W44).
add_executable(sbc_probe
  src/sbc_probe.c
  src/sbc_fixture.c
  src/sbc_test.c
  src/input_sbc_usb.c
)
target_include_directories(sbc_probe PRIVATE src)
target_link_libraries(sbc_probe PRIVATE setupapi winusb)
target_compile_options(sbc_probe PRIVATE /W3)

# ---- input_selftest: Vergleich alt<->neu + Gamepad-Logik (W76) ----
add_executable(input_selftest
  src/input_selftest.c
  src/input_map.c
  src/input_tap.c
  src/input_sbc_usb.c
)
target_include_directories(input_selftest PRIVATE src)
target_link_libraries(input_selftest PRIVATE setupapi winusb)
target_compile_options(input_selftest PRIVATE /W3)

# ---- hlock_selftest: Suspend-Fenster der Host-Sperren (W119 LOADLOCK) ----
add_executable(hlock_selftest
  src/hlock_selftest.c
  src/hlock.c
)
target_include_directories(hlock_selftest PRIVATE src)
target_link_libraries(hlock_selftest PRIVATE synchronization)
target_compile_options(hlock_selftest PRIVATE /W3)

# ---- iso_vfs_check: XDVDFS-Abbild pruefen/vergleichen (W150 ISO-DIRECT) ----
add_executable(iso_vfs_check
  tests/iso_vfs_check.c
  src/iso_vfs.c
  src/sb_gamever.c
  src/sb_sha256.c
  src/sb_userdata.c
)
target_include_directories(iso_vfs_check PRIVATE src)
target_compile_options(iso_vfs_check PRIVATE /W3)

# ---- vr_mock_runtime: OpenXR-Test-Runtime (W134 VR-PROTO, nur Tests) ----
# Eigene DLL + Manifest vr_mock_runtime.json neben den Binaries; Auswahl im
# Client per vr_runtime_json=<pfad>. Nie fuer Spieler (Launcher nutzt die
# aktive Runtime des Systems).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/vr_mock_runtime.c)
add_library(vr_mock_runtime SHARED tests/vr_mock_runtime.c)
target_include_directories(vr_mock_runtime PRIVATE src)
target_link_libraries(vr_mock_runtime PRIVATE d3d11 dxgi dxguid)
target_compile_options(vr_mock_runtime PRIVATE /W3)
add_custom_command(TARGET vr_mock_runtime POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vr_mock_runtime.json
    $<TARGET_FILE_DIR:vr_mock_runtime>/vr_mock_runtime.json)
endif()

# ---- render_selftest: VSH-Interpreter/-Uebersetzung + Combiner (W77 RENDER-QA) ----
# W152 COMM-SELFTEST: privat mit render_selftest_data.h (Spiel-Testvektoren),
# community ohne (Vektoren zur Laufzeit aus der eigenen default.xbe) -> beide.
add_executable(render_selftest
  src/render_selftest.c
  src/render_vsh.c
  src/render_vshgen.c
  src/render_psgen.c
)
target_include_directories(render_selftest PRIVATE src)
target_link_libraries(render_selftest PRIVATE d3d11)
target_compile_options(render_selftest PRIVATE /W3)

# W116: Edition an alle C-Ziele (nur community setzt etwas)
if(SB_EDITION_COMMUNITY)
  foreach(t sb_ldr sb_emu sbc_probe input_selftest render_selftest iso_vfs_check)
    target_compile_definitions(${t} PRIVATE SB_EDITION_COMMUNITY=1)
    target_link_options(${t} PRIVATE "/PDBALTPATH:%_PDB%")
  endforeach()
endif()

# Default-Config neben die Binaries legen
# W116: community kopiert eine Fassung mit geleerten xbe_path/media_root
# (private Rechnerpfade; der Spieler waehlt sein Abbild im Launcher),
# private unveraendert die Repo-Datei.
# W152 COMM-CFG: community nimmt die Spieler-cfg sb_player.cfg (keine
# Test-Voreinstellungen) und legt sie als sb_native.cfg ab; Tests nutzen
# weiter sb_native.cfg.
set(SB_CFG_SRC ${CMAKE_CURRENT_SOURCE_DIR}/sb_native.cfg)
# W180 EXPORT: im Community-Export liegt die Spieler-cfg bereits als
# sb_native.cfg (Positivliste sb_player.cfg => sb_native.cfg, ohne die
# private Test-cfg) -> dann diese nehmen.
if(SB_EDITION_COMMUNITY AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sb_player.cfg)
  set(SB_CFG_SRC ${CMAKE_CURRENT_SOURCE_DIR}/sb_player.cfg)
endif()
if(SB_EDITION_COMMUNITY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SB_CFG_SRC})
  file(READ ${SB_CFG_SRC} _sb_cfg)
  string(REGEX REPLACE "(^|\n)xbe_path=[^\r\n]*" "\\1xbe_path=" _sb_cfg "${_sb_cfg}")
  string(REGEX REPLACE "(^|\n)media_root=[^\r\n]*" "\\1media_root=" _sb_cfg "${_sb_cfg}")
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/sb_native.community.cfg "${_sb_cfg}")
  set(SB_CFG_SRC ${CMAKE_CURRENT_BINARY_DIR}/sb_native.community.cfg)
endif()
add_custom_command(TARGET sb_emu POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${SB_CFG_SRC}
    $<TARGET_FILE_DIR:sb_emu>/sb_native.cfg)
# W76: Eingabe-Profile neben die Binaries legen
add_custom_command(TARGET sb_emu POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/profiles
    $<TARGET_FILE_DIR:sb_emu>/profiles)
# W142: Sprachdateien neben die Binaries legen (de.lang; en ist eingebaut)
add_custom_command(TARGET sb_emu POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${SB_LANG_DIR}
    $<TARGET_FILE_DIR:sb_emu>/lang)
# W142: Sprach-Konsistenz bei jedem Build pruefen (Fehler = Build rot)
add_custom_target(lang_check ALL
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/lang_check.py
    ${CMAKE_CURRENT_SOURCE_DIR}/..
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
