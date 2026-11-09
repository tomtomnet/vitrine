# The VM display in GTK4 (src/gtkview/README.md), included by gtk.cmake under
# its switch, VITRINE_GTK: the research side's display client of src/vmview as
# it is, built without Qt GUI (src/gtkview/qtgui stands in for the few Qt GUI
# classes it uses), with the GTK glue; the nested desktop on GDK's connection
# (src/nested's library, nestedvmview); and vitrine-gtk-view, its test program.
# A target using it must never link Qt6::Gui (src/gtkview/qtgui/qtguicompat.h).

# GDK's Wayland part: gdk/wayland/gdkwayland.h
pkg_check_modules(VITRINE_GDK_WAYLAND QUIET IMPORTED_TARGET gtk4-wayland)
if(NOT VITRINE_GDK_WAYLAND_FOUND)
    message(FATAL_ERROR "VITRINE_GTK needs GDK's Wayland part (gtk4-wayland), which "
            "pkg-config does not find: install GTK 4's development files, or configure "
            "with -DVITRINE_GTK=OFF")
endif()

set(GTKVIEW_OWN_SOURCES
    src/gtkview/displayinput.cpp
    src/gtkview/displaysurface.cpp
    src/gtkview/displaywidget.cpp
    src/gtkview/framestill.cpp
    src/gtkview/fullscreenorder.cpp
    src/gtkview/gtkclipboard.cpp
    src/gtkview/nesteddisplay.cpp
    src/gtkview/pointerconstraints.cpp
    src/gtkview/qtgui/qtguicompat.cpp
    src/gtkview/shortcutsinhibit.cpp
    src/gtkview/viewthreads.cpp
    src/gtkview/vmdisplay.cpp)
add_library(vitrine_gtkview STATIC
    # the research side's files, as in vitrine_vmview, and what goes where
    # between the host's clipboard and the guest's, vitrine's own
    src/vmview/clipboardstate.cpp
    src/vmview/clipboardstate.h
    src/vmview/common.h
    src/vmview/dbusdisplay.cpp
    src/vmview/dbusdisplay.h
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
    # the GTK glue
    ${GTKVIEW_OWN_SOURCES}
    src/gtkview/displayinput.h
    src/gtkview/displaysurface.h
    src/gtkview/displaywidget.h
    src/gtkview/framestill.h
    src/gtkview/fullscreenorder.h
    src/gtkview/gtkclipboard.h
    src/gtkview/nesteddisplay.h
    src/gtkview/pointerconstraints.h
    src/gtkview/qtgui/qtguicompat.h
    src/gtkview/shortcutsinhibit.h
    src/gtkview/viewkeys.h
    src/gtkview/viewthreads.h
    src/gtkview/vmdisplay.h
    src/gtkview/widgetrelease.h)
# <QOpenGLContext>, <QImage>...: src/gtkview/qtgui's, before anything else
target_include_directories(vitrine_gtkview BEFORE PUBLIC src/gtkview/qtgui)
target_include_directories(vitrine_gtkview PUBLIC src PRIVATE src/vmview)
target_link_libraries(vitrine_gtkview
    PUBLIC Qt6::Core PkgConfig::VITRINE_GTKMM PkgConfig::VITRINE_ADW PkgConfig::VITRINE_GDK_WAYLAND
    PRIVATE PkgConfig::GIO PkgConfig::EGL PkgConfig::WAYLAND PkgConfig::GBM vitrine_wlprotocols
            nestedvmview)
# GLib's headers use the names of Qt's signals/slots keywords
target_compile_definitions(vitrine_gtkview PRIVATE QT_NO_KEYWORDS)
set_source_files_properties(${GTKVIEW_OWN_SOURCES} src/vmview/clipboardstate.cpp
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

add_executable(vitrine-gtk-view src/gtkview/main.cpp)
target_link_libraries(vitrine-gtk-view PRIVATE vitrine_gtkview)
target_compile_definitions(vitrine-gtk-view PRIVATE QT_NO_KEYWORDS)
target_compile_options(vitrine-gtk-view PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
    # headless: the input logic, and the Qt GUI stand-ins (a real context on
    # the render node when there is one)
    foreach(name displayinput qtguicompat viewthreads)
        add_executable(test_${name} tests/test_${name}.cpp)
        target_link_libraries(test_${name} PRIVATE vitrine_gtkview Qt6::Test)
        target_compile_definitions(test_${name} PRIVATE QT_NO_KEYWORDS)
        target_compile_options(test_${name} PRIVATE -Wall -Wextra)
        add_test(NAME ${name} COMMAND test_${name})
        vitrine_test_env(${name})
    endforeach()
    target_link_libraries(test_qtguicompat PRIVATE PkgConfig::EGL PkgConfig::GBM)
    target_link_libraries(test_viewthreads PRIVATE PkgConfig::EGL PkgConfig::GBM)
    # the full-screen window's turn, on broadway (a display no one sees) and a
    # session bus of its own, as the app's GTK tests
    add_executable(test_fullscreenorder tests/test_fullscreenorder.cpp tests/gtkdisplay.h)
    target_link_libraries(test_fullscreenorder PRIVATE vitrine_gtkview Qt6::Test)
    target_compile_definitions(test_fullscreenorder PRIVATE QT_NO_KEYWORDS)
    target_compile_options(test_fullscreenorder PRIVATE -Wall -Wextra)
    add_test(NAME fullscreenorder COMMAND test_fullscreenorder)
    vitrine_test_env(fullscreenorder GDK_BACKEND=broadway GTK_A11Y=none GSETTINGS_BACKEND=memory
                     ADW_DISABLE_PORTAL=1 GDK_DEBUG=no-portals)
    set_property(TEST fullscreenorder PROPERTY ENVIRONMENT_MODIFICATION
                 DISPLAY=unset: WAYLAND_DISPLAY=unset: DBUS_SESSION_BUS_ADDRESS=unset:)
endif()
