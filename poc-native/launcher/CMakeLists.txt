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
# W221 HOTAS: input_joy.c (DirectInput 8, dinput8.dll dynamisch, keine Lib).
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
# W353 LOC-DURCHSTICH / W389 RELEASE-030: SB_LOC_PLAY gibt in der Community-Ausgabe den LoC-Start frei.
# Seit 0.3.0 Standard an (LoC fuer alle); -DSB_LOC_PLAY=OFF = Notschalter (LoC-Start grau).
# Interne Ausgabe: LoC-Start immer frei.
option(SB_LOC_PLAY "W389: LoC-Start in der Community-Ausgabe (Standard an; OFF = grau, Notschalter)" ON)
message(STATUS "SB_LOC_PLAY=${SB_LOC_PLAY}")

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
  # W414: Pfad mit Vorwaerts-Schraegstrichen schreiben. Mit TO_NATIVE_PATH standen einfache
  # Backslashes im rc-String, rc.exe las Ordner wie \abc, \new, \x.. als Escape (\a, \n, \x)
  # und brach ab. rc.exe nimmt '/' als Trenner an (wie launcher.rc "../res/...").
  file(TO_CMAKE_PATH "${CMAKE_CURRENT_BINARY_DIR}/bg_nutzer.png" SB_BG_PNG_RC)
  string(REPLACE "\\" "/" SB_BG_PNG_RC "${SB_BG_PNG_RC}")
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/launcher_bg.rc
    "// erzeugt (W162): Hintergrundbild aus WebP (LFS-Zeiger im Quellbaum)\n"
    "#define IDR_BG 101\n"
    "IDR_BG RCDATA \"${SB_BG_PNG_RC}\"\n")
  set(SB_LAUNCHER_RC ${CMAKE_CURRENT_BINARY_DIR}/launcher_bg.rc)
endif()

add_executable(sb_launcher WIN32
  src/launcher_saves.c
  src/launcher.c
  src/launcher_gui.c
  src/launcher_gui_draw.c
  src/launcher_gui_settings.c
  src/launcher_gui_help.c
  src/launcher_gui_bind.c
  src/launcher_gui_assist.c
  src/launcher_gui_content.c
  src/launcher_gui_loc.c
  src/launcher_gui_hub.c
  src/launcher_gui_netrep.c # W496 NET-TELEMETRY: Frage anonyme Netzwerkstatistik
  src/launcher_gui_account.c
  src/launcher_gui_test_loc.c
  src/launcher_gui_test.c
  src/launcher_gui_int.h
  src/launcher_u8.c
  src/launcher_cfg.c
  src/launcher_ctrl.c
  ${SB_LAUNCHER_RC}
  src/sb_launcher.manifest
  src/launcher_assist.c
  src/launcher_packtest.c
  src/launcher_rules.c
  src/launcher_update.c
  src/launcher_alpha.c
  src/launcher_alpha.h
  src/launcher_alpha.inc
  src/launcher_diag.c
  src/launcher_diag.h
  src/launcher_diag.inc
  src/launcher_games.c
  src/launcher_orig.c
  src/launcher_layout.c
  src/launcher_account.c
  src/launcher_camp.c
  ../harness/src/sb_tls.c
  ../harness/src/input_sbc_usb.c
  ../harness/src/input_joy.c
  ../harness/src/input_despike.c
  ../harness/src/sbc_fixture.c
  ../harness/src/sbc_test.c
  ../harness/src/sb_lang.c
  ../harness/src/sb_packscan.c
  ../harness/src/sb_sha256.c
  ../harness/src/iso_vfs.c
  ../harness/src/sb_gamever.c
  ../harness/src/sb_userdata.c
  ../harness/src/sb_savecheck.c
  ../harness/src/sb_diagcore.c
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
  if(SB_LOC_PLAY)
    target_compile_definitions(sb_launcher PRIVATE SB_LOC_PLAY=1)
  endif()
else()
  target_compile_definitions(sb_launcher PRIVATE
    SB_HARNESS_DIR_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/../harness/build/Release")
  # W336 LOC-TESTPARTNER: nur interne Ausgabe (community.include nimmt die Datei heraus)
  target_sources(sb_launcher PRIVATE src/launcher_loctp.c src/launcher_loctp.h)
  target_compile_definitions(sb_launcher PRIVATE SB_HAVE_LOCTP=1)
  # 18.11.: steelnet v2 ist live (W335 abgenommen, Pins gesetzt) - Testpartner darf den echten Dienst nutzen
  target_compile_definitions(sb_launcher PRIVATE LOCTP_REAL_SERVICE_ON=1)
endif()
target_link_libraries(sb_launcher PRIVATE comdlg32 shell32 ole32 comctl32 setupapi winusb
  uxtheme dwmapi windowscodecs winhttp crypt32 bcrypt ws2_32)
# W250 UPDATE-CHECK: Release-Version wie beim Client (make_alpha.py setzt
# -DSB_VERSION=0.1.0-alpha.N fuer beide Komponenten); leer = Entwickler-Build,
# dann ist der Update-Hinweis ausgeblendet (nichts zu vergleichen).
set(SB_VERSION "" CACHE STRING "Release-Version (leer = Entwickler-Build)")
if(NOT SB_VERSION MATCHES "^[A-Za-z0-9.+-]*$")
  message(FATAL_ERROR "SB_VERSION nur aus A-Z a-z 0-9 . + - (ist: ${SB_VERSION})")
endif()
if(SB_VERSION)
  target_compile_definitions(sb_launcher PRIVATE "SB_VERSION_STR=\"${SB_VERSION}\"")
endif()
# W334 (SECURITY.md §9, L6): Test-Haken (z. B. --steelnet game-auth gibt den Spielschluessel aus) nur in
# Entwickler-Builds; Release-Builds (make_alpha setzt SB_VERSION) haben sie nicht.
if(NOT SB_VERSION)
  target_compile_definitions(sb_launcher PRIVATE SB_TEST_HOOKS=1)
endif()
message(STATUS "SB_VERSION=${SB_VERSION}")
# W421 LOC-VERSION-GATE: eigene steelnet-Version wie beim Client (W420, harness/CMakeLists.txt) aus poc-native/VERSION,
# auch in Entwickler-Builds; Vergleich mit min_client_version aus GET /v1/info (LoC-Start gesperrt, wenn aelter).
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../VERSION SB_NET_VERSION LIMIT_COUNT 1)
string(STRIP "${SB_NET_VERSION}" SB_NET_VERSION)
if(NOT SB_NET_VERSION MATCHES "^[A-Za-z0-9.+-]+$")
  message(FATAL_ERROR "poc-native/VERSION ungueltig (${SB_NET_VERSION})")
endif()
target_compile_definitions(sb_launcher PRIVATE "SB_NET_VERSION=\"${SB_NET_VERSION}\"")
# REL-DEFENDER-FP: Dateieigenschaften (VERSIONINFO) wie beim Client, Version aus SB_VERSION bzw.
# poc-native/VERSION. Manifest (asInvoker, Win10/11, Per-Monitor-DPI v2, Common Controls v6) s.
# src/sb_launcher.manifest + Linker-UAC-Eintrag.
include(${CMAKE_CURRENT_SOURCE_DIR}/../harness/src/sb_verinfo.cmake)
sb_add_verinfo(sb_launcher "Steelbound Launcher")
target_compile_options(sb_launcher PRIVATE /W3)
# W217 FIRST-RUN: Launcher mit statischer CRT (/MT) - startet auch ohne
# VC++-Redistributable und meldet dann vor dem Spielstart klar, dass der
# Client (sb_emu.dll) sie braucht (app_client_runtime_missing).
set_property(TARGET sb_launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Hybrid: nur vcruntime statisch, UCRT dynamisch (Teil von Windows 10/11).
target_link_options(sb_launcher PRIVATE "/NODEFAULTLIB:libucrt.lib" "ucrt.lib")
# W332 SEC-HARDEN: Binaer-Haertung (Nachweis: tools/sec/pe_harden_check.py, dumpbin).
#  /sdl       zusaetzliche Sicherheitswarnungen als Fehler + strengere /GS-Heuristik
#  /guard:cf  Control Flow Guard (indirekte Aufrufe nur auf gueltige Ziele)
#  /DYNAMICBASE /NXCOMPAT  ASLR + DEP (MSVC-Standard, hier ausdruecklich)
#  /DEPENDENTLOADFLAG:0x800  statische Importe (uxtheme, dwmapi, winhttp ...) nur aus
#             System32 - keine untergeschobenen DLLs aus dem Programmordner (z. B. Downloads).
#  /CETCOMPAT Schattenstapel-tauglich markiert (Windows erzwingt CET nur in 64-bit-Prozessen;
#             vorbereitet). /HIGHENTROPYVA gibt es nur fuer 64-bit (hier 32-bit).
target_compile_options(sb_launcher PRIVATE /sdl /guard:cf)
target_link_options(sb_launcher PRIVATE "/guard:cf" "/DYNAMICBASE" "/NXCOMPAT" "/DEPENDENTLOADFLAG:0x800" "/CETCOMPAT")
