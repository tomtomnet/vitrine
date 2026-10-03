# The Qt application: the VM manager and its windows

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Concurrent DBus Gui Network Widgets)

# Everything but the windows, so that tests can use it
qt_add_library(vitrine_core STATIC
    src/core/argsfile.cpp
    src/core/argsfile.h
    src/core/diskinfo.cpp
    src/core/diskinfo.h
    src/core/firmware.cpp
    src/core/firmware.h
    src/core/firmwarefiles.cpp
    src/core/firmwarefiles.h
    src/core/gpucontexts.cpp
    src/core/gpucontexts.h
    src/core/guestagent.cpp
    src/core/guestagent.h
    src/core/hostdevices.cpp
    src/core/hostdevices.h
    src/core/hostmemory.cpp
    src/core/hostmemory.h
    src/core/importer.cpp
    src/core/importer.h
    src/core/optionvalue.cpp
    src/core/optionvalue.h
    src/core/paths.cpp
    src/core/paths.h
    src/core/perfstats.cpp
    src/core/perfstats.h
    src/core/qemubuilder.cpp
    src/core/qemubuilder.h
    src/core/qemuinfo.cpp
    src/core/qemuinfo.h
    src/core/qmpclient.cpp
    src/core/qmpclient.h
    src/core/snapshots.cpp
    src/core/snapshots.h
    src/core/updatecheck.cpp
    src/core/updatecheck.h
    src/core/vmconfig.cpp
    src/core/vmcloner.cpp
    src/core/vmcloner.h
    src/core/vmconfig.h
    src/core/vmhardware.cpp
    src/core/vmhardware.h
    src/core/vmrunner.cpp
    src/core/vmrunner.h
    src/core/vmstore.cpp
    src/core/vmstore.h
    src/core/vmtemplate.cpp
    src/core/vmtemplate.h
)
target_include_directories(vitrine_core PUBLIC src)
# the virglrenderer patches File > Build QEMU applies
qt_add_resources(vitrine_core virglpatches PREFIX / BASE data
    FILES data/patches/virglrenderer-amdgpu-force-wc.patch)
target_link_libraries(vitrine_core PUBLIC
    Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::Network)
target_compile_options(vitrine_core PRIVATE -Wall -Wextra)

# The VM display: QEMU's D-Bus display in a native Wayland surface. The
# client files in src/vmview come unchanged from the research side's D-Bus
# display client (embed-poc); see src/vmview/README.md.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Gui GuiPrivate Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm libdrm)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# Wayland protocols Qt does not expose: presentation feedback, dma-buf
# feedback, shortcut inhibition (keyboard grab), pointer lock and relative
# motion (mouse grab for guests without a tablet).
set(VMVIEW_PROTOCOLS
    stable/presentation-time/presentation-time.xml
    stable/linux-dmabuf/linux-dmabuf-v1.xml
    unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml
    unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
    unstable/relative-pointer/relative-pointer-unstable-v1.xml)
set(VMVIEW_PROTOCOL_DIR ${CMAKE_CURRENT_BINARY_DIR}/vmview-protocols)
set(VMVIEW_PROTOCOL_SOURCES)
foreach(xml ${VMVIEW_PROTOCOLS})
    get_filename_component(name ${xml} NAME_WE)
    set(hdr ${VMVIEW_PROTOCOL_DIR}/${name}-client-protocol.h)
    set(src ${VMVIEW_PROTOCOL_DIR}/${name}-protocol.c)
    add_custom_command(OUTPUT ${hdr} ${src}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${VMVIEW_PROTOCOL_DIR}
        COMMAND ${WAYLAND_SCANNER} client-header ${WAYLAND_PROTOCOLS_DIR}/${xml} ${hdr}
        COMMAND ${WAYLAND_SCANNER} private-code ${WAYLAND_PROTOCOLS_DIR}/${xml} ${src}
        DEPENDS ${WAYLAND_PROTOCOLS_DIR}/${xml})
    list(APPEND VMVIEW_PROTOCOL_SOURCES ${hdr} ${src})
endforeach()

qt_add_library(vitrine_vmview STATIC
    src/vmview/common.h
    src/vmview/dbusdisplay.cpp
    src/vmview/dbusdisplay.h
    src/vmview/displaywindow.cpp
    src/vmview/displaywindow.h
    src/vmview/framemailbox.h
    src/vmview/keymap_linux_to_qnum.h
    src/vmview/listener.cpp
    src/vmview/listener.h
    src/vmview/qmp.cpp
    src/vmview/qmp.h
    src/vmview/renderer.cpp
    src/vmview/renderer.h
    src/vmview/stats.cpp
    src/vmview/stats.h
    src/vmview/swapchain.cpp
    src/vmview/swapchain.h
    src/vmview/vmview.cpp
    src/vmview/vmview.h
    src/vmview/waylandextras.cpp
    src/vmview/waylandextras.h
    ${VMVIEW_PROTOCOL_SOURCES}
)
target_include_directories(vitrine_vmview PUBLIC src PRIVATE src/vmview ${VMVIEW_PROTOCOL_DIR})
target_link_libraries(vitrine_vmview
    PUBLIC Qt6::Gui Qt6::Widgets
    PRIVATE Qt6::GuiPrivate PkgConfig::GIO PkgConfig::EGL PkgConfig::WAYLAND PkgConfig::GBM)
# GLib's headers use the names of Qt's signals/slots keywords; Qt's GL headers
# come with Qt6::Gui and GL entry points are resolved at run time
target_compile_definitions(vitrine_vmview PRIVATE QT_NO_KEYWORDS)

qt_add_executable(vitrine
    src/main.cpp
    src/ui/argseditor.cpp
    src/ui/argseditor.h
    src/ui/banner.cpp
    src/ui/banner.h
    src/ui/clonedialog.cpp
    src/ui/clonedialog.h
    src/ui/firmwarerepair.cpp
    src/ui/firmwarerepair.h
    src/ui/icons.cpp
    src/ui/icons.h
    src/ui/importdialog.cpp
    src/ui/importdialog.h
    src/ui/logview.cpp
    src/ui/logview.h
    src/ui/mainwindow.cpp
    src/ui/mainwindow.h
    src/ui/memorymonitor.cpp
    src/ui/memorymonitor.h
    src/ui/newvmdialog.cpp
    src/ui/newvmdialog.h
    src/ui/perfmonitor.cpp
    src/ui/perfmonitor.h
    src/ui/preferencesdialog.cpp
    src/ui/preferencesdialog.h
    src/ui/qemubuilddialog.cpp
    src/ui/qemubuilddialog.h
    src/ui/qemudocs.cpp
    src/ui/qemudocs.h
    src/ui/referencepanel.cpp
    src/ui/referencepanel.h
    src/ui/settingspages.cpp
    src/ui/settingspages.h
    src/ui/snapshotview.cpp
    src/ui/snapshotview.h
    src/ui/textdialog.cpp
    src/ui/textdialog.h
    src/ui/uiconfig.cpp
    src/ui/uiconfig.h
    src/ui/updatenotifier.cpp
    src/ui/updatenotifier.h
    src/ui/usbaccess.cpp
    src/ui/usbaccess.h
    src/ui/vmconsole.cpp
    src/ui/vmconsole.h
    src/ui/vmdetails.cpp
    src/ui/vmdetails.h
    src/ui/vmpane.cpp
    src/ui/vmpane.h
    src/ui/vmwindow.cpp
    src/ui/vmwindow.h
    src/ui/widgets.cpp
    src/ui/widgets.h
)
qt_add_resources(vitrine icons PREFIX /icons BASE data
    FILES data/vitrine.svg)
target_link_libraries(vitrine PRIVATE vitrine_core vitrine_vmview Qt6::DBus Qt6::Widgets)
target_compile_definitions(vitrine PRIVATE VITRINE_VERSION="${PROJECT_VERSION}")

# The commit it is built from, for the update check; none outside a git checkout
set(VITRINE_COMMIT "")
find_package(Git QUIET)
if(GIT_FOUND AND IS_DIRECTORY "${CMAKE_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    OUTPUT_VARIABLE VITRINE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    # configured again when the checkout moves, as a git pull does
    set(git "${CMAKE_SOURCE_DIR}/.git")
    set(watched "${git}/HEAD")
    file(READ "${git}/HEAD" head)
    if(head MATCHES "^ref: ([^\n]+)" AND EXISTS "${git}/${CMAKE_MATCH_1}")
        list(APPEND watched "${git}/${CMAKE_MATCH_1}")
    endif()
    if(EXISTS "${git}/packed-refs")
        list(APPEND watched "${git}/packed-refs")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${watched})
endif()
# rewritten only when it changes: only what includes it builds again
configure_file(src/buildinfo.h.in buildinfo.h @ONLY)
target_include_directories(vitrine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(vitrine PRIVATE -Wall -Wextra)

include(GNUInstallDirs)
install(TARGETS vitrine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/vitrine.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/vitrine.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    foreach(name
            argsfile
            diskinfo
            firmware
            firmwarefiles
            gpucontexts
            guestagent
            hostdevices
            hostmemory
            importer
            optionvalue
            perfstats
            qemubuilder
            qemuinfo
            qmpclient
            snapshots
            updatecheck
            vmcloner
            vmconfig
            vmhardware
            vmrunner
            vmstore
            vmtemplate)
        qt_add_executable(test_${name} tests/test_${name}.cpp)
        target_link_libraries(test_${name} PRIVATE vitrine_core Qt6::Test)
        add_test(NAME ${name} COMMAND test_${name})
    endforeach()
endif()
