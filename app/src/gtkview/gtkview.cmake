# The VM display in GTK4 (src/gtkview/README.md), included by app/CMakeLists.txt
# after the Qt app: the research side's display client of src/vmview as it is,
# built without Qt GUI (src/gtkview/qtgui stands in for the few Qt GUI classes
# it uses), with the GTK glue; and vitrine-gtk-view, its test program.  Built
# when pkg-config finds gtkmm-4.0, GDK's Wayland part and libadwaita-1.  A
# target using it must never link Qt6::Gui (src/gtkview/qtgui/qtguicompat.h).

pkg_check_modules(GTKVIEW IMPORTED_TARGET gtkmm-4.0 gtk4-wayland libadwaita-1)
if(NOT GTKVIEW_FOUND)
    message(STATUS "gtkmm-4.0, gtk4-wayland or libadwaita-1 not found: no GTK view")
    return()
endif()

set(GTKVIEW_OWN_SOURCES
    src/gtkview/displayinput.cpp
    src/gtkview/displaysurface.cpp
    src/gtkview/displaywidget.cpp
    src/gtkview/pointerconstraints.cpp
    src/gtkview/qtgui/qtguicompat.cpp
    src/gtkview/vmdisplay.cpp)
add_library(vitrine_gtkview STATIC
    # the research side's files, as in vitrine_vmview
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
    src/gtkview/pointerconstraints.h
    src/gtkview/qtgui/qtguicompat.h
    src/gtkview/vmdisplay.h
    ${VMVIEW_PROTOCOL_SOURCES})
# <QOpenGLContext>, <QImage>...: src/gtkview/qtgui's, before anything else
target_include_directories(vitrine_gtkview BEFORE PUBLIC src/gtkview/qtgui)
target_include_directories(vitrine_gtkview PUBLIC src PRIVATE src/vmview ${VMVIEW_PROTOCOL_DIR})
target_link_libraries(vitrine_gtkview
    PUBLIC Qt6::Core PkgConfig::GTKVIEW
    PRIVATE PkgConfig::GIO PkgConfig::EGL PkgConfig::WAYLAND PkgConfig::GBM)
# GLib's headers use the names of Qt's signals/slots keywords
target_compile_definitions(vitrine_gtkview PRIVATE QT_NO_KEYWORDS)
set_source_files_properties(${GTKVIEW_OWN_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

add_executable(vitrine-gtk-view src/gtkview/main.cpp)
target_link_libraries(vitrine-gtk-view PRIVATE vitrine_gtkview)
target_compile_definitions(vitrine-gtk-view PRIVATE QT_NO_KEYWORDS)
target_compile_options(vitrine-gtk-view PRIVATE -Wall -Wextra)
