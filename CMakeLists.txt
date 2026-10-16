cmake_minimum_required(VERSION 3.13)
project(j_alert_decoder)

# ------------------------------------------------------------------------------
# Out-of-tree SDR++ decoder module for J-ALERT (全国瞬時警報システム) satellite
# downlink — BPSK SCPC, K=7 (171,133) convolutional FEC, IESS-308 self-sync
# scrambler, HDLC framing, NowcastPacket → gzip → JMA alert XML.
#
# The plugin only consumes SDR++ headers; sdrpp_core symbols are resolved at
# runtime when SDR++ loads the shared library. Cross compiling for any ELF
# target (Linux x86_64/aarch64/armhf) only needs a C++ compiler plus zlib — no
# SDR++ build artefacts. Windows (PE) and macOS (Mach-O) are out of scope for
# the Dev Container; see README.md for the rationale.
#
# Windows (MSVC, x64): a DLL cannot leave symbols unresolved, so the plugin
# links import libraries generated from the official SDR++ Windows build.
# scripts/fetch-windows-deps.py prepares them (plus the fftw3/volk headers the
# SDR++ headers include) in external/sdrpp-windows; scripts/build-windows.ps1
# drives the whole build.
#
# SDR++ source is searched, in order:
#   1. -DSDRPP_SOURCE_DIR=...
#   2. ./external/SDRPlusPlus   (populated by scripts/fetch-sdrpp.sh)
#   3. ../SDRPlusPlus           (sibling checkout)
# ------------------------------------------------------------------------------

# Resolve the SDR++ source tree
set(_sdrpp_candidates "")
if (SDRPP_SOURCE_DIR)
    list(APPEND _sdrpp_candidates "${SDRPP_SOURCE_DIR}")
endif ()
list(APPEND _sdrpp_candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/external/SDRPlusPlus"
    "${CMAKE_CURRENT_SOURCE_DIR}/../SDRPlusPlus")

set(SDRPP_SOURCE_DIR "")
foreach (_candidate IN LISTS _sdrpp_candidates)
    get_filename_component(_abs "${_candidate}" ABSOLUTE)
    if (EXISTS "${_abs}/core/src/module.h")
        set(SDRPP_SOURCE_DIR "${_abs}")
        break()
    endif ()
endforeach ()

if (NOT SDRPP_SOURCE_DIR)
    message(FATAL_ERROR
        "Could not locate the SDR++ source tree. Run scripts/fetch-sdrpp.sh "
        "to download it, or pass -DSDRPP_SOURCE_DIR=/path/to/SDRPlusPlus.")
endif ()
message(STATUS "Using SDR++ source tree: ${SDRPP_SOURCE_DIR}")

# Compiler flags — mirror SDR++'s own settings so the ABI matches sdrpp_core.
if (CMAKE_BUILD_TYPE MATCHES "Debug")
    if (MSVC)
        set(SDRPP_MODULE_COMPILER_FLAGS /std:c++17 /EHsc)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(SDRPP_MODULE_COMPILER_FLAGS -g -Og -std=c++17 -Wno-unused-command-line-argument -undefined dynamic_lookup)
    else ()
        set(SDRPP_MODULE_COMPILER_FLAGS -g -Og -std=c++17)
    endif ()
else ()
    if (MSVC)
        set(SDRPP_MODULE_COMPILER_FLAGS /O2 /Ob2 /std:c++17 /EHsc)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(SDRPP_MODULE_COMPILER_FLAGS -O3 -std=c++17 -Wno-unused-command-line-argument -undefined dynamic_lookup)
    else ()
        set(SDRPP_MODULE_COMPILER_FLAGS -O3 -std=c++17)
    endif ()
endif ()

find_package(ZLIB REQUIRED)

# ── Windows: import libraries for the SDR++ DLLs ──────────────────────────────
if (WIN32)
    if (NOT MSVC)
        message(FATAL_ERROR "Windows builds require MSVC (SDR++ core is built with MSVC).")
    endif ()
    if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "SDR++ for Windows is x64 only; use an x64 MSVC toolchain.")
    endif ()
    set(SDRPP_WINDOWS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/sdrpp-windows"
        CACHE PATH "Output of scripts/fetch-windows-deps.py")
    if (NOT EXISTS "${SDRPP_WINDOWS_DIR}/def/sdrpp_core.def")
        message(FATAL_ERROR
            "SDR++ Windows import definitions not found in ${SDRPP_WINDOWS_DIR}. "
            "Run: python scripts/fetch-windows-deps.py")
    endif ()
    set(_implib_dir "${CMAKE_CURRENT_BINARY_DIR}/implib")
    file(MAKE_DIRECTORY "${_implib_dir}")
    set(SDRPP_IMPORT_LIBS "")
    foreach (_dll sdrpp_core volk fftw3f)
        set(_def "${SDRPP_WINDOWS_DIR}/def/${_dll}.def")
        set(_lib "${_implib_dir}/${_dll}.lib")
        if (NOT EXISTS "${_lib}" OR "${_def}" IS_NEWER_THAN "${_lib}")
            execute_process(
                COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${_def}" "/out:${_lib}"
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
            if (NOT _rc EQUAL 0)
                message(FATAL_ERROR "lib.exe failed for ${_dll}.def:\n${_out}")
            endif ()
        endif ()
        list(APPEND SDRPP_IMPORT_LIBS "${_lib}")
    endforeach ()
    message(STATUS "SDR++ import libraries: ${_implib_dir}")
endif ()

# Sources
file(GLOB_RECURSE SRC "src/*.cpp")

# ── SDR++ plugin shared library ───────────────────────────────────────────────
add_library(${PROJECT_NAME} SHARED ${SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
target_compile_options(${PROJECT_NAME} PRIVATE ${SDRPP_MODULE_COMPILER_FLAGS})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${SDRPP_SOURCE_DIR}/core/src"
    "${SDRPP_SOURCE_DIR}/core/src/imgui"
    "${SDRPP_SOURCE_DIR}/core/backends/glfw"
    "${SDRPP_SOURCE_DIR}/core/backends/glfw/imgui"
    "src/")
target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)

if (WIN32)
    target_include_directories(${PROJECT_NAME} PRIVATE "${SDRPP_WINDOWS_DIR}/include")
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS)
    # Sources are UTF-8 (Japanese comments); don't let a CP932 locale misread them.
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /bigobj)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${SDRPP_IMPORT_LIBS} ws2_32)
endif ()

# Allow unresolved sdrpp_core symbols at link time.
#   * Mach-O (macOS / clang): -undefined dynamic_lookup
#   * ELF (everything else):  --unresolved-symbols=ignore-in-object-files
if (APPLE)
    target_link_options(${PROJECT_NAME} PRIVATE -undefined dynamic_lookup)
elseif (NOT MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        -Wl,--unresolved-symbols=ignore-in-object-files)
endif ()

if (WIN32)
    install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION modules)
else ()
    install(TARGETS ${PROJECT_NAME} DESTINATION lib/sdrpp/plugins)
endif ()
