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
# W353 LOC-DURCHSTICH / W389 RELEASE-030: LoC in der Community-Ausgabe (nur community wirksam). Seit 0.3.0
# Standard an (LoC fuer alle, Nutzerentscheid 20.11.); -DSB_LOC_PLAY=OFF = Notschalter (LoC-Start grau,
# Platzhalter-Transport). Client: Rollback-Eingabeschicht und LoC-Standards wie intern.
option(SB_LOC_PLAY "W389: LoC in der Community-Ausgabe (Standard an; OFF = LoC grau, Notschalter)" ON)
if(SB_LOC_PLAY AND NOT SB_EDITION_COMMUNITY)
  set(SB_LOC_PLAY OFF)  # intern ohne Wirkung, Kommandozeile bleibt unveraendert
endif()
message(STATUS "SB_LOC_PLAY=${SB_LOC_PLAY}")


# ---- Loader: winzig, kein CRT, fix auf 0x10000 (Vorbild: Cxbx cxbxr-ldr) ----
add_executable(sb_ldr src/ldr.c)
target_link_options(sb_ldr PRIVATE
  "/NODEFAULTLIB" "/DYNAMICBASE:NO" "/BASE:0x10000" "/FIXED"
  "/LARGEADDRESSAWARE" "/SUBSYSTEM:CONSOLE" "/ENTRY:rawMain")
target_link_libraries(sb_ldr PRIVATE kernel32)
target_compile_options(sb_ldr PRIVATE /W3 /GS-)
# W332 SEC-HARDEN: DEP ausdruecklich (das XBE-Abbild bekommt seine Seiten per VirtualProtect als
# PAGE_EXECUTE_READWRITE, xbe_loader.c/kernel_hle.c). Bewusst NICHT: /DYNAMICBASE (das XBE braucht
# die feste Basis 0x10000), /GS und /guard:cf (kein CRT -> kein Sicherheits-Cookie und keine
# Lade-Konfiguration; der Loader liest keine Fremddaten), /SAFESEH (SEH-Handler des Gastcodes liegen
# im Abbild-Bereich des Loaders). DLL-Suchpfad: SetDefaultDllDirectories in ldr.c.
target_link_options(sb_ldr PRIVATE "/NXCOMPAT")

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
  src/guest_wx.c
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
  src/sb_rauto.c
  src/sb_rauto_core.c
  src/sb_mauto.c
  src/sb_mauto_core.c
  src/sb_cchain.c
  src/sb_cchain_core.c
  src/fs_emu.c
  src/sctrace.c
  src/d3d11_render.c
  src/render_im.c
  src/render_dsc.c
  src/render_shader.c
  src/render_state.c
  src/render_rtt.c
  src/render_diag.c
  src/render_fx.c
  src/render_line.c
  src/render_mfx.c
  src/render_shfx.c
  src/render_fx_rem.c
  src/render_bloom.c
  src/render_d3ddbg.c
  src/sb_texhealth.c
  src/render_visq.c
  src/render_interp.c
  src/render_warm.c
  src/render_locmenu.c
  src/render_edge.c
  src/render_cpersp.c
  src/render_monview.c
  src/render_cpanini.c
  src/render_xfx.c
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
  src/sb_profile.c
  src/sb_gamever.c
  src/loc_boot.c
  src/loc_mem.c
  src/loc_render.c
  src/loc_pad.c
  src/loc_aim.c
  src/loc_vpf.c
  src/loc_dodge.c
  src/loc_smooth.c
  src/loc_audio.c
  src/loc_audio_scope.c
  src/loc_wma.c
  src/loc_xact_fmt.c
  src/loc_sync.c
  src/loc_state.c
  src/loc_rollback.c
  src/loc_lampdiag.c
  src/loc_rb_ring.c
  src/loc_rbi.c
  src/loc_rbn.c
  src/loc_rule.c
  src/loc_rulenet.c
  src/loc_lsarb.c
  src/loc_lsarb_game.c
  src/krnl_loc.c
  src/sb_userdata.c
  src/sb_savecheck.c
  src/sb_overlay.c
  src/sb_pause.c
  src/render_photo.c
  src/render_reproj.c
  src/render_clip.c
  src/render_vexp.c
  src/vexp_enc.c
  src/sb_feedback.c
  src/sb_diagcore.c
  src/sb_crash.c
  src/sb_perfhud.c
  src/sb_nethud.c
  src/sb_watermark.c
  src/sb_origview.c
  src/audio_out.c
  src/audio_fmt.c
  src/audio_mix.c
  src/audio_ring.c
  src/audio_3d.c
  src/audio_fx.c
  src/audio_spatial.c
  src/diag_states.c
  src/diag_campaign.c
  src/diag_input.c
  src/watch_dr.c
  src/input_sbc_usb.c
  src/sbc_fixture.c
  src/sbc_wire.c
  src/input_map.c
  src/input_joy.c
  src/input_mouse.c
  src/sb_state.c
  src/input_tap.c
  src/hlock.c
  src/vr.c
)
target_include_directories(sb_emu PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(sb_emu PRIVATE d3d11 dxgi dxguid dbghelp windowscodecs ole32 oleaut32 uuid winmm setupapi winusb synchronization mfuuid) # W367: mfuuid = nur GUIDs (statisch); mfplat/mfreadwrite dynamisch
# W286 LOC-SYSLINK / W292 LOC-COMM: Netz-HLE (Winsock, nur Loopback/private LAN-Adressen, kein Bind auf
# alle Schnittstellen, kein DNS, nur mit loc_boot=1) in beiden Fassungen (Nutzer 15.11.: "keine Geheimnisse").
# Community: clean_check-Freigaben in editions/clean_allow.tsv (Kategorie netz); der LoC-Start im Launcher
# bleibt dort gesperrt (lgame_loc_client_ready).
target_sources(sb_emu PRIVATE src/loc_net.c src/loc_net_core.c src/loc_netsim.c src/loc_route.c) # W395: loc_route (Wegwahl)
# W324 LOC-NETPLAY: eigener UDP-Transport mit Internet-Treffpunkt (DNS, Relays). W389: seit 0.3.0 auch im
# Community-Export; nur mit -DSB_LOC_PLAY=OFF bekommt Community den Platzhalter (Original-Transport).
if(SB_EDITION_COMMUNITY AND SB_LOC_PLAY)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/loc_tun.c)
    message(FATAL_ERROR "SB_LOC_PLAY=ON braucht src/loc_tun.c (Community-Export ab W389 bzw. 0.3.0)")
  endif()
  set(SB_LOC_TUN_SRC src/loc_tun.c)
elseif(SB_EDITION_COMMUNITY)
  set(SB_LOC_TUN_SRC src/loc_tun_stub.c)
else()
  set(SB_LOC_TUN_SRC src/loc_tun.c)
endif()
target_sources(sb_emu PRIVATE ${SB_LOC_TUN_SRC})
target_link_libraries(sb_emu PRIVATE ws2_32)
# W334 LOC-SEC-CLIENT: Sicherheitsprotokoll v2 (SECURITY.md §12 B) - SChannel-TLS mit SPKI-Pin, SBT2 ueber CNG,
# Gastfilter (auch in den HLE-Hooks beider Fassungen). Netznahe neue Dateien mit /sdl /W4 /WX (SECURITY.md §7).
set(SB_LOC_SEC_SRC src/sb_tls.c src/sb_e2e.c src/loc_guard.c)
set_source_files_properties(${SB_LOC_SEC_SRC} PROPERTIES COMPILE_OPTIONS "/sdl;/W4;/WX")
# W407 CRYPTO-PORTABLE: X25519 und AES-256-GCM aus einer unveraenderten Teilkopie von BearSSL 0.6 (MIT,
# third_party/bearssl/LICENSE.txt) statt Windows-CNG - gleiche Bausteine auf jeder Plattform, zeitkonstant
# (aes_ct, ghash_ctmul, c25519_m31). Protokoll unveraendert (SECURITY.md §4), Selbsttest + Vektoren pruefen es.
add_library(sb_bearssl STATIC
  third_party/bearssl/src/symcipher/aes_ct.c
  third_party/bearssl/src/symcipher/aes_ct_enc.c
  third_party/bearssl/src/symcipher/aes_ct_ctr.c
  third_party/bearssl/src/hash/ghash_ctmul.c
  third_party/bearssl/src/aead/gcm.c
  third_party/bearssl/src/ec/ec_c25519_m31.c
  third_party/bearssl/src/codec/ccopy.c
  third_party/bearssl/src/codec/dec32be.c
  third_party/bearssl/src/codec/enc32be.c
  third_party/bearssl/src/codec/dec32le.c
  third_party/bearssl/src/codec/enc32le.c
)
target_include_directories(sb_bearssl PUBLIC third_party/bearssl/inc PRIVATE third_party/bearssl/src)
target_compile_options(sb_bearssl PRIVATE /W3 /sdl /wd4267) # C4267 nur aus dem (ungenutzten) bearssl_ssl.h
target_sources(sb_emu PRIVATE ${SB_LOC_SEC_SRC})
target_link_libraries(sb_emu PRIVATE sb_bearssl bcrypt secur32 crypt32)
target_compile_options(sb_emu PRIVATE /Zi)
# W279 DIAG-CRASH: Release (community) mit vollstaendiger PDB - FASTLINK-PDBs
# verweisen auf die .obj-Dateien und taugen spaeter nicht zum Aufloesen von
# Minidumps (crash_symbolize.py). Private Builds bleiben schnell (FASTLINK),
# ausser -DSB_PDB_FULL=ON.
option(SB_PDB_FULL "Vollstaendige PDB (/DEBUG:FULL) statt FASTLINK" OFF)
if(SB_EDITION STREQUAL "community" OR SB_PDB_FULL)
  target_link_options(sb_emu PRIVATE /DEBUG:FULL)
else()
  target_link_options(sb_emu PRIVATE /DEBUG:FASTLINK)
endif()
target_link_options(sb_emu PRIVATE "/BASE:0x30000000" "/DYNAMICBASE:NO" "/STACK:8388608" "/MAP")
# W332 SEC-HARDEN: /sdl (Sicherheitswarnungen als Fehler, strengere /GS-Heuristik; /GS ist
# MSVC-Standard), DEP ausdruecklich, statische Importe nur aus System32 (zusaetzlich zu
# SetDefaultDllDirectories in ldr.c). Bewusst NICHT: /DYNAMICBASE (feste Basis 0x30000000:
# VEH-OWN-Bereich, Speicher-Layout/Golden deterministisch) und /guard:cf (CFG wirkt nur, wenn die
# Exe es hat; sb_ldr.exe kann es nicht, s. dort). Begruendung: BERICHT_M2 Abschnitt W332.
target_compile_options(sb_emu PRIVATE /sdl)
target_link_options(sb_emu PRIVATE "/NXCOMPAT" "/DEPENDENTLOADFLAG:0x800")
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
  src/input_mouse_selftest.c
  src/sb_rauto_core.c
  src/sb_mauto_core.c
  src/sb_cchain_core.c
  src/audio_vol.c
  src/sb_profile.c
  src/input_map.c
  src/input_joy.c
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
  src/sb_savecheck.c
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

# ---- loc_net_selftest: System-Link-Netz-HLE LoC, zwei Instanzen ueber Loopback (W286; seit W292 beide Fassungen) ----
add_executable(loc_net_selftest
  tests/loc_net_selftest.c
  src/loc_net_core.c
  src/loc_rule.c
  src/loc_netsim.c
  src/loc_route.c
  ${SB_LOC_TUN_SRC}
  ${SB_LOC_SEC_SRC}
  src/sb_sha256.c
)
target_include_directories(loc_net_selftest PRIVATE src)
target_link_libraries(loc_net_selftest PRIVATE sb_bearssl ws2_32 winmm bcrypt secur32 crypt32)
target_compile_options(loc_net_selftest PRIVATE /W3)

# ---- loc_audio_selftest: XACT-1-Parser (XWB/XSB) + Wave-Bank-Dekodierung, synthetische Daten (W298) ----
add_executable(loc_audio_selftest
  tests/loc_audio_selftest.c
  src/loc_xact_fmt.c
  src/loc_audio_scope.c
  src/audio_fmt.c
  src/loc_wma.c
)
target_include_directories(loc_audio_selftest PRIVATE src)
target_compile_definitions(loc_audio_selftest PRIVATE LW_SELFTEST=1) # W367: Playlist + Media-Foundation-Dekoder ohne Mischer
target_link_libraries(loc_audio_selftest PRIVATE mfuuid ole32)
target_compile_options(loc_audio_selftest PRIVATE /W3)

# ---- audio_reverb_selftest: W348 I3DL2-Hall (Nachhallzeit, Pegel, Stabilitaet, Kosten) + Huellkurven ----
add_executable(audio_reverb_selftest
  tests/audio_reverb_selftest.c
  src/audio_fx.c
  src/audio_spatial.c
)
target_include_directories(audio_reverb_selftest PRIVATE src)
target_compile_definitions(audio_reverb_selftest PRIVATE AUFX_SELFTEST)
target_compile_options(audio_reverb_selftest PRIVATE /W3)

# ---- audio_ring_selftest: Unterlauf/Cursor-Reset der Bink-Ringstimmen, synthetischer Ring (ISSUE-10) ----
add_executable(audio_ring_selftest
  tests/audio_ring_selftest.c
  src/audio_ring.c
  src/audio_fmt.c
)
target_include_directories(audio_ring_selftest PRIVATE src)
target_compile_options(audio_ring_selftest PRIVATE /W3)

# ---- loc_rb_selftest: Zustandsring fuer Rollback (Write-Watch + Schreibschutz-Verfolgung), synthetischer Speicher (W330) ----
add_executable(loc_rb_selftest
  tests/loc_rb_selftest.c
  src/loc_rb_ring.c
)
target_include_directories(loc_rb_selftest PRIVATE src)
target_compile_options(loc_rb_selftest PRIVATE /W3)

# ---- loc_rbi_selftest: Eingabeschicht fuer Rollback R2 (Slots, Vorhersage, Kanal, Hash), Spielzwilling synthetisch (W338) ----
add_executable(loc_rbi_selftest
  tests/loc_rbi_selftest.c
  tests/loc_lsarb_cases.c
  src/loc_rbi.c
  src/loc_rbn.c
  src/loc_lsarb.c
  src/loc_state.c
)
target_include_directories(loc_rbi_selftest PRIVATE src)
target_compile_options(loc_rbi_selftest PRIVATE /W3)

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

# W219 WATERMARK: Release-Version fuer das Alpha-Wasserzeichen (make_alpha.py
# --version bzw. release.sh setzen -DSB_VERSION=0.1.0-alpha.N); leer = "dev".
set(SB_VERSION "" CACHE STRING "Release-Version (leer = Entwickler-Build)")
if(NOT SB_VERSION MATCHES "^[A-Za-z0-9.+-]*$")
  message(FATAL_ERROR "SB_VERSION nur aus A-Z a-z 0-9 . + - (ist: ${SB_VERSION})")
endif()
if(SB_VERSION)
  target_compile_definitions(sb_emu PRIVATE "SB_VERSION_STR=\"${SB_VERSION}\"")
endif()
message(STATUS "SB_VERSION=${SB_VERSION}")
# W420 LOC-CLIPDUMP (Nutzerentscheid 22.11.): steelnet nur mit gleicher Client-Version - Version aus poc-native/VERSION
# zur Build-Zeit (auch Entwickler-Builds), Austausch im Tunnel-HELLO und bei der Anmeldung am Treffpunkt (loc_tun.c).
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../VERSION SB_NET_VERSION LIMIT_COUNT 1)
string(STRIP "${SB_NET_VERSION}" SB_NET_VERSION)
if(NOT SB_NET_VERSION MATCHES "^[A-Za-z0-9.+-]+$")
  message(FATAL_ERROR "poc-native/VERSION ungueltig (${SB_NET_VERSION})")
endif()
foreach(t sb_emu loc_net_selftest)
  target_compile_definitions(${t} PRIVATE "SB_NET_VERSION=\"${SB_NET_VERSION}\"")
endforeach()
# REL-DEFENDER-FP: Dateieigenschaften (VERSIONINFO) der ausgelieferten Client-Dateien, Version aus
# SB_VERSION bzw. poc-native/VERSION. Nur Ressource (passt in die bestehende .rsrc-Seite).
include(${CMAKE_CURRENT_SOURCE_DIR}/src/sb_verinfo.cmake)
sb_add_verinfo(sb_ldr "Steelbound client loader")
sb_add_verinfo(sb_emu "Steelbound client core")

# W116: Edition an alle C-Ziele (nur community setzt etwas)
if(SB_EDITION_COMMUNITY)
  foreach(t sb_ldr sb_emu sbc_probe input_selftest render_selftest iso_vfs_check)
    target_compile_definitions(${t} PRIVATE SB_EDITION_COMMUNITY=1)
    target_link_options(${t} PRIVATE "/PDBALTPATH:%_PDB%")
    if(SB_LOC_PLAY)
      target_compile_definitions(${t} PRIVATE SB_LOC_PLAY=1)
    endif()
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

# W210: synthetic offscreen capture, MF encoder and fallback (no game).
add_executable(clip_selftest tests/clip_selftest.c src/render_clip.c src/render_dsc.c)
target_include_directories(clip_selftest PRIVATE src)
target_compile_options(clip_selftest PRIVATE /W3)

# W255: Video-Export - Zeitachse, Farbwandlung, synthetische MP4 (kein Spiel, kein Fenster).
add_executable(vexp_selftest tests/vexp_selftest.c src/vexp_enc.c)
target_include_directories(vexp_selftest PRIVATE src)
target_compile_options(vexp_selftest PRIVATE /W3)
target_link_options(vexp_selftest PRIVATE /LARGEADDRESSAWARE) # wie sb_ldr (4-GB-Adressraum)
