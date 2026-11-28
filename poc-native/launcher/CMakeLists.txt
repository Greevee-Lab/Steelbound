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
# UTF-8-Schicht (launcher_u8.c), Manifest
# (Common Controls v6, Per-Monitor-DPI v2).
# W116 EDITIONS: private (Standard) | community. community: kein
# Buildzeit-Pfad zum Harness (Client-Suche nur --client-dir/SB_HARNESS_DIR/
# Exe-Ordner), SB_EDITION_COMMUNITY=1, /PDBALTPATH. Beide Editionen liefern
# das Hintergrundbild aus (W178 COMM-BG: Eigentum des Projektinhabers; seit W535 als Datei res/launcher_bg.png).
set(SB_EDITION "private" CACHE STRING "Edition: private | community")
set_property(CACHE SB_EDITION PROPERTY STRINGS private community)
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
# W538 AVLINK: Debug-Infos der ausgelieferten Exes (community: sb_launcher, sb_account). Vergleich auf VirusTotal,
# danach wird die Siegervariante mit einer Zeile Standard (BERICHT_M2 W538):
#   full  = Stand W513 (bis 27.11. Standard): /Zi /DEBUG:FULL /OPT:REF /OPT:ICF, CodeView-Name per /PDBALTPATH:%_PDB%
#           (kommt ueber MSBuild als "%%%<ziel>.pdb%%%" in der Exe an)
#   off   = (Standard seit 27.11.) ohne /Zi und /DEBUG (Linker-Standard /OPT:REF,ICF; kein CodeView-Eintrag, keine PDB) wie 0.6.1;
#           dafuer /MAP (Modul+RVA aus der Absturz-Telemetrie aufloesbar, aendert die Exe nicht)
#   name  = wie full, CodeView-Name aber woertlich <ziel>.pdb (ohne Pfad, ohne %-Reste)
#   noref = /Zi /DEBUG:FULL mit /OPT:NOREF /OPT:NOICF (Linker-Standard bei /DEBUG)
set(SB_RELEASE_DEBUGINFO "off" CACHE STRING "W538: Debug-Infos der Release-Exes: full | off | name | noref (Standard off seit 27.11.: VT-Test V1 ohne Bitdefender-Treffer)")
set_property(CACHE SB_RELEASE_DEBUGINFO PROPERTY STRINGS full off name noref)
if(NOT SB_RELEASE_DEBUGINFO MATCHES "^(full|off|name|noref)$")
  message(FATAL_ERROR "SB_RELEASE_DEBUGINFO muss full, off, name oder noref sein (ist: ${SB_RELEASE_DEBUGINFO})")
endif()
if(SB_EDITION_COMMUNITY)
  message(STATUS "SB_RELEASE_DEBUGINFO=${SB_RELEASE_DEBUGINFO}")
endif()
function(sb_release_debuginfo t)
  if(SB_RELEASE_DEBUGINFO STREQUAL "off")
    target_link_options(${t} PRIVATE "/MAP")
    return()
  endif()
  if(SB_RELEASE_DEBUGINFO STREQUAL "name")
    target_link_options(${t} PRIVATE "/PDBALTPATH:${t}.pdb")
  else()
    target_link_options(${t} PRIVATE "/PDBALTPATH:%_PDB%")
  endif()
  target_compile_options(${t} PRIVATE /Zi)
  if(SB_RELEASE_DEBUGINFO STREQUAL "noref")
    target_link_options(${t} PRIVATE /DEBUG:FULL /OPT:NOREF /OPT:NOICF)
  else()
    target_link_options(${t} PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
  endif()
endfunction()

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
# versionierten WebP erzeugen (Ablage im Build-Ordner, Quelle unangetastet;
# convert_bg.py ist byte-identisch zum eingecheckten PNG).
# W535 AVVT: Das Bild ist keine Ressource mehr (VirusTotal: .rsrc-Block mit hoher Entropie -> "obfuscated"),
# sondern liegt als Datei res/launcher_bg.png neben sb_launcher.exe (POST_BUILD unten; Release-Paket:
# make_alpha.py). Fehlt die Datei, zeichnet der Launcher den einfarbigen Verlauf (wie Schalter "Hintergrund aus").
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/res/bg_nutzer.png)
set(SB_BG_PNG ${CMAKE_CURRENT_SOURCE_DIR}/res/bg_nutzer.png)
file(READ ${SB_BG_PNG} SB_BG_HEAD LIMIT 80)
if(SB_BG_HEAD MATCHES "git-lfs.github.com/spec/v1")
  message(STATUS "bg_nutzer.png ist LFS-Zeiger, erzeuge aus WebP (convert_bg.py)")
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/res/convert_bg.py
      ${CMAKE_CURRENT_BINARY_DIR}/bg_nutzer.png
    RESULT_VARIABLE SB_BG_GEN_RC)
  if(NOT SB_BG_GEN_RC EQUAL 0)
    message(FATAL_ERROR "convert_bg.py scheiterte (Pillow mit WebP noetig)")
  endif()
  set(SB_BG_PNG ${CMAKE_CURRENT_BINARY_DIR}/bg_nutzer.png)
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
  src/launcher_gui_int.h
  src/launcher_u8.c
  src/launcher_cfg.c
  src/launcher_ctrl.c
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
  src/launcher_acchelp.c # W537 LAUNCHER-SLIM: Konto-Auftraege ueber sb_account.exe
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
  ../harness/src/sb_crashtel.c # W522 CRASH-TELEMETRY: Exitcode einordnen
  src/hlog_stub.c
)
target_include_directories(sb_launcher PRIVATE src ../harness/src ${CMAKE_CURRENT_BINARY_DIR})
# W142: Sprachdateien neben die Binaries legen (de.lang; en ist eingebaut)
add_custom_command(TARGET sb_launcher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${SB_LANG_DIR}
    $<TARGET_FILE_DIR:sb_launcher>/lang)
# W535 AVVT: Hintergrundbild als Datei neben die Exe (statt RCDATA-Ressource)
add_custom_command(TARGET sb_launcher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:sb_launcher>/res
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SB_BG_PNG}
    $<TARGET_FILE_DIR:sb_launcher>/res/launcher_bg.png)
# W142: Sprach-Konsistenz bei jedem Build pruefen (Fehler = Build rot)
add_custom_target(lang_check ALL
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../harness/scripts/lang_check.py
    ${CMAKE_CURRENT_SOURCE_DIR}/..
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Fallback-Suchpfad fuer den Client (Buildzeit); zur Laufzeit gewinnen:
# --client-dir, SB_HARNESS_DIR, Exe-Ordner (s. lcfg_find_client_dir).
if(SB_EDITION_COMMUNITY)
  target_compile_definitions(sb_launcher PRIVATE SB_EDITION_COMMUNITY=1)
  # W513 DEFENDER: Release-Launcher mit vollstaendiger PDB wie sb_emu (W279). Bisher fehlte
  # sb_launcher.pdb (keep_pdbs listet sie, Launcher-Abstuerze waren nicht aufloesbar) und die Exe hatte
  # keinen CodeView-Eintrag. /OPT:REF,ICF ausdruecklich, weil /DEBUG sonst NOREF/NOICF setzt (Code bleibt gleich).
  # W538 AVLINK: seit dem Schalter SB_RELEASE_DEBUGINFO (oben), Standard full = genau diese Optionen.
  sb_release_debuginfo(sb_launcher)
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
  uxtheme dwmapi windowscodecs winhttp crypt32 bcrypt ws2_32) # bcrypt/ws2_32 nur noch fuer den Netzteil (Dev/Test)
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
# W532 AVFP: Selbsttest (--selftest, ~13 000 Zeilen mit Testvektoren: Attrappen-HTTP-Anfragen an den
# Discord-Ruecklauf, Passwort-JSON, Testschluessel, DPAPI-Proben) nicht im ausgelieferten Release-Launcher -
# fuer Virenscanner-Heuristiken sah das nach Backdoor/Token-Stealer aus. AUTO = nur in Entwickler-Builds
# (ohne SB_VERSION). make_alpha.py baut fuer den Erststart-Selbsttest zusaetzlich einen nicht ausgelieferten
# Test-Launcher mit -DSB_LAUNCHER_SELFTEST=ON (gleiche Quellen, gleiche SB_VERSION). Ohne Aufrufer entfernt
# der Linker (/OPT:REF, /Gy, /GF) Testcode und Teststrings.
set(SB_LAUNCHER_SELFTEST "AUTO" CACHE STRING "W532: Selbsttest einbauen (AUTO = nur ohne SB_VERSION | ON | OFF)")
set_property(CACHE SB_LAUNCHER_SELFTEST PROPERTY STRINGS AUTO ON OFF)
if(SB_LAUNCHER_SELFTEST STREQUAL "AUTO")
  if(SB_VERSION)
    set(SB_LAUNCHER_SELFTEST_ON 0)
  else()
    set(SB_LAUNCHER_SELFTEST_ON 1)
  endif()
elseif(SB_LAUNCHER_SELFTEST)
  set(SB_LAUNCHER_SELFTEST_ON 1)
else()
  set(SB_LAUNCHER_SELFTEST_ON 0)
endif()
target_compile_definitions(sb_launcher PRIVATE SB_LAUNCHER_SELFTEST=${SB_LAUNCHER_SELFTEST_ON})
# W535 AVVT: GUI-Selbsttest-Quellen nur im Test-/Entwickler-Launcher. /OPT:REF entfernt zwar deren Code, aber nicht
# die statischen Puffer (.bss ist kein COMDAT): ~2,5 MB der 5,7 MB grossen .data (VirtualSize) im Release-Launcher 0.6.3.
if(SB_LAUNCHER_SELFTEST_ON)
  target_sources(sb_launcher PRIVATE src/launcher_gui_test.c src/launcher_gui_test_loc.c)
endif()
# W537 LAUNCHER-SLIM: Netzteil des Kontos (HTTPS, Challenge-Response, Discord-Loopback, Lobbyliste, Erreichbarkeit,
# Kampagnen-Probe, DPAPI-Schreiben) nur noch im Hilfsprogramm sb_account.exe (unten) und - fuer --steelnet (Konto-CLI)
# und den Selbsttest - im Entwickler-/Test-Launcher (gleiche Bedingung wie LCLI_DEV_TOOLS in launcher.c). Der
# ausgelieferte Launcher hat damit kein ws2_32, kein bcrypt, keine Zertifikats-APIs und keinen Empfaenger mehr.
if(NOT SB_VERSION OR SB_LAUNCHER_SELFTEST_ON)
  set(SB_LAUNCHER_NET_DEV 1)
  target_sources(sb_launcher PRIVATE src/launcher_account_net.c src/launcher_camp_net.c ../harness/src/sb_tls.c)
else()
  set(SB_LAUNCHER_NET_DEV 0)
endif()
message(STATUS "W537: Konto-Netzteil im Launcher (nur Entwickler/Test) = ${SB_LAUNCHER_NET_DEV}")
# /Gw: auch statische Daten (z. B. Testtabellen in Funktionen) als COMDAT, damit /OPT:REF sie ohne Aufrufer entfernt.
target_compile_options(sb_launcher PRIVATE /Gw)
message(STATUS "SB_LAUNCHER_SELFTEST=${SB_LAUNCHER_SELFTEST} (${SB_LAUNCHER_SELFTEST_ON})")
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

# ---------------------------------------------------------------------------------------------------------------------
# W537 LAUNCHER-SLIM: Konto-Hilfsprogramm sb_account.exe. Der Launcher startet es je Auftrag unsichtbar (vererbter
# unbenannter Speicherblock + Ereignisse, keine Pipes; launcher_acchelp.h): steelnet-Konto (HTTPS + SPKI-Pin, Challenge-Response), Discord-Anmeldung (lokaler Empfaenger
# 127.0.0.1 nur waehrend des Ablaufs), Lobbyliste, Erreichbarkeit (Kopf-Chip, Kampagne). Gleiche Quellen, Schalter,
# VERSIONINFO, Manifest und Haertung wie der Launcher; liegt im selben Ausgabeordner (neben sb_launcher.exe).
add_executable(sb_account WIN32
  src/sb_account_main.c
  src/launcher_account.c
  src/launcher_account_net.c
  src/launcher_camp.c
  src/launcher_camp_net.c
  src/launcher_u8.c
  ../harness/src/sb_sha256.c
  src/sb_account.manifest
)
target_include_directories(sb_account PRIVATE src ../harness/src ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(sb_account PRIVATE LACC_HELPER=1 "SB_NET_VERSION=\"${SB_NET_VERSION}\"")
if(SB_VERSION)
  target_compile_definitions(sb_account PRIVATE "SB_VERSION_STR=\"${SB_VERSION}\"")
else()
  target_compile_definitions(sb_account PRIVATE SB_TEST_HOOKS=1)
endif()
target_compile_definitions(sb_account PRIVATE SB_LAUNCHER_SELFTEST=${SB_LAUNCHER_SELFTEST_ON})
if(SB_EDITION_COMMUNITY)
  target_compile_definitions(sb_account PRIVATE SB_EDITION_COMMUNITY=1)
  sb_release_debuginfo(sb_account)  # W538 AVLINK (Standard full = /PDBALTPATH:%_PDB% /Zi /DEBUG:FULL /OPT:REF,ICF)
endif()
target_link_libraries(sb_account PRIVATE winhttp crypt32 bcrypt ws2_32)
sb_add_verinfo(sb_account "Steelbound Account Helper")
target_compile_options(sb_account PRIVATE /W3 /Gw /sdl /guard:cf)
set_property(TARGET sb_account PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_options(sb_account PRIVATE "/NODEFAULTLIB:libucrt.lib" "ucrt.lib"
  "/guard:cf" "/DYNAMICBASE" "/NXCOMPAT" "/DEPENDENTLOADFLAG:0x800" "/CETCOMPAT")
add_dependencies(sb_launcher sb_account)
