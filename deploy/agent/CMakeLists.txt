cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK toolchain")
    else()
        message(FATAL_ERROR "Set VITASDK before configuring VitaDevDeploy Agent")
    endif()
endif()

project(VitaDevDeployAgent C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaDevDeploy Agent")
set(VITA_VERSION "01.00")

option(VDEV_ENABLE_INSTALL "Enable the signed install action (OFF until verification tests pass)" OFF)
option(VDEV_ENABLE_DISPLAY_UI
    "Enable the experimental direct-framebuffer status interface" OFF)
set(VDEV_AGENT_TITLE_ID "VDEVDEP01" CACHE STRING
    "Title ID used by this agent build (use SLRS00001 for the bootstrap build)")
set(VDEV_ONLY_TARGET_TITLE_ID "" CACHE STRING
    "Optional single title ID this build is permitted to deploy")
set(VDD_PUBLIC_KEY_HEX "" CACHE STRING
    "Trusted Ed25519 public key as exactly 64 lowercase hexadecimal characters")

foreach(TITLE_ID_VARIABLE VDEV_AGENT_TITLE_ID VDEV_ONLY_TARGET_TITLE_ID)
    if(NOT "${${TITLE_ID_VARIABLE}}" STREQUAL "")
        string(LENGTH "${${TITLE_ID_VARIABLE}}" TITLE_ID_LENGTH)
        if(NOT TITLE_ID_LENGTH EQUAL 9 OR
           NOT "${${TITLE_ID_VARIABLE}}" MATCHES "^[A-Z0-9]+$")
            message(FATAL_ERROR "${TITLE_ID_VARIABLE} must be exactly 9 uppercase ASCII letters/digits")
        endif()
    endif()
endforeach()

if(VDEV_AGENT_TITLE_ID STREQUAL "")
    message(FATAL_ERROR "VDEV_AGENT_TITLE_ID cannot be empty")
endif()
if(NOT VDEV_ONLY_TARGET_TITLE_ID STREQUAL "" AND
   VDEV_ONLY_TARGET_TITLE_ID STREQUAL VDEV_AGENT_TITLE_ID)
    message(FATAL_ERROR "VDEV_ONLY_TARGET_TITLE_ID cannot equal VDEV_AGENT_TITLE_ID")
endif()

string(LENGTH "${VDD_PUBLIC_KEY_HEX}" VDD_PUBLIC_KEY_LENGTH)
if(NOT VDD_PUBLIC_KEY_LENGTH EQUAL 64 OR
   NOT VDD_PUBLIC_KEY_HEX MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "VDD_PUBLIC_KEY_HEX must be exactly 64 lowercase hex characters")
endif()
if(VDD_PUBLIC_KEY_HEX STREQUAL
   "0000000000000000000000000000000000000000000000000000000000000000")
    message(FATAL_ERROR "VDD_PUBLIC_KEY_HEX cannot be the all-zero key")
endif()

set(VITADEVDEPLOY_AGENT_SOURCES
    src/main.c
    src/crypto.c
    src/io.c
    src/protocol.c
    src/result.c
    src/sfo.c
    src/ui.c
    third_party/monocypher/monocypher.c
    third_party/monocypher/monocypher-ed25519.c
)

if(VDEV_ENABLE_INSTALL)
    list(APPEND VITADEVDEPLOY_AGENT_SOURCES src/promoter_vitadb.c)
endif()

if(VDEV_ENABLE_DISPLAY_UI)
    set(VITADEVDEPLOY_DEBUG_SCREEN_DIR
        "${VITASDK}/share/gcc-arm-vita-eabi/samples/common")
    if(NOT EXISTS "${VITADEVDEPLOY_DEBUG_SCREEN_DIR}/debugScreen.c" OR
       NOT EXISTS "${VITADEVDEPLOY_DEBUG_SCREEN_DIR}/debugScreen.h")
        message(FATAL_ERROR
            "The VitaSDK debugScreen helper is missing from ${VITADEVDEPLOY_DEBUG_SCREEN_DIR}")
    endif()
    file(SHA256 "${VITADEVDEPLOY_DEBUG_SCREEN_DIR}/debugScreen.c"
         VITADEVDEPLOY_DEBUG_SCREEN_C_SHA256)
    file(SHA256 "${VITADEVDEPLOY_DEBUG_SCREEN_DIR}/debugScreen.h"
         VITADEVDEPLOY_DEBUG_SCREEN_H_SHA256)
    if(NOT VITADEVDEPLOY_DEBUG_SCREEN_C_SHA256 STREQUAL
           "a69ab5b35f825858630b704402c09befe9855186b7e17e9f1e9b7b8b32c99d46" OR
       NOT VITADEVDEPLOY_DEBUG_SCREEN_H_SHA256 STREQUAL
           "b9b790bad51444cb474cc16acf8e331ffe3aca082840b067781aa81c49b96592")
        message(FATAL_ERROR
            "The installed VitaSDK debugScreen helper changed; review and pin the new version before building")
    endif()
    list(APPEND VITADEVDEPLOY_AGENT_SOURCES
        "${VITADEVDEPLOY_DEBUG_SCREEN_DIR}/debugScreen.c")
    set_source_files_properties(
        "${VITADEVDEPLOY_DEBUG_SCREEN_DIR}/debugScreen.c"
        PROPERTIES COMPILE_OPTIONS "-Wno-shadow")
endif()

add_executable(vitadevdeploy_agent ${VITADEVDEPLOY_AGENT_SOURCES})

target_include_directories(vitadevdeploy_agent PRIVATE
    src
    third_party/monocypher
)
if(VDEV_ENABLE_DISPLAY_UI)
    target_include_directories(vitadevdeploy_agent PRIVATE
        "${VITADEVDEPLOY_DEBUG_SCREEN_DIR}")
endif()

target_compile_features(vitadevdeploy_agent PRIVATE c_std_99)
target_compile_options(vitadevdeploy_agent PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
)
target_compile_definitions(vitadevdeploy_agent PRIVATE
    VDEV_ENABLE_INSTALL=$<BOOL:${VDEV_ENABLE_INSTALL}>
    VDEV_ENABLE_DISPLAY_UI=$<BOOL:${VDEV_ENABLE_DISPLAY_UI}>
    VDEV_AGENT_TITLE_ID="${VDEV_AGENT_TITLE_ID}"
    VDEV_ONLY_TARGET_TITLE_ID="${VDEV_ONLY_TARGET_TITLE_ID}"
    VDD_PUBLIC_KEY_HEX="${VDD_PUBLIC_KEY_HEX}"
)

target_link_libraries(vitadevdeploy_agent
    crypto
    SceCtrl_stub
)

if(VDEV_ENABLE_DISPLAY_UI)
    target_link_libraries(vitadevdeploy_agent SceDisplay_stub)
endif()

if(VDEV_ENABLE_INSTALL)
    target_link_libraries(vitadevdeploy_agent
        ScePromoterUtil_stub
        SceSysmodule_stub
    )
endif()

# Verification-only builds contain no promotion code and use a safe fSELF.
# Promotion builds use VitaSDK's normal user-mode UNSAFE profile. This matches
# VitaDB-Downloader's released, hardware-proven PAF/PromoterUtil path. The
# explicit VitaShell/SceShell 0x2808 profile is intentionally not used: A/B
# hardware testing showed that it was rejected before main() on the target
# retail Vita. Installation remains behind the signed request gate.
if(VDEV_ENABLE_INSTALL)
    vita_create_self(eboot.bin vitadevdeploy_agent UNSAFE)
else()
    vita_create_self(eboot.bin vitadevdeploy_agent)
endif()
vita_create_vpk(VitaDevDeployAgent.vpk ${VDEV_AGENT_TITLE_ID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
