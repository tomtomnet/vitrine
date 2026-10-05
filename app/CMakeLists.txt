# The Qt application: the VM manager and its windows

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Concurrent DBus Gui Network Widgets)

# Everything but the windows, so that tests can use it
qt_add_library(vitrine_core STATIC
    src/core/argsfile.cpp
    src/core/argsfile.h
    src/core/cardupdate.cpp
    src/core/cardupdate.h
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
    src/core/guestos.cpp
    src/core/guestos.h
    src/core/guesttools.cpp
    src/core/guesttools.h
    src/core/hostdevices.cpp
    src/core/hostdevices.h
    src/core/hostkvm.cpp
    src/core/hostkvm.h
    src/core/hostmemory.cpp
    src/core/hostmemory.h
    src/core/hostsettings.cpp
    src/core/hostsettings.h
    src/core/importer.cpp
    src/core/importer.h
    src/core/optionvalue.cpp
    src/core/optionvalue.h
    src/core/paths.cpp
    src/core/paths.h
    src/core/perfstats.cpp
    src/core/perfstats.h
    src/core/qemuinfo.cpp
    src/core/qemuinfo.h
    src/core/qmpclient.cpp
    src/core/qmpclient.h
    src/core/snapshots.cpp
    src/core/snapshots.h
    src/core/stackbuilder.cpp
    src/core/stackbuilder.h
    src/core/udmabuf.cpp
    src/core/udmabuf.h
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
# host/ of the source tree: what a build tree's app builds its QEMU with
target_compile_definitions(vitrine_core PRIVATE
    VITRINE_HOST_SOURCE_DIR="${PROJECT_SOURCE_DIR}/host"
    # the root helper the host settings go through (helper/)
    VITRINE_HELPER_PATH="${VITRINE_HELPER_PATH}")
# what the runner passes a guest to install the guest tools at boot
qt_add_resources(vitrine_core guestbootstrap PREFIX /guest BASE ../guest
    FILES ../guest/bootstrap/vitrine-tools-bootstrap.service
          ../guest/bootstrap/multi-user.target.conf)
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
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-egl)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm libdrm)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# Wayland protocols Qt does not expose: presentation feedback, dma-buf
# feedback, shortcut inhibition (keyboard grab), pointer lock and relative
# motion (mouse grab for guests without a tablet), and the viewport of the
# view's own surface.
set(VMVIEW_PROTOCOLS
    stable/presentation-time/presentation-time.xml
    stable/linux-dmabuf/linux-dmabuf-v1.xml
    stable/viewporter/viewporter.xml
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
    src/vmview/vmclipboard.cpp
    src/vmview/vmclipboard.h
    src/vmview/viewsurface.cpp
    src/vmview/viewsurface.h
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
# vitrine's own files there; the research side's build as they come
set_source_files_properties(src/vmview/vmview.cpp src/vmview/vmclipboard.cpp
    src/vmview/viewsurface.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# The windows, a library of their own so that tests can use them
qt_add_library(vitrine_ui STATIC
    src/ui/argseditor.cpp
    src/ui/argseditor.h
    src/ui/banner.cpp
    src/ui/banner.h
    src/ui/cardupdatedialog.cpp
    src/ui/cardupdatedialog.h
    src/ui/clonedialog.cpp
    src/ui/clonedialog.h
    src/ui/firmwarerepair.cpp
    src/ui/firmwarerepair.h
    src/ui/guesttoolsdialog.cpp
    src/ui/guesttoolsdialog.h
    src/ui/hosttuningnotifier.cpp
    src/ui/hosttuningnotifier.h
    src/ui/hosttuningprefs.cpp
    src/ui/hosttuningprefs.h
    src/ui/icons.cpp
    src/ui/icons.h
    src/ui/importdialog.cpp
    src/ui/importdialog.h
    src/ui/killprompt.cpp
    src/ui/killprompt.h
    src/ui/logview.cpp
    src/ui/logview.h
    src/ui/mainbar.cpp
    src/ui/mainbar.h
    src/ui/mainwindow.cpp
    src/ui/mainwindow.h
    src/ui/memorymonitor.cpp
    src/ui/memorymonitor.h
    src/ui/newvmdialog.cpp
    src/ui/newvmdialog.h
    src/ui/oschooser.cpp
    src/ui/oschooser.h
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
    src/ui/systems.cpp
    src/ui/systems.h
    src/ui/stackbanner.cpp
    src/ui/stackbanner.h
    src/ui/textdialog.cpp
    src/ui/textdialog.h
    src/ui/udmabufnotifier.cpp
    src/ui/udmabufnotifier.h
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
target_link_libraries(vitrine_ui PUBLIC vitrine_core vitrine_vmview Qt6::DBus Qt6::Widgets)
# the systems' logos (data/os/README.md: where they come from, their licenses)
qt_add_resources(vitrine_ui oslogos PREFIX /os BASE data/os FILES
    data/os/almalinux.svg
    data/os/alpinelinux.svg
    data/os/apple.svg
    data/os/archlinux.svg
    data/os/cachyos.svg
    data/os/centos.svg
    data/os/elementary.svg
    data/os/endeavouros.svg
    data/os/freebsd.svg
    data/os/kalilinux.svg
    data/os/kde.svg
    data/os/linux.svg
    data/os/linuxmint.svg
    data/os/manjaro.svg
    data/os/netbsd.svg
    data/os/openbsd.svg
    data/os/opensuse.svg
    data/os/popos.svg
    data/os/redhat.svg
    data/os/slackware.svg
    data/os/suse.svg
    data/os/ubuntu.svg
    data/os/voidlinux.svg
    data/os/windows.svg
    data/os/zorin.svg)
target_compile_options(vitrine_ui PRIVATE -Wall -Wextra)

qt_add_executable(vitrine
    src/main.cpp
)
qt_add_resources(vitrine icons PREFIX /icons BASE data
    FILES data/vitrine.svg)
target_link_libraries(vitrine PRIVATE vitrine_ui)
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
target_include_directories(vitrine_ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(vitrine PRIVATE -Wall -Wextra)

include(GNUInstallDirs)
install(TARGETS vitrine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/vitrine.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/vitrine.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    # Each test runs with a home of its own: Qt's test mode puts the settings
    # in ~/.qttest, one file for every test, which ctest -j runs side by side
    # (one test's reset or qemu/binary would be another's mid-run).  And with
    # a runtime folder of its own, short and out of the build tree, which
    # Qt creates (0700): the sockets of the VMs the tests start go there, and
    # must fit in sun_path (107 bytes) wherever the build tree is; one per
    # build tree, as builds in several worktrees may test at once.  ctest
    # gives both: a test binary run by hand has the caller's home and runtime
    # folder (ctest -V shows each test's environment).
    execute_process(COMMAND id -u OUTPUT_VARIABLE uid OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(SHA1 tree "${CMAKE_BINARY_DIR}")
    string(SUBSTRING "${tree}" 0 8 tree)
    function(vitrine_test_env name)
        set(home ${CMAKE_CURRENT_BINARY_DIR}/test-home/${name})
        file(MAKE_DIRECTORY ${home})
        set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
            "HOME=${home}" "XDG_RUNTIME_DIR=/tmp/vitrine-test-${uid}-${tree}-${name}" ${ARGN})
    endfunction()
    foreach(name
            argsfile
            cardupdate
            diskinfo
            firmware
            firmwarefiles
            gpucontexts
            guestagent
            guestos
            guesttools
            hostdevices
            hostkvm
            hostmemory
            hostsettings
            importer
            optionvalue
            perfstats
            qemuinfo
            qmpclient
            snapshots
            stackbuilder
            udmabuf
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
        vitrine_test_env(${name})
    endforeach()
    # the windows': widgets that are never shown, so no screen either
    foreach(name
            cardupdatedialog
            guestoffer
            mainbar
            newvmdialog
            oschooser
            settingspages
            shortcuts
            snapshotview
            stackbanner
            udmabufnotifier
            vmdetails)
        qt_add_executable(test_${name} tests/test_${name}.cpp)
        target_link_libraries(test_${name} PRIVATE vitrine_ui Qt6::Test)
        add_test(NAME ${name} COMMAND test_${name})
        vitrine_test_env(${name} QT_QPA_PLATFORM=offscreen)
    endforeach()
    # the windows at every scale Wayland gives (n/120, tests/scales.h), one
    # after the other: Qt's global factor, QT_SCALE_FACTOR's, is private
    foreach(name
            banner
            dialogs
            icons
            mainwindow
            systems)
        qt_add_executable(test_${name} tests/test_${name}.cpp tests/scales.h)
        target_link_libraries(test_${name} PRIVATE vitrine_ui Qt6::GuiPrivate Qt6::Test)
        target_compile_options(test_${name} PRIVATE -Wall -Wextra)
        add_test(NAME ${name} COMMAND test_${name})
        vitrine_test_env(${name} QT_QPA_PLATFORM=offscreen)
        # and a few set at start by QT_SCALE_FACTOR, as a user would: the same
        foreach(scale 1.15 1.3 1.7 2.35)
            add_test(NAME ${name}-${scale} COMMAND test_${name})
            vitrine_test_env(${name}-${scale} QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=${scale})
        endforeach()
    endforeach()
    # the helper's test build on a fake tree, and its QEMU stand-in (helper/)
    target_compile_definitions(test_hostsettings PRIVATE
        HELPER_FAKE="$<TARGET_FILE:vitrine-helper-fake>"
        FAKE_QEMU="$<TARGET_FILE:fake-qemu>"
        VITRINE_HELPER_PATH="${VITRINE_HELPER_PATH}"
        TEST_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-work")
    add_dependencies(test_hostsettings vitrine-helper-fake fake-qemu)
    # the same stand-in, for QEMUs found running
    target_compile_definitions(test_vmrunner PRIVATE FAKE_QEMU="$<TARGET_FILE:fake-qemu>")
    add_dependencies(test_vmrunner fake-qemu)
    target_compile_definitions(test_udmabuf PRIVATE FAKE_QEMU="$<TARGET_FILE:fake-qemu>")
    add_dependencies(test_udmabuf fake-qemu)
    target_compile_definitions(test_udmabufnotifier PRIVATE FAKE_QEMU="$<TARGET_FILE:fake-qemu>")
    add_dependencies(test_udmabufnotifier fake-qemu)

    # VmView against a stand-in for QEMU's display (QMP and D-Bus), offscreen
    qt_add_executable(test_vmview tests/test_vmview.cpp)
    # Qt6::GuiPrivate: the pointer delivered as a compositor does (QWindowSystemInterface)
    target_link_libraries(test_vmview PRIVATE vitrine_vmview PkgConfig::GIO Qt6::GuiPrivate
                                              Qt6::Test Qt6::Widgets)
    target_compile_definitions(test_vmview PRIVATE QT_NO_KEYWORDS)
    target_compile_options(test_vmview PRIVATE -Wall -Wextra)
    add_test(NAME vmview COMMAND test_vmview)
    # freed memory filled and not cached per thread: a use after free shows
    vitrine_test_env(vmview
        "GLIBC_TUNABLES=glibc.malloc.tcache_count=0:glibc.malloc.perturb=165")

    # the clipboard shared with the guest, against a stand-in for QEMU's side
    qt_add_executable(test_vmclipboard tests/test_vmclipboard.cpp)
    target_link_libraries(test_vmclipboard PRIVATE vitrine_vmview PkgConfig::GIO Qt6::Test Qt6::Widgets)
    target_compile_definitions(test_vmclipboard PRIVATE QT_NO_KEYWORDS)
    target_compile_options(test_vmclipboard PRIVATE -Wall -Wextra)
    add_test(NAME vmclipboard COMMAND test_vmclipboard)
    set_tests_properties(vmclipboard PROPERTIES ENVIRONMENT
        "GLIBC_TUNABLES=glibc.malloc.tcache_count=0:glibc.malloc.perturb=165")
endif()
