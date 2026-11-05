# The GTK4 + libadwaita app, vitrine-gtk: the windows of src/gtk on the
# core the Qt app runs on (vitrine_core, Qt Core without Qt GUI), in one
# main loop (src/gtk/gtkloop.h).  Built when pkg-config finds gtkmm-4.0 and
# libadwaita-1 (libadwaita through its C API: there is no C++ binding).

pkg_check_modules(VITRINE_GTKMM QUIET IMPORTED_TARGET gtkmm-4.0)
pkg_check_modules(VITRINE_ADW QUIET IMPORTED_TARGET libadwaita-1)
if(VITRINE_GTKMM_FOUND AND VITRINE_ADW_FOUND)
    set(VITRINE_GTK_FOUND ON)
else()
    set(VITRINE_GTK_FOUND OFF)
endif()
option(VITRINE_GTK "Build vitrine-gtk, the GTK4 app (gtkmm-4.0, libadwaita-1)" ${VITRINE_GTK_FOUND})
if(NOT VITRINE_GTK)
    return()
endif()
if(NOT VITRINE_GTK_FOUND)
    message(FATAL_ERROR "VITRINE_GTK needs gtkmm-4.0 and libadwaita-1, which pkg-config does "
            "not find: install their development files, or configure with -DVITRINE_GTK=OFF")
endif()

# The VM display (src/gtkview): its library, vitrine_gtkview, and its test
# program, vitrine-gtk-view
include(src/gtkview/gtkview.cmake)

# The icon of a build tree's app, which is not installed: by the name GNOME
# knows it by, for the icon theme to find beside the program
configure_file(data/vitrine.svg ${CMAKE_CURRENT_BINARY_DIR}/gtk-icons/org.vitrine.Vitrine.svg
               COPYONLY)

# The windows, a library so that the tests can use them.  No QObject class
# of its own: Qt's connections go to context objects (no moc over GTK's
# headers, which use Qt's keywords as names: src/gtk/gtkbase.h)
add_library(vitrine_gtk STATIC
    src/gtk/argspage.cpp
    src/gtk/argspage.h
    src/gtk/clonedialog.cpp
    src/gtk/clonedialog.h
    src/gtk/desktopgpu.cpp
    src/gtk/desktopgpu.h
    src/gtk/dialog.cpp
    src/gtk/dialog.h
    src/gtk/displaypage.cpp
    src/gtk/displaypage.h
    src/gtk/gtkbase.h
    src/gtk/gtkloop.cpp
    src/gtk/gtkloop.h
    src/gtk/guarded.h
    src/gtk/hostprefs.cpp
    src/gtk/hostprefs.h
    src/gtk/importdialog.cpp
    src/gtk/importdialog.h
    src/gtk/logdialog.cpp
    src/gtk/logdialog.h
    src/gtk/machinepage.cpp
    src/gtk/machinepage.h
    src/gtk/mainwindow.cpp
    src/gtk/mainwindow.h
    src/gtk/mainwindowconsole.cpp
    src/gtk/mainwindowdialogs.cpp
    src/gtk/mainwindowparts.cpp
    src/gtk/newvmdialog.cpp
    src/gtk/newvmdialog.h
    src/gtk/notices.cpp
    src/gtk/notices.h
    src/gtk/preferencesdialog.cpp
    src/gtk/preferencesdialog.h
    src/gtk/qemureference.cpp
    src/gtk/qemureference.h
    src/gtk/referencepanel.cpp
    src/gtk/referencepanel.h
    src/gtk/richtext.cpp
    src/gtk/richtext.h
    src/gtk/rows.cpp
    src/gtk/rows.h
    src/gtk/settingsdialog.cpp
    src/gtk/settingsdialog.h
    src/gtk/settingspage.cpp
    src/gtk/settingspage.h
    src/gtk/settingspages.cpp
    src/gtk/settingspages.h
    src/gtk/sharespage.cpp
    src/gtk/sharespage.h
    src/gtk/statsbutton.cpp
    src/gtk/statsbutton.h
    src/gtk/snapshotsdialog.cpp
    src/gtk/snapshotsdialog.h
    src/gtk/storagepage.cpp
    src/gtk/storagepage.h
    src/gtk/taskbutton.cpp
    src/gtk/taskbutton.h
    src/gtk/textdialog.cpp
    src/gtk/textdialog.h
    src/gtk/vmconsole.cpp
    src/gtk/vmconsole.h
    src/gtk/vmpage.cpp
    src/gtk/vmpage.h
    src/gtk/vmsummary.cpp
    src/gtk/vmsummary.h
    src/gtk/widgets.cpp
    src/gtk/widgets.h
)
target_include_directories(vitrine_gtk PUBLIC src)
# the VM's screen and the nested desktop (vitrine_gtkview), the desktop's GPU
# from linux-dmabuf over GDK's connection (vitrine_wlprotocols)
target_link_libraries(vitrine_gtk
    PUBLIC vitrine_core vitrine_uishared vitrine_gtkview PkgConfig::VITRINE_GTKMM PkgConfig::VITRINE_ADW
    PRIVATE vitrine_wlprotocols PkgConfig::WAYLAND)
target_compile_definitions(vitrine_gtk PRIVATE
    VITRINE_VERSION="${PROJECT_VERSION}"
    VITRINE_GTK_BUILD_ICONS="${CMAKE_CURRENT_BINARY_DIR}/gtk-icons")
target_compile_options(vitrine_gtk PRIVATE -Wall -Wextra)
set_target_properties(vitrine_gtk PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

add_executable(vitrine-gtk src/gtk/main.cpp)
target_link_libraries(vitrine-gtk PRIVATE vitrine_gtk)
target_compile_definitions(vitrine-gtk PRIVATE
    VITRINE_VERSION="${PROJECT_VERSION}"
    VITRINE_GTK_BUILD_ICONS="${CMAKE_CURRENT_BINARY_DIR}/gtk-icons")
target_compile_options(vitrine-gtk PRIVATE -Wall -Wextra)
set_target_properties(vitrine-gtk PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

install(TARGETS vitrine-gtk RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/org.vitrine.Vitrine.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/vitrine.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    RENAME org.vitrine.Vitrine.svg)

if(BUILD_TESTING)
    # GTK on a display of its own that no one sees (broadway), on a session
    # bus of its own, never the desktop's: what a test window would open on
    # otherwise, as ctest keeps the caller's DISPLAY and WAYLAND_DISPLAY
    function(vitrine_gtk_test name)
        add_executable(test_${name} tests/test_${name}.cpp)
        target_link_libraries(test_${name} PRIVATE vitrine_gtk Qt6::Test ${ARGN})
        target_compile_options(test_${name} PRIVATE -Wall -Wextra)
        add_test(NAME ${name} COMMAND test_${name})
        vitrine_test_env(${name} GDK_BACKEND=broadway GTK_A11Y=none GSETTINGS_BACKEND=memory
                         ADW_DISABLE_PORTAL=1 GDK_DEBUG=no-portals)
        set_property(TEST ${name} PROPERTY ENVIRONMENT_MODIFICATION
                     DISPLAY=unset: WAYLAND_DISPLAY=unset: DBUS_SESSION_BUS_ADDRESS=unset:)
    endfunction()
    vitrine_gtk_test(gtkloop Qt6::Concurrent)
    # what the windows say of a VM: words alone, no display needed
    vitrine_gtk_test(vmsummary)
    vitrine_gtk_test(qemureference)
    vitrine_gtk_test(gtkmainwindow)
    vitrine_gtk_test(gtkdialogs)
    # the consoles: views, keys, a dialog over them, what closing says (a stand-in
    # for the nested desktop)
    vitrine_gtk_test(gtkconsole)
    target_sources(test_gtkloop PRIVATE tests/gtkdisplay.h)
    target_sources(test_gtkmainwindow PRIVATE tests/gtkdisplay.h)
    target_sources(test_gtkdialogs PRIVATE tests/gtkdisplay.h)
    target_sources(test_gtkconsole PRIVATE tests/gtkdisplay.h)
    # Preferences and the notices: a fake host (GPUs, gpu-input-awake's root, pkexec and
    # systemctl stand-ins), the stand-in QEMU for a VM found running
    vitrine_gtk_test(gtkprefs)
    target_sources(test_gtkprefs PRIVATE tests/gtkdisplay.h tests/fakegpus.h)
    target_compile_definitions(test_gtkprefs PRIVATE
        FAKE_QEMU="$<TARGET_FILE:fake-qemu>"
        GPU_AWAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}/host/gpu-input-awake")
    add_dependencies(test_gtkprefs fake-qemu)
    # the tasks' button: its texts, the popover, the ring alone, a success that lingers
    vitrine_gtk_test(gtktasks)
    target_sources(test_gtktasks PRIVATE tests/gtkdisplay.h)
    # the statistics: read only while they show, a VM found running (the stand-in QEMU)
    vitrine_gtk_test(gtkstats)
    target_sources(test_gtkstats PRIVATE tests/gtkdisplay.h)
    target_compile_definitions(test_gtkstats PRIVATE FAKE_QEMU="$<TARGET_FILE:fake-qemu>")
    add_dependencies(test_gtkstats fake-qemu)
endif()
