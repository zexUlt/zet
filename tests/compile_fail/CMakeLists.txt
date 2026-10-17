# Compile-fail tests, per docs/design.md §16 and ADR 025.
#
# Each file is built twice. As it stands it must compile: it is part of the
# normal build, so a file that rots — a renamed type, a missing include —
# breaks the build instead of passing for the wrong reason. With
# ZET_EXPECT_FAIL it switches on one forbidden line and must not compile, and
# the test passes only when the compiler names the reason the file expects.
# A bare WILL_FAIL would also pass on a typo in that line.

# zet_add_compile_fail(<name> <regex>) — tests <name>.cpp. <regex> is matched
# against the build output of the failing variant.
function(zet_add_compile_fail name regex)
    add_library(${name}_compiles OBJECT ${name}.cpp)
    target_link_libraries(${name}_compiles PRIVATE zet::session)

    add_library(${name}_fails OBJECT EXCLUDE_FROM_ALL ${name}.cpp)
    target_link_libraries(${name}_fails PRIVATE zet::session)
    target_compile_definitions(${name}_fails PRIVATE ZET_EXPECT_FAIL)
    # Out of compile_commands.json: clang-tidy reads the first entry it finds
    # for a file, and this one is meant not to compile.
    set_target_properties(${name}_fails PROPERTIES EXPORT_COMPILE_COMMANDS OFF)

    add_test(
        NAME compile_fail.${name}
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                --target ${name}_fails
    )
    set_tests_properties(compile_fail.${name} PROPERTIES
        PASS_REGULAR_EXPRESSION "${regex}"
        # Two of these may not build the same object at once.
        RESOURCE_LOCK compile_fail
    )
endfunction()

zet_add_compile_fail(handle_from_nowhere "calling a private constructor")
zet_add_compile_fail(ref_reaches_session "is a private member")
zet_add_compile_fail(ref_cannot_confirm "no member named 'ConfirmResume'")
zet_add_compile_fail(session_cannot_be_changed_directly "is a private member")
zet_add_compile_fail(handle_cannot_be_copied "call to deleted constructor")
zet_add_compile_fail(attempt_finishes_once "rvalue ref-qualifier|is an lvalue")

zet_add_compile_fail(client_handle_from_nowhere "calling a private constructor")
zet_add_compile_fail(client_session_cannot_be_changed_directly
                     "is a private member")
zet_add_compile_fail(client_handle_cannot_be_copied "call to deleted constructor")
zet_add_compile_fail(client_attempt_finishes_once
                     "rvalue ref-qualifier|is an lvalue")
