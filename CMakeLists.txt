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

# ── Unit tests (no SDR++ runtime needed) ─────────────────────────────────────
# Each tests/*_test.cpp is one test executable, linked against the decoder and
# sink sources (everything except the SDR++ module and UI code).
option(JALERT_BUILD_TESTS "Build unit tests" OFF)
if (JALERT_BUILD_TESTS)
    enable_testing()
    find_package(ZLIB REQUIRED)
    file(GLOB JALERT_CORE_SRC
        src/dsp/*.cpp src/fec/*.cpp src/frame/*.cpp src/decode/*.cpp src/sink/*.cpp)
    add_library(jalert_core STATIC ${JALERT_CORE_SRC})
    target_include_directories(jalert_core PUBLIC "src/")
    target_compile_features(jalert_core PUBLIC cxx_std_17)
    target_link_libraries(jalert_core PUBLIC ZLIB::ZLIB)
    if (MSVC)
        target_compile_definitions(jalert_core PUBLIC _USE_MATH_DEFINES)
        target_compile_options(jalert_core PUBLIC /utf-8)
    endif ()
    if (WIN32)
        target_link_libraries(jalert_core PUBLIC ws2_32)
    endif ()

    file(GLOB JALERT_TESTS tests/*_test.cpp)
    foreach (_test IN LISTS JALERT_TESTS)
        get_filename_component(_name "${_test}" NAME_WE)
        add_executable(${_name} "${_test}")
        target_link_libraries(${_name} PRIVATE jalert_core)
        add_test(NAME ${_name} COMMAND ${_name})
    endforeach ()
endif ()

find_package(ZLIB REQUIRED)

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

# Allow unresolved sdrpp_core symbols at link time.
#   * Mach-O (macOS / clang): -undefined dynamic_lookup
#   * ELF (everything else):  --unresolved-symbols=ignore-in-object-files
if (APPLE)
    target_link_options(${PROJECT_NAME} PRIVATE -undefined dynamic_lookup)
elseif (NOT MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        -Wl,--unresolved-symbols=ignore-in-object-files)
endif ()

install(TARGETS ${PROJECT_NAME} DESTINATION lib/sdrpp/plugins)
