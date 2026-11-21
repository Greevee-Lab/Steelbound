# sb_verinfo.cmake - REL-DEFENDER-FP (21.11.): VERSIONINFO-Ressource fuer ausgelieferte Binaerdateien.
#
# Unsignierte, frisch gebaute EXEs ohne Dateieigenschaften fallen ML-Heuristiken (Windows Defender
# "Bearfoos.A!ml") eher auf. Jede ausgelieferte Datei bekommt daher Produkt/Firma/Version/Beschreibung.
# Version automatisch: -DSB_VERSION (Release, make_alpha.py) gewinnt, sonst poc-native/VERSION mit
# Zusatz "-dev" (Entwickler-Build). Keine Handpflege; Aenderung an VERSION konfiguriert neu.
# Nur Ressourcen: Code, Daten und Abschnitts-Layout der Binaerdateien bleiben unveraendert (die
# VERSIONINFO passt in die bestehende .rsrc-Seite mit dem Linker-Manifest).
#
#   include(<pfad>/sb_verinfo.cmake)
#   sb_add_verinfo(<ziel> "<FileDescription>")     # nach der Definition von SB_VERSION aufrufen

set(SB_VI_TEMPLATE ${CMAKE_CURRENT_LIST_DIR}/sb_verinfo.rc.in)
set(SB_VI_VERSION_FILE ${CMAKE_CURRENT_LIST_DIR}/../../VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SB_VI_VERSION_FILE})

function(sb_add_verinfo target desc)
  set(ver "")
  if(EXISTS ${SB_VI_VERSION_FILE})
    file(STRINGS ${SB_VI_VERSION_FILE} ver LIMIT_COUNT 1)
    string(STRIP "${ver}" ver)
  endif()
  if(SB_VERSION)
    set(SB_VI_STR "${SB_VERSION}")
  elseif(ver)
    set(SB_VI_STR "${ver}-dev")
  else()
    message(WARNING "sb_verinfo: poc-native/VERSION fehlt - VERSIONINFO 0.0.0-dev")
    set(SB_VI_STR "0.0.0-dev")
  endif()
  if(NOT SB_VI_STR MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)([-+][A-Za-z0-9.+-]*)?$")
    message(FATAL_ERROR "sb_verinfo: Version '${SB_VI_STR}' ist kein x.y.z[-pre][+build]")
  endif()
  set(SB_VI_NUM "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")
  set(SB_VI_DESC "${desc}")
  get_target_property(ttype ${target} TYPE)
  if(ttype STREQUAL "SHARED_LIBRARY")
    set(SB_VI_FILETYPE "VFT_DLL")
    set(SB_VI_ORIG "${target}.dll")
  else()
    set(SB_VI_FILETYPE "VFT_APP")
    set(SB_VI_ORIG "${target}.exe")
  endif()
  set(SB_VI_INTERNAL "${target}")
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${target}_verinfo.rc)
  configure_file(${SB_VI_TEMPLATE} ${out} @ONLY NEWLINE_STYLE CRLF)
  target_sources(${target} PRIVATE ${out})
  message(STATUS "sb_verinfo: ${SB_VI_ORIG} ${SB_VI_STR} (${SB_VI_NUM})")
endfunction()
