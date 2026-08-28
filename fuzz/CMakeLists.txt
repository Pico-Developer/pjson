# SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
# SPDX-License-Identifier: Apache-2.0

# ---- Fuzzing engine selection -------------------------------------------

# Standalone builds use Clang's bundled libFuzzer. OSS-Fuzz instead supplies
# PJSON_FUZZING_ENGINE so its chosen engine is linked into each harness.
if(NOT PJSON_FUZZING_ENGINE)
    if(NOT UNIX OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
        message(FATAL_ERROR
            "PJSON_BUILD_FUZZERS requires Clang with libFuzzer on Linux/macOS, "
            "or an external PJSON_FUZZING_ENGINE")
    endif()

    # Probe the complete sanitizer combination because accepting a compile
    # flag does not guarantee that the compiler can also link libFuzzer.
    include(CheckCXXSourceCompiles)
    set(PJSON_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fsanitize=fuzzer,address,undefined")
    check_cxx_source_compiles(
        "#include <cstddef>
         #include <cstdint>
         extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }"
        PJSON_HAS_BUILTIN_LIBFUZZER)
    set(CMAKE_REQUIRED_FLAGS "${PJSON_SAVED_REQUIRED_FLAGS}")
    unset(PJSON_SAVED_REQUIRED_FLAGS)
    if(NOT PJSON_HAS_BUILTIN_LIBFUZZER)
        message(FATAL_ERROR
            "${CMAKE_CXX_COMPILER} cannot compile and link -fsanitize=fuzzer; "
            "install a full LLVM toolchain or provide PJSON_FUZZING_ENGINE")
    endif()

    # Instrument the library under test without adding libFuzzer's main to it;
    # the final executable receives that entry point at link time below.
    target_compile_options(pjson PRIVATE
        -fsanitize=fuzzer-no-link,address,undefined
        -fno-omit-frame-pointer)
    # Any other executable linked to the instrumented static library still
    # needs the ASan/UBSan runtimes, but must not receive libFuzzer's main.
    target_link_options(pjson INTERFACE -fsanitize=address,undefined)
else()
    # OSS-Fuzz passes the engine as a shell-style string, which may contain
    # several archives or flags and therefore must be split before linking.
    separate_arguments(PJSON_FUZZING_ENGINE_ARGS NATIVE_COMMAND "${PJSON_FUZZING_ENGINE}")
endif()

# ---- Harness target definition -----------------------------------------

# Defines one fuzz harness with the common language level, warnings, project
# include path, and the engine selected above.
function(pjson_add_fuzzer target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE pjson::pjson)
    target_compile_features(${target} PRIVATE cxx_std_11)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()

    if(PJSON_FUZZING_ENGINE)
        # External engines are responsible for their own instrumentation and
        # entry point; CXX/CXXFLAGS supplied by OSS-Fuzz instrument the code.
        target_link_libraries(${target} PRIVATE ${PJSON_FUZZING_ENGINE_ARGS})
    else()
        # Local builds instrument both the harness and library, then link the
        # executable with libFuzzer plus the runtime bug detectors.
        target_compile_options(${target} PRIVATE
            -fsanitize=fuzzer-no-link,address,undefined
            -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
endfunction()

# ---- Harnesses ----------------------------------------------------------

pjson_add_fuzzer(pjson_fuzz_parse fuzz_parse.cpp)
pjson_add_fuzzer(pjson_fuzz_stream fuzz_stream.cpp)
pjson_add_fuzzer(pjson_fuzz_schema fuzz_schema.cpp)
pjson_add_fuzzer(pjson_fuzz_patch fuzz_patch.cpp)
