# Vitrine's QEMU: what builds it, installed where the app looks for it
# (share/vitrine/host); a build tree's app uses the source tree's host/
include(GNUInstallDirs)
set(VITRINE_HOST_DESTINATION ${CMAKE_INSTALL_DATADIR}/vitrine/host)
install(PROGRAMS build.sh DESTINATION ${VITRINE_HOST_DESTINATION})
install(FILES versions.conf components.conf DESTINATION ${VITRINE_HOST_DESTINATION})
# build.sh applies every *.patch it finds, or those a series file names in
# its order (without it, another order), and installing never removes a
# file: a patch a newer Vitrine dropped would stay in an installation made
# over an older one, and fail to apply.  The folder goes first, under the
# install's DESTDIR (the prefix is the configured one: the top-level guard
# refuses another).
if(IS_ABSOLUTE "${VITRINE_HOST_DESTINATION}")
    set(vitrine_patches "${VITRINE_HOST_DESTINATION}/patches")
else()
    set(vitrine_patches "\${CMAKE_INSTALL_PREFIX}/${VITRINE_HOST_DESTINATION}/patches")
endif()
install(CODE "file(REMOVE_RECURSE \"\$ENV{DESTDIR}${vitrine_patches}\")")
install(DIRECTORY patches/ DESTINATION ${VITRINE_HOST_DESTINATION}/patches
    FILES_MATCHING PATTERN "*.patch" PATTERN "series" PATTERN "README.md")
# The host's KWin (kwin/README.md): its scripts, which keep their modes, and
# the research's recipe as Vitrine pins it.  The folder goes first too: its
# key sums up every file of research/, and a file a newer copy dropped would
# change it.
if(IS_ABSOLUTE "${VITRINE_HOST_DESTINATION}")
    set(vitrine_kwin "${VITRINE_HOST_DESTINATION}/kwin")
else()
    set(vitrine_kwin "\${CMAKE_INSTALL_PREFIX}/${VITRINE_HOST_DESTINATION}/kwin")
endif()
install(CODE "file(REMOVE_RECURSE \"\$ENV{DESTDIR}${vitrine_kwin}\")")
install(DIRECTORY kwin/ DESTINATION ${VITRINE_HOST_DESTINATION}/kwin USE_SOURCE_PERMISSIONS)
