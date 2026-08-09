# libFuzzer targets. Adding one is a single zet_add_fuzzer line: the source is
# named after the target, the seed corpus is found by name, and the runner
# picks the target up from the generated manifest — nothing in CI has to learn
# about it.
#
# EternalTerminal is the cautionary tale: its fuzzers point at paths that do
# not exist and are never run, so the repository carries the reassurance of
# fuzzing without any of it.

set_property(GLOBAL PROPERTY ZET_FUZZ_MANIFEST "")

# zet_add_fuzzer(<name> <libraries...>) — builds <name> from <name>.cpp.
function(zet_add_fuzzer name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ARGN} zet::compile_options)

    # The preset compiles everything with -fsanitize=fuzzer-no-link so that the
    # instrumentation reaches the libraries too; libFuzzer's own main() is
    # pulled in here and only here.
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)

    # A fuzzer without seeds starts from a single empty input and spends its
    # budget rediscovering the header layout, so a missing corpus is an error
    # rather than a default.
    string(REGEX REPLACE "_fuzz$" "" corpus "${name}")
    set(corpusDir "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${corpus}")
    if(NOT IS_DIRECTORY "${corpusDir}")
        message(FATAL_ERROR
            "fuzzer ${name} has no seed corpus at ${corpusDir}")
    endif()

    set_property(GLOBAL APPEND_STRING PROPERTY ZET_FUZZ_MANIFEST
        "${name} $<TARGET_FILE:${name}> ${corpusDir}\n")
endfunction()

zet_add_fuzzer(frame_fuzz zet::wire)
zet_add_fuzzer(sealed_frame_fuzz zet::wire zet::crypto)

# One line per target: name, binary, seed corpus. file(GENERATE) is what
# resolves $<TARGET_FILE>, so the runner never has to guess where the generator
# put the executable.
get_property(zetFuzzManifest GLOBAL PROPERTY ZET_FUZZ_MANIFEST)
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fuzzers.txt"
    CONTENT "${zetFuzzManifest}"
)
