cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE
            "$ENV{VITASDK}/share/vita.toolchain.cmake"
            CACHE PATH "VitaSDK toolchain")
    else()
        message(FATAL_ERROR
            "Set VITASDK before configuring the Vita companion gate")
    endif()
endif()

project(VitaDebugCompanionGate C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaDebug Companion Gate")
set(VITA_TITLE_ID "VDSCRN001")
set(VITA_VERSION "01.00")

set(SCREEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(MONOCYPHER_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../deploy/agent/third_party/monocypher")

add_library(vitadebug_companion_endpoint STATIC
    src/vd_endpoint.c
)
target_include_directories(vitadebug_companion_endpoint PUBLIC
    include
    "${SCREEN_ROOT}/include"
)
target_compile_features(vitadebug_companion_endpoint PRIVATE c_std_11)
target_compile_options(vitadebug_companion_endpoint PRIVATE
    -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes
)

add_library(vitadebug_companion_core STATIC
    "${SCREEN_ROOT}/src/vitadebug_companion.c"
    "${SCREEN_ROOT}/src/vitadebug_input_trace.c"
    "${SCREEN_ROOT}/src/vitadebug_screen.c"
    "${MONOCYPHER_ROOT}/monocypher.c"
)
target_include_directories(vitadebug_companion_core PUBLIC
    "${SCREEN_ROOT}/include"
    "${MONOCYPHER_ROOT}"
)
target_compile_features(vitadebug_companion_core PRIVATE c_std_11)
target_compile_options(vitadebug_companion_core PRIVATE
    -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes
)

add_executable(vitadebug_companion_gate src/main.c)
target_compile_features(vitadebug_companion_gate PRIVATE c_std_11)
target_compile_options(vitadebug_companion_gate PRIVATE
    -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes
    -ffunction-sections -fdata-sections
)
target_link_options(vitadebug_companion_gate PRIVATE
    -Wl,--gc-sections
)
target_link_libraries(vitadebug_companion_gate
    vitadebug_companion_endpoint
    vitadebug_companion_core
    SceCtrl_stub
    SceDisplay_stub
    SceIofilemgr_stub
    SceNet_stub
    SceSysmodule_stub
    SceLibKernel_stub
)

# This is an ordinary safe user-mode fSELF. Never add UNSAFE here.
vita_create_self(eboot.bin vitadebug_companion_gate)
vita_create_vpk(vitadebug-companion-gate.vpk
    ${VITA_TITLE_ID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
