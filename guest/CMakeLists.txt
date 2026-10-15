# The guest tools: what builds them (build-rpms.sh, build-medium.sh) and the
# sources of their packages, installed where the app looks for them
# (share/vitrine/guest); a build tree's app uses the source tree's guest/
include(GNUInstallDirs)
set(VITRINE_GUEST_DESTINATION ${CMAKE_INSTALL_DATADIR}/vitrine/guest)
# Installing never removes a file, and every file of a package's folder is
# among its inputs (build-rpms.sh's inputs_hash): a file a newer Vitrine
# dropped would stay in an installation made over an older one, and make the
# packages out of date for good.  The folder goes first, under the install's
# DESTDIR (the prefix is the configured one: the top-level guard refuses
# another).
if(IS_ABSOLUTE "${VITRINE_GUEST_DESTINATION}")
    set(vitrine_guest "${VITRINE_GUEST_DESTINATION}")
else()
    set(vitrine_guest "\${CMAKE_INSTALL_PREFIX}/${VITRINE_GUEST_DESTINATION}")
endif()
install(CODE "file(REMOVE_RECURSE \"\$ENV{DESTDIR}${vitrine_guest}\")")
install(PROGRAMS build-rpms.sh build-medium.sh rpm-build-inside.sh
    DESTINATION ${VITRINE_GUEST_DESTINATION})
install(FILES components.conf DESTINATION ${VITRINE_GUEST_DESTINATION})
# Every package's folder, as components.conf names them (spec=FOLDER/...): a
# list of its own here left the proxy and KPipeWire out of every installation
# until 2026-10-15, and its media came out without them (no nested desktop).
# The file changing configures again.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS components.conf)
file(STRINGS components.conf vitrine_guest_specs REGEX "^spec=")
set(vitrine_guest_packages)
foreach(spec IN LISTS vitrine_guest_specs)
    string(REGEX REPLACE "^spec=([^/]+)/.*$" "\\1" folder "${spec}")
    if(NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${folder}")
        message(FATAL_ERROR "guest/components.conf names ${spec}: no folder ${folder}")
    endif()
    list(APPEND vitrine_guest_packages ${folder})
endforeach()
list(REMOVE_DUPLICATES vitrine_guest_packages)
# the installer and the agent keep their modes
install(DIRECTORY ${vitrine_guest_packages} DESTINATION ${VITRINE_GUEST_DESTINATION}
    USE_SOURCE_PERMISSIONS)
