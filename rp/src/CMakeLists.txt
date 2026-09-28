# Set minimum required version of CMake
cmake_minimum_required(VERSION 3.26)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# if PICOTOOL_OVERRIDE_DIR system environment variable is set,
# then use that as the folder for the picotool executable
if (DEFINED ENV{PICOTOOL_OVERRIDE_DIR})
    message("PICOTOOL_OVERRIDE_DIR env var is set to '$ENV{PICOTOOL_OVERRIDE_DIR}'")
    add_executable(picotool IMPORTED GLOBAL)
    set_property(TARGET picotool PROPERTY IMPORTED_LOCATION $ENV{PICOTOOL_OVERRIDE_DIR}/picotool)
    # check the picotool version:
    execute_process(COMMAND $ENV{PICOTOOL_OVERRIDE_DIR}/picotool version
                    OUTPUT_VARIABLE PICOTOOL_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX MATCH "^picotool v${picotool_VERSION_REQUIRED}" PICOTOOL_VERSION_MATCH ${PICOTOOL_VERSION})
    if (NOT PICOTOOL_VERSION_MATCH)
        message("picotool version response was: ${PICOTOOL_VERSION}")
        message(FATAL_ERROR "PICOTOOL_OVERRIDE_DIR is set to '$ENV{PICOTOOL_OVERRIDE_DIR}', but the version of picotool found is not ${picotool_VERSION_REQUIRED}")
    else()
        message("picotool version response was: ${PICOTOOL_VERSION}")
    endif()
endif ()

# If not specified, use pico_w as default board type
set(BOARD_TYPE $ENV{BOARD_TYPE})
if(NOT BOARD_TYPE)
    set(BOARD_TYPE "pico_w")
endif()
set(PICO_BOARD ${BOARD_TYPE})

# Determine board type macros
if("${BOARD_TYPE}" STREQUAL "pico_w")
    add_compile_definitions(BOARD_TYPE_PICO_W=1 BOARD_TYPE_PICO=0 BOARD_TYPE_CUSTOM16MB=0)
elseif("${BOARD_TYPE}" STREQUAL "pico")
    add_compile_definitions(BOARD_TYPE_PICO_W=0 BOARD_TYPE_PICO=1 BOARD_TYPE_CUSTOM16MB=0)
elseif("${BOARD_TYPE}" STREQUAL "sidecartos_16mb")
    add_compile_definitions(BOARD_TYPE_PICO_W=0 BOARD_TYPE_PICO=1 BOARD_TYPE_CUSTOM16MB=0)
else()
    message(FATAL_ERROR "Unknown BOARD_TYPE: ${BOARD_TYPE}")
endif()

# Resolve bundled Pico dependencies relative to the repo root so configure
# works regardless of the active build directory.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
set(DEFAULT_FATFS_SDK_PATH "${REPO_ROOT}/fatfs-sdk")
set(DEFAULT_PICO_SDK_PATH "${REPO_ROOT}/pico-sdk")
set(DEFAULT_PICO_EXTRAS_PATH "${REPO_ROOT}/pico-extras")

if(NOT DEFINED ENV{FATFS_SDK_PATH} OR "$ENV{FATFS_SDK_PATH}" STREQUAL "")
    set(ENV{FATFS_SDK_PATH} "${DEFAULT_FATFS_SDK_PATH}")
endif()
set(FATFS_SDK_PATH "$ENV{FATFS_SDK_PATH}" CACHE PATH "Path to the FATFS SDK" FORCE)
get_filename_component(FATFS_SDK_PATH "${FATFS_SDK_PATH}" REALPATH BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(FATFS_SDK_PATH "${FATFS_SDK_PATH}" CACHE PATH "Path to the FATFS SDK" FORCE)
set(ENV{FATFS_SDK_PATH} "${FATFS_SDK_PATH}")
message("FATFS_SDK_PATH: ${FATFS_SDK_PATH}")

if(NOT DEFINED ENV{PICO_SDK_PATH} OR "$ENV{PICO_SDK_PATH}" STREQUAL "")
    set(ENV{PICO_SDK_PATH} "${DEFAULT_PICO_SDK_PATH}")
endif()
set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK" FORCE)
get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK" FORCE)
set(ENV{PICO_SDK_PATH} "${PICO_SDK_PATH}")
message("PICO_SDK_PATH: ${PICO_SDK_PATH}")

if(NOT DEFINED ENV{PICO_EXTRAS_PATH} OR "$ENV{PICO_EXTRAS_PATH}" STREQUAL "")
    set(ENV{PICO_EXTRAS_PATH} "${DEFAULT_PICO_EXTRAS_PATH}")
endif()
set(PICO_EXTRAS_PATH "$ENV{PICO_EXTRAS_PATH}" CACHE PATH "Path to the PICO EXTRAS" FORCE)
get_filename_component(PICO_EXTRAS_PATH "${PICO_EXTRAS_PATH}" REALPATH BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(PICO_EXTRAS_PATH "${PICO_EXTRAS_PATH}" CACHE PATH "Path to the PICO EXTRAS" FORCE)
set(ENV{PICO_EXTRAS_PATH} "${PICO_EXTRAS_PATH}")
message("PICO_EXTRAS_PATH: ${PICO_EXTRAS_PATH}")

# Include build functions from Pico SDK
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
include(${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)

# Set name of project (as PROJECT_NAME) and C/C   standards
project(rp C CXX)
set(CMAKE_C_STANDARD 11)

# malloc must return NULL on failure instead of panicking.
#
# This MUST come before pico_sdk_init(): the check that panics lives in the
# SDK's own malloc.c, and pico_sdk_init() adds the SDK as a subdirectory, which
# inherits compile definitions as they stand at that moment. Defined further
# down, it would compile our sources with it and leave the SDK's malloc.c
# untouched, which looks right and does nothing.
#
# The SDK default (PICO_MALLOC_PANIC 1) turns a failed allocation into
# panic("Out of memory"), which defeats code written to handle NULL. FatFs's
# directory creation is the case that matters: it asks for a buffer as large
# as a cluster (32 KB on a large FAT32 card) and halves the request until it
# fits, which it can only do if malloc is allowed to say no. Every allocation
# in this firmware checks its result.
if (NOT DEFINED ENV{PICO_MALLOC_PANIC})
    set(ENV{PICO_MALLOC_PANIC} "0")
endif()
add_compile_definitions(PICO_MALLOC_PANIC=$ENV{PICO_MALLOC_PANIC})

# Creates a pico-sdk subdirectory in our project for the libraries
pico_sdk_init()

# Add u8g2 library
add_subdirectory(u8g2)

# Tell CMake where to find other source code
add_subdirectory(${FATFS_SDK_PATH}/src build)
  
# Tell CMake where to find the executable source file
add_executable(${PROJECT_NAME} 
        main.c
)

pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/romemul.pio)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/commemul.pio)

target_sources(${PROJECT_NAME} PRIVATE
        aconfig.c
        blink.c
        chandler.c
        commemul.c
        devdownload.c
        display.c
        display_term.c
        download.c
        emul.c
        gconfig.c
        httpc/httpc.c
        hw_config.c
        network.c
        reset.c
        romemul.c
        sdcard.c
        select.c
        term.c
        tprotocol.c
        md5/md5.c
        settings/settings.c)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(${PROJECT_NAME})

# Link libraries required for the project
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${LINK_LIBRARIES}        # External or additional libraries passed as variables
    hardware_flash           # Flash memory access
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico                # FATFS library   
    pico_stdlib              # Core functionality
    pico_multicore          # Multicore support
    pico_lwip_http           # lwIP's HTTP client, under httpc/
    u8g2                     # for display
)

# Conditionally link the CYW43 architecture library if supported
if (PICO_CYW43_SUPPORTED)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        pico_cyw43_arch_lwip_poll
#        pico_cyw43_arch_lwip_threadsafe_background
        )
else()
    message(WARNING "CYW43 architecture not supported")
endif()

# Link custom memmap with reserved memory for ROMs
set_target_properties(${PROJECT_NAME} PROPERTIES
        PICO_TARGET_LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/memmap_rp.ld
)

# Make our project-local FatFs config (rp/src/ff/ffconf.h) win over the
# fatfs-sdk submodule's default copy. fatfs sources do `#include "ffconf.h"`
# which is resolved against the include path; placing this BEFORE PRIVATE
# guarantees our override is found first, so the submodule stays pristine
# and no on-the-fly sed patching is needed.
target_include_directories(${PROJECT_NAME} BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/ff)

# Needed to include lwipopts.h properly. settings/ holds settings.h;
# settings.c is compiled directly into the target (add_executable above).
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/settings
    ${CMAKE_CURRENT_LIST_DIR}/..)


# Use reduced space for ROMs
pico_set_binary_type(${PROJECT_NAME} rp)

# HTTPS downloads. APP_DOWNLOAD_HTTPS=1 in the environment builds TLS in:
# mbedTLS, lwIP's ALTCP layer and the memory they need. Unset or 0 builds HTTP
# only, with neither linked. The C code, lwipopts.h and the build ID all follow
# the one definition, and httpc.c is compiled into this target, not a library
# of its own, so it cannot be built with a different value.
set(APP_DOWNLOAD_HTTPS "$ENV{APP_DOWNLOAD_HTTPS}")
if(APP_DOWNLOAD_HTTPS STREQUAL "")
    set(APP_DOWNLOAD_HTTPS 0)
endif()
if(NOT APP_DOWNLOAD_HTTPS MATCHES "^[01]$")
    message(FATAL_ERROR "APP_DOWNLOAD_HTTPS must be 0 or 1, not '${APP_DOWNLOAD_HTTPS}'")
endif()
if(APP_DOWNLOAD_HTTPS)
    # Core 0's stack takes SCRATCH_X as well: 8 KB (see memmap_rp.ld).
    target_link_options(${PROJECT_NAME} PRIVATE
        "-Wl,--defsym=__core0_stack_takes_scratch_x=1")
    target_sources(${PROJECT_NAME} PRIVATE mbedtls_ms_time.c)
    # mbedTLS itself is compiled -Os whatever the build type. At -O3 (CMake
    # Release) every TLS download failed: the server closed the connection
    # about 0.5 s into the handshake. Bisected on the device to
    # library/gcm.c (AES-GCM, the suite servers pick) alone; the whole library
    # stays at -Os, the configuration Booster ships and every test host passed
    # with. TLS speed is set by the network, not by this.
    file(GLOB MBEDTLS_LIBRARY_SOURCES ${PICO_SDK_PATH}/lib/mbedtls/library/*.c)
    set_source_files_properties(${MBEDTLS_LIBRARY_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-Os")
    target_link_libraries(${PROJECT_NAME} PRIVATE
        pico_lwip_mbedtls    # lwIP's ALTCP-over-mbedTLS layer
        pico_mbedtls)        # mbedTLS
endif()

# Fetch the values from the environment variables
set(RELEASE_TYPE $ENV{RELEASE_TYPE})
set(RELEASE_VERSION $ENV{RELEASE_VERSION})
set(RELEASE_DATE $ENV{RELEASE_DATE})
set(_DEBUG $ENV{DEBUG_MODE})

# If the environment variables are not set, use default values
if(NOT RELEASE_VERSION)

    # Check if RELEASE_TYPE is set and not empty, otherwise use default "FINAL"
    if(NOT RELEASE_TYPE OR RELEASE_TYPE STREQUAL "")
        set(RELEASE_TYPE "final")
    endif()

    # Determine the file name based on RELEASE_TYPE
    if(RELEASE_TYPE STREQUAL "final")
        set(VERSION_FILE_NAME "version.txt")
    else()
        set(VERSION_FILE_NAME "version-${RELEASE_TYPE}.txt")
    endif()

    # rp/<file>, as rp/build.sh reads it; a build that does not go through
    # the scripts (VS Code) gets the same version.
    set(VERSION_FILE_PATH "${CMAKE_CURRENT_LIST_DIR}/../${VERSION_FILE_NAME}")
    if(EXISTS "${VERSION_FILE_PATH}")
        file(READ "${VERSION_FILE_PATH}" RELEASE_VERSION)
    endif()

    # Remove newline character if present
    string(STRIP "${RELEASE_VERSION}" RELEASE_VERSION)

    # In case version.txt is empty or does not exist, use a default version
    if("${RELEASE_VERSION}" STREQUAL "")
        set(RELEASE_VERSION "v0.0.1dev")
    endif()
endif()

if(NOT RELEASE_DATE)
        string(TIMESTAMP CURRENT_DATE_TIME "%Y-%m-%d %H:%M:%S")
        set(RELEASE_DATE ${CURRENT_DATE_TIME})
endif()

# DEBUG_MODE is "0" for a release build, which CMake reads as false: defaulting
# to 1 here made every release build a debug build. Unset means release too.
if (NOT _DEBUG)
        set(_DEBUG 0)
endif()

# Debug outputs
pico_enable_stdio_usb(${PROJECT_NAME} 0)
# Workaround to disable USB output in release builds
if(${_DEBUG} STREQUAL "0")
    pico_enable_stdio_uart(${PROJECT_NAME} 0)
else()
    pico_enable_stdio_uart(${PROJECT_NAME} 1)
    # 921,600 baud, not the SDK's 115,200: DPRINTF blocks on the UART, and on a
    # timing-sensitive cartridge bus a chatty console at 115,200 delays the
    # firmware enough to change behavior. The UART clock error at this rate is
    # 0.02 %. Set the serial terminal (tools/dev/console.py) to 921600.
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        PICO_DEFAULT_UART_BAUD_RATE=921600)
endif()

# Print the variables on screen
message("RELEASE_TYPE: " ${RELEASE_TYPE})
message("RELEASE_VERSION: " ${RELEASE_VERSION})
message("RELEASE_DATE: " ${RELEASE_DATE})
message("DEBUG_MODE: " ${_DEBUG})
message("APP_DOWNLOAD_HTTPS: " ${APP_DOWNLOAD_HTTPS})
message("LATEST_RELEASE_URL: " ${LATEST_RELEASE_URL})

# Pass these values to the C compiler
add_definitions(-DRELEASE_TYPE="${RELEASE_TYPE}")
add_definitions(-DRELEASE_VERSION="${RELEASE_VERSION}")
add_definitions(-DRELEASE_DATE="${RELEASE_DATE}")
add_definitions(-DLATEST_RELEASE_URL="${LATEST_RELEASE_URL}")

# Pass the APP_UUID_KEY to the C compiler
if(DEFINED ENV{APP_UUID_KEY} AND NOT "$ENV{APP_UUID_KEY}" STREQUAL "")
    message(STATUS "Using APP_UUID_KEY from environment: $ENV{APP_UUID_KEY}")
    add_definitions(-DCURRENT_APP_UUID_KEY=\"$ENV{APP_UUID_KEY}\")
else()
    message(STATUS "Using default UUID")
    add_definitions(-DCURRENT_APP_UUID_KEY=\"44444444-4444-4444-8444-444444444444\")
endif()

# Pass the _DEBUG flag to the C compiler
add_definitions(-D_DEBUG=${_DEBUG})

# Device/Computer type
add_definitions(-DDISPLAY_ATARIST)

# We don't modify the flash in Core 1
add_definitions(-DPICO_FLASH_ASSUME_CORE0_SAFE=1)

# Core 0's stack takes all of SCRATCH_Y (4 KB) instead of the SDK's 2 KB, and
# a guard at its bottom turns an overflow into a HardFault instead of a silent
# walk through SCRATCH_X into the cartridge window below it. Measured on an ST
# with the setup menu: 1,696 bytes at the deepest. PICO_HEAP_SIZE is the heap
# the link guarantees: a build whose static data leaves less fails to link.
# Measured: 13.1 KB of heap at the peak after boot and the menu commands.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    PICO_STACK_SIZE=0x1000
    PICO_USE_STACK_GUARDS=1
    PICO_HEAP_SIZE=0x8000)

# HTTP only, or HTTP and HTTPS downloads (see APP_DOWNLOAD_HTTPS above)
add_definitions(-DAPP_DOWNLOAD_HTTPS=${APP_DOWNLOAD_HTTPS})

# build_id.h and build_id.c (RELEASE_BUILD_ID and the release_build_id symbol,
# the git commit) are regenerated on every build; see build_id.cmake. They are
# rewritten only when the ID changes, so only their users recompile. The symbol
# is kept through --gc-sections so tools/dev/swd.py can read it over SWD.
# A debug build's ID ends in +debug and an HTTPS build's in +https (before
# +debug), so builds of one tree cannot be taken for each other on the device.
set(BUILD_ID_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/build_id)
add_custom_target(build_id
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_CURRENT_LIST_DIR}
            -DOUT_DIR=${BUILD_ID_DIR} -DDEBUG_BUILD=${_DEBUG}
            -DHTTPS_BUILD=${APP_DOWNLOAD_HTTPS}
            -P ${CMAKE_CURRENT_LIST_DIR}/build_id.cmake
    BYPRODUCTS ${BUILD_ID_DIR}/build_id.h ${BUILD_ID_DIR}/build_id.c
    COMMENT "Checking the build ID")
add_dependencies(${PROJECT_NAME} build_id)
target_sources(${PROJECT_NAME} PRIVATE ${BUILD_ID_DIR}/build_id.c)
set_source_files_properties(${BUILD_ID_DIR}/build_id.c PROPERTIES GENERATED TRUE)
target_include_directories(${PROJECT_NAME} PRIVATE ${BUILD_ID_DIR})
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--undefined=release_build_id")

# Remove unused data. The ELF keeps its symbol table: it never reaches the
# .uf2 (byte-identical either way), and tools/dev/swd.py finds the cartridge
# window, the build ID and its other addresses by symbol.
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--gc-sections")

# Find clang-format executable
find_program(CLANG_FORMAT_BIN NAMES clang-format)

if(CLANG_FORMAT_BIN)
    message(STATUS "Found clang-format: ${CLANG_FORMAT_BIN}")
    message(STATUS "Linting in the source dir: ${CMAKE_SOURCE_DIR}")

    # Add a custom target to run clang-format on all source files
    file(GLOB_RECURSE ALL_CXX_SOURCE_FILES
        "${CMAKE_SOURCE_DIR}/*.c"
        "${CMAKE_SOURCE_DIR}/include/*.h"
    )

    add_custom_target(clang-format
        COMMAND ${CLANG_FORMAT_BIN}
        -i
        -style=file
        ${ALL_CXX_SOURCE_FILES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/../..
        COMMENT "Running clang-format on the source code..."
        VERBATIM
    )

else()
    message(WARNING "clang-format not found!")
endif()
