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

add_subdirectory(helper)
add_subdirectory(app)
add_subdirectory(host)
