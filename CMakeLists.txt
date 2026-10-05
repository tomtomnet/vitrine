cmake_minimum_required(VERSION 3.21)
project(vitrine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core)
qt_standard_project_setup()

option(BUILD_TESTING "Build the unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
endif()

include(GNUInstallDirs)
# where the root helper is installed: the polkit action and the app need it
set(VITRINE_HELPER_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/vitrine-helper")

# That path is built in now: an install under another prefix (cmake --install
# --prefix) would put a helper neither the app nor the polkit action points
# to.  Refused before anything is copied (install rules run in order).
string(REGEX REPLACE "(.)/+$" "\\1" VITRINE_CONFIGURED_PREFIX "${CMAKE_INSTALL_PREFIX}")
install(CODE "set(_vitrine_configured \"${VITRINE_CONFIGURED_PREFIX}\")")
install(CODE [[
    string(REGEX REPLACE "(.)/+$" "\\1" _vitrine_installing "${CMAKE_INSTALL_PREFIX}")
    if(NOT _vitrine_installing STREQUAL _vitrine_configured)
        message(FATAL_ERROR "vitrine is configured for ${_vitrine_configured}, where the app "
                "and the polkit action look for vitrine-helper: configure it again with "
                "-DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX} instead of installing with --prefix")
    endif()
]])

add_subdirectory(helper)
add_subdirectory(app)
add_subdirectory(host)
add_subdirectory(guest)
