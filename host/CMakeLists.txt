# Vitrine's QEMU: what builds it, installed where the app looks for it
# (share/vitrine/host); a build tree's app uses the source tree's host/
include(GNUInstallDirs)
set(VITRINE_HOST_DESTINATION ${CMAKE_INSTALL_DATADIR}/vitrine/host)
install(PROGRAMS build.sh DESTINATION ${VITRINE_HOST_DESTINATION})
install(FILES versions.conf DESTINATION ${VITRINE_HOST_DESTINATION})
install(DIRECTORY patches/ DESTINATION ${VITRINE_HOST_DESTINATION}/patches
    FILES_MATCHING PATTERN "*.patch" PATTERN "README.md")
