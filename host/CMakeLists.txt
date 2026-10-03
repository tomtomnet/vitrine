# Vitrine's QEMU: what builds it, installed where the app looks for it
# (share/vitrine/host); a build tree's app uses the source tree's host/
include(GNUInstallDirs)
set(VITRINE_HOST_DESTINATION ${CMAKE_INSTALL_DATADIR}/vitrine/host)
install(PROGRAMS build.sh DESTINATION ${VITRINE_HOST_DESTINATION})
install(FILES versions.conf DESTINATION ${VITRINE_HOST_DESTINATION})
# build.sh applies every *.patch it finds, and installing never removes a
# file: a patch a newer Vitrine dropped would stay in an installation made
# over an older one, and fail to apply.  The folder goes first, under the
# prefix of the install itself (cmake --install --prefix, DESTDIR), which
# the generated script tells, not the one configured.
if(IS_ABSOLUTE "${VITRINE_HOST_DESTINATION}")
    set(vitrine_patches "${VITRINE_HOST_DESTINATION}/patches")
else()
    set(vitrine_patches "\${CMAKE_INSTALL_PREFIX}/${VITRINE_HOST_DESTINATION}/patches")
endif()
install(CODE "file(REMOVE_RECURSE \"\$ENV{DESTDIR}${vitrine_patches}\")")
install(DIRECTORY patches/ DESTINATION ${VITRINE_HOST_DESTINATION}/patches
    FILES_MATCHING PATTERN "*.patch" PATTERN "README.md")
