cmake_minimum_required(VERSION 3.24)
project(sb_launcher C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "sb_launcher braucht 32-bit: -A Win32 (wie der Harness)")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Startanwendung (A10, W26/W47): schreibt user.cfg neben sb_native.cfg und
# startet sb_ldr.exe. Win32 + CRT ok (kein XBE-Mapping hier).
# W47: Tab-Seiten (Bildformat/Profile/Steuerung) brauchen comctl32.
# W78: Steuerung (launcher_ctrl.c) + unveraendertes input_sbc_usb.c aus dem
# Harness (nur einkompiliert: SBC-Status exakt wie der Client).
# W82: neue Oberflaeche (launcher_gui.c, W-API, randlos, WIC-Hintergrund),
# UTF-8-Schicht (launcher_u8.c), Ressourcen (Hintergrundbild) + Manifest
# (Common Controls v6, Per-Monitor-DPI v2).
# W116 EDITIONS: private (Standard) | community. community: kein
# Buildzeit-Pfad zum Harness (Client-Suche nur --client-dir/SB_HARNESS_DIR/
# Exe-Ordner), SB_EDITION_COMMUNITY=1, /PDBALTPATH. Beide Editionen betten
# das Hintergrundbild ein (W178 COMM-BG: Eigentum des Projektinhabers).
set(SB_EDITION "private" CACHE STRING "Edition: private | community")
set_property(CACHE SB_EDITION PROPERTY STRINGS private community)
# W178 COMM-BG: beide Editionen betten das Hintergrundbild ein.
set(SB_LAUNCHER_RC src/launcher.rc)
if(SB_EDITION STREQUAL "community")
  set(SB_EDITION_COMMUNITY 1)
elseif(SB_EDITION STREQUAL "private")
  set(SB_EDITION_COMMUNITY 0)
else()
  message(FATAL_ERROR "SB_EDITION muss private oder community sein (ist: ${SB_EDITION})")
endif()
message(STATUS "SB_EDITION=${SB_EDITION}")

# W142 L10N-EN: eingebautes Englisch (sb_lang_en.inc aus en.lang).
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(SB_LANG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../lang)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SB_LANG_DIR}/en.lang)
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../harness/scripts/lang_gen.py
    ${SB_LANG_DIR}/en.lang ${CMAKE_CURRENT_BINARY_DIR}/sb_lang_en.inc
  RESULT_VARIABLE SB_LANG_GEN_RC)
if(NOT SB_LANG_GEN_RC EQUAL 0)
  message(FATAL_ERROR "lang_gen.py scheiterte (en.lang pruefen)")
endif()

# W162 FIX-SMALL: res/bg_nutzer.png ist per LFS versioniert (*.png) und liegt
# in Worktrees nur als Zeiger vor (WIC-Dekodierung + Selbsttest 23 schlagen
# dann fehl). In dem Fall das PNG zur Konfigurierungszeit aus dem direkt
# versionierten WebP erzeugen (Ablage im Build-Ordner, Quelle unangetastet)
# und eine erzeugte .rc mit absolutem Pfad einkompilieren (W178 COMM-BG:
# beide Editionen; convert_bg.py ist byte-identisch zum eingecheckten PNG).
# Mit echtem PNG (Haupt-Checkout): unveraendert.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/res/bg_nutzer.png)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/res/bg_nutzer.png SB_BG_HEAD LIMIT 80)
if(SB_BG_HEAD MATCHES "git-lfs.github.com/spec/v1")
  message(STATUS "bg_nutzer.png ist LFS-Zeiger, erzeuge aus WebP (convert_bg.py)")
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/res/convert_bg.py
      ${CMAKE_CURRENT_BINARY_DIR}/bg_nutzer.png
    RESULT_VARIABLE SB_BG_GEN_RC)
  if(NOT SB_BG_GEN_RC EQUAL 0)
    message(FATAL_ERROR "convert_bg.py scheiterte (Pillow mit WebP noetig)")
  endif()
  file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/bg_nutzer.png" SB_BG_PNG_NATIVE)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/launcher_bg.rc
    "// erzeugt (W162): Hintergrundbild aus WebP (LFS-Zeiger im Quellbaum)\n"
    "#define IDR_BG 101\n"
    "IDR_BG RCDATA \"${SB_BG_PNG_NATIVE}\"\n")
  set(SB_LAUNCHER_RC ${CMAKE_CURRENT_BINARY_DIR}/launcher_bg.rc)
endif()

add_executable(sb_launcher WIN32
  src/launcher_saves.c
  src/launcher.c
  src/launcher_gui.c
  src/launcher_u8.c
  src/launcher_cfg.c
  src/launcher_ctrl.c
  ${SB_LAUNCHER_RC}
  src/sb_launcher.manifest
  src/launcher_assist.c
  src/launcher_packtest.c
  src/launcher_rules.c
  ../harness/src/input_sbc_usb.c
  ../harness/src/sbc_fixture.c
  ../harness/src/sbc_test.c
  ../harness/src/sb_lang.c
  ../harness/src/sb_packscan.c
  ../harness/src/sb_sha256.c
  ../harness/src/iso_vfs.c
  ../harness/src/sb_gamever.c
  ../harness/src/sb_userdata.c
  ../harness/src/sb_savecheck.c
  src/hlog_stub.c
)
target_include_directories(sb_launcher PRIVATE src ../harness/src ${CMAKE_CURRENT_BINARY_DIR})
# W142: Sprachdateien neben die Binaries legen (de.lang; en ist eingebaut)
add_custom_command(TARGET sb_launcher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${SB_LANG_DIR}
    $<TARGET_FILE_DIR:sb_launcher>/lang)
# W142: Sprach-Konsistenz bei jedem Build pruefen (Fehler = Build rot)
add_custom_target(lang_check ALL
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../harness/scripts/lang_check.py
    ${CMAKE_CURRENT_SOURCE_DIR}/..
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Fallback-Suchpfad fuer den Client (Buildzeit); zur Laufzeit gewinnen:
# --client-dir, SB_HARNESS_DIR, Exe-Ordner (s. lcfg_find_client_dir).
if(SB_EDITION_COMMUNITY)
  target_compile_definitions(sb_launcher PRIVATE SB_EDITION_COMMUNITY=1)
  target_link_options(sb_launcher PRIVATE "/PDBALTPATH:%_PDB%")
else()
  target_compile_definitions(sb_launcher PRIVATE
    SB_HARNESS_DIR_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/../harness/build/Release")
endif()
target_link_libraries(sb_launcher PRIVATE comdlg32 shell32 ole32 comctl32 setupapi winusb
  uxtheme dwmapi windowscodecs)
target_compile_options(sb_launcher PRIVATE /W3)
# W217 FIRST-RUN: Launcher mit statischer CRT (/MT) - startet auch ohne
# VC++-Redistributable und meldet dann vor dem Spielstart klar, dass der
# Client (sb_emu.dll) sie braucht (app_client_runtime_missing).
set_property(TARGET sb_launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Hybrid: nur vcruntime statisch, UCRT dynamisch (Teil von Windows 10/11).
target_link_options(sb_launcher PRIVATE "/NODEFAULTLIB:libucrt.lib" "ucrt.lib")
