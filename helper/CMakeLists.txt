# vitrine-helper: host settings as root while VMs run (polkit, pkexec)

# C only, no Qt: small enough to read through
add_executable(vitrine-helper
    src/group.c
    src/helper.h
    src/main.c
    src/process.c
    src/settings.c
    src/sys.c)
set_target_properties(vitrine-helper PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(vitrine-helper PRIVATE -Wall -Wextra -fstack-protector-strong
    "$<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>")
target_link_options(vitrine-helper PRIVATE -Wl,-z,relro,-z,now)
set_target_properties(vitrine-helper PROPERTIES POSITION_INDEPENDENT_CODE ON)

configure_file(org.vitrine.helper.policy.in org.vitrine.helper.policy @ONLY)

# polkit reads the actions of /usr/share/polkit-1/actions and the rules of
# /usr/share/polkit-1/rules.d and /etc/polkit-1/rules.d only: an install
# elsewhere needs these set (or a package that puts them there)
set(VITRINE_POLKIT_ACTIONS_DIR "${CMAKE_INSTALL_DATADIR}/polkit-1/actions"
    CACHE STRING "Where the helper's polkit action goes")
set(VITRINE_POLKIT_RULES_DIR "${CMAKE_INSTALL_DATADIR}/polkit-1/rules.d"
    CACHE STRING "Where the polkit rule for the vitrine group goes")
if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr" AND NOT IS_ABSOLUTE "${VITRINE_POLKIT_ACTIONS_DIR}")
    message(STATUS "vitrine-helper: polkit reads its actions from /usr/share/polkit-1 only; "
                   "with the prefix ${CMAKE_INSTALL_PREFIX}, set VITRINE_POLKIT_ACTIONS_DIR "
                   "and VITRINE_POLKIT_RULES_DIR to install a working helper")
endif()

install(TARGETS vitrine-helper RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.vitrine.helper.policy
    DESTINATION ${VITRINE_POLKIT_ACTIONS_DIR})
install(FILES 49-vitrine.rules DESTINATION ${VITRINE_POLKIT_RULES_DIR})

if(BUILD_TESTING)
    # The same helper on a fake tree that plays the kernel's part; never installed
    add_executable(vitrine-helper-fake
        src/group.c
        src/helper.h
        src/main.c
        src/process.c
        src/settings.c
        tests/fakesys.c)
    set_target_properties(vitrine-helper-fake PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    target_compile_options(vitrine-helper-fake PRIVATE -Wall -Wextra)

    find_package(Threads REQUIRED)
    find_package(Qt6 REQUIRED COMPONENTS Core Test)

    # a QEMU to watch: the name is what the helper checks
    add_executable(fake-qemu tests/fakeqemu.c)
    set_target_properties(fake-qemu PROPERTIES OUTPUT_NAME qemu-system-x86_64
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fake-qemu)
    target_link_libraries(fake-qemu PRIVATE Threads::Threads)

    qt_add_executable(test_helper tests/test_helper.cpp)
    target_link_libraries(test_helper PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(test_helper PRIVATE
        HELPER_FAKE="$<TARGET_FILE:vitrine-helper-fake>"
        FAKE_QEMU="$<TARGET_FILE:fake-qemu>"
        HELPER_PATH="${VITRINE_HELPER_PATH}"
        HELPER_POLICY="${CMAKE_CURRENT_BINARY_DIR}/org.vitrine.helper.policy"
        HELPER_RULES="${CMAKE_CURRENT_SOURCE_DIR}/49-vitrine.rules"
        TEST_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-work")
    add_dependencies(test_helper vitrine-helper-fake fake-qemu)
    add_test(NAME helper COMMAND test_helper)
endif()
