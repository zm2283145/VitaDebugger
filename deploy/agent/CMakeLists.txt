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
option(VDEV_ENABLE_LIVEAREA_ASSETS
    "Package the reviewed icon, splash, and LiveArea artwork" ON)
option(VDEV_ENABLE_DISPLAY_UI
    "Enable the opt-in native vita2d status interface" OFF)
option(VDEV_ENABLE_DIRECT_TCP
    "Enable the experimental authenticated direct TCP package intake" OFF)
set(VDEV_DIRECT_TCP_PORT "18196" CACHE STRING
    "TCP port used by the optional direct package intake")
if(NOT VDEV_DIRECT_TCP_PORT MATCHES "^[0-9]+$" OR
   VDEV_DIRECT_TCP_PORT LESS 1 OR VDEV_DIRECT_TCP_PORT GREATER 65535)
    message(FATAL_ERROR "VDEV_DIRECT_TCP_PORT must be an integer from 1 through 65535")
endif()
set(VDEV_AGENT_TITLE_ID "VDEVDEP01" CACHE STRING
    "Title ID used by this agent build (use SLRS00001 for the bootstrap build)")
set(VDEV_ONLY_TARGET_TITLE_ID "" CACHE STRING
    "Optional single title ID this build is permitted to deploy")
set(VDD_PUBLIC_KEY_HEX "" CACHE STRING
    "Trusted Ed25519 public key as exactly 64 lowercase hexadecimal characters")

set(VDEV_LIVEAREA_ASSET_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/sce_sys" CACHE PATH
    "Root containing VitaDevDeploy's reviewed sce_sys artwork")

function(vdev_validate_png RELATIVE_PATH EXPECTED_WIDTH EXPECTED_HEIGHT)
    set(ASSET_PATH "${VDEV_LIVEAREA_ASSET_ROOT}/${RELATIVE_PATH}")
    if(NOT EXISTS "${ASSET_PATH}" OR IS_DIRECTORY "${ASSET_PATH}")
        message(FATAL_ERROR
            "LiveArea artwork is enabled, but '${ASSET_PATH}' is missing. "
            "See assets/README.md for the required source files.")
    endif()

    # Read only the fixed PNG signature and IHDR fields. This makes malformed,
    # resized, or interlaced artwork fail at configure time without invoking a
    # host image library whose output could vary between build machines.
    file(READ "${ASSET_PATH}" PNG_HEADER HEX OFFSET 0 LIMIT 29)
    string(TOLOWER "${PNG_HEADER}" PNG_HEADER)
    string(LENGTH "${PNG_HEADER}" PNG_HEADER_LENGTH)
    if(PNG_HEADER_LENGTH LESS 58)
        message(FATAL_ERROR "LiveArea asset '${ASSET_PATH}' has a truncated PNG header")
    endif()

    string(SUBSTRING "${PNG_HEADER}" 0 16 PNG_SIGNATURE)
    string(SUBSTRING "${PNG_HEADER}" 24 8 PNG_IHDR_NAME)
    if(NOT PNG_SIGNATURE STREQUAL "89504e470d0a1a0a" OR
       NOT PNG_IHDR_NAME STREQUAL "49484452")
        message(FATAL_ERROR "LiveArea asset '${ASSET_PATH}' is not a canonical PNG")
    endif()

    string(SUBSTRING "${PNG_HEADER}" 32 8 PNG_WIDTH_HEX)
    string(SUBSTRING "${PNG_HEADER}" 40 8 PNG_HEIGHT_HEX)
    string(SUBSTRING "${PNG_HEADER}" 48 2 PNG_BIT_DEPTH)
    string(SUBSTRING "${PNG_HEADER}" 50 2 PNG_COLOR_TYPE)
    string(SUBSTRING "${PNG_HEADER}" 52 2 PNG_COMPRESSION)
    string(SUBSTRING "${PNG_HEADER}" 54 2 PNG_FILTER)
    string(SUBSTRING "${PNG_HEADER}" 56 2 PNG_INTERLACE)
    math(EXPR PNG_WIDTH "0x${PNG_WIDTH_HEX}")
    math(EXPR PNG_HEIGHT "0x${PNG_HEIGHT_HEX}")

    if(NOT PNG_WIDTH EQUAL EXPECTED_WIDTH OR
       NOT PNG_HEIGHT EQUAL EXPECTED_HEIGHT)
        message(FATAL_ERROR
            "LiveArea asset '${ASSET_PATH}' must be ${EXPECTED_WIDTH}x${EXPECTED_HEIGHT}; "
            "its IHDR is ${PNG_WIDTH}x${PNG_HEIGHT}")
    endif()
    if(NOT PNG_BIT_DEPTH STREQUAL "08" OR
       NOT PNG_COLOR_TYPE STREQUAL "03" OR
       NOT PNG_COMPRESSION STREQUAL "00" OR
       NOT PNG_FILTER STREQUAL "00" OR
       NOT PNG_INTERLACE STREQUAL "00")
        message(FATAL_ERROR
            "LiveArea asset '${ASSET_PATH}' must be a non-interlaced 8-bit indexed PNG "
            "using the standard PNG compression/filter methods")
    endif()
endfunction()

set(VDEV_LIVEAREA_VPK_ASSETS)
if(VDEV_ENABLE_LIVEAREA_ASSETS)
    vdev_validate_png("icon0.png" 128 128)
    vdev_validate_png("pic0.png" 960 544)
    vdev_validate_png("livearea/contents/bg.png" 840 500)
    vdev_validate_png("livearea/contents/startup.png" 280 158)

    set(VDEV_LIVEAREA_TEMPLATE
        "${VDEV_LIVEAREA_ASSET_ROOT}/livearea/contents/template.xml")
    if(NOT EXISTS "${VDEV_LIVEAREA_TEMPLATE}" OR
       IS_DIRECTORY "${VDEV_LIVEAREA_TEMPLATE}")
        message(FATAL_ERROR
            "LiveArea artwork is enabled, but '${VDEV_LIVEAREA_TEMPLATE}' is missing")
    endif()
    file(READ "${VDEV_LIVEAREA_TEMPLATE}" VDEV_LIVEAREA_TEMPLATE_XML LIMIT 16385)
    string(LENGTH "${VDEV_LIVEAREA_TEMPLATE_XML}" VDEV_LIVEAREA_TEMPLATE_LENGTH)
    if(VDEV_LIVEAREA_TEMPLATE_LENGTH GREATER 16384 OR
       NOT VDEV_LIVEAREA_TEMPLATE_XML MATCHES
           "<livearea style=\"a1\" format-ver=\"01\\.00\" content-rev=\"1\">" OR
       NOT VDEV_LIVEAREA_TEMPLATE_XML MATCHES "<image>bg\\.png</image>" OR
       NOT VDEV_LIVEAREA_TEMPLATE_XML MATCHES
           "<startup-image>startup\\.png</startup-image>")
        message(FATAL_ERROR
            "LiveArea template.xml is oversized or does not match the reviewed a1 layout")
    endif()

    set(VDEV_LIVEAREA_VPK_ASSETS
        "${VDEV_LIVEAREA_ASSET_ROOT}/icon0.png" "sce_sys/icon0.png"
        "${VDEV_LIVEAREA_ASSET_ROOT}/pic0.png" "sce_sys/pic0.png"
        "${VDEV_LIVEAREA_ASSET_ROOT}/livearea/contents/bg.png"
        "sce_sys/livearea/contents/bg.png"
        "${VDEV_LIVEAREA_ASSET_ROOT}/livearea/contents/startup.png"
        "sce_sys/livearea/contents/startup.png"
        "${VDEV_LIVEAREA_TEMPLATE}"
        "sce_sys/livearea/contents/template.xml"
    )
endif()

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

# Syntax and an all-zero check are not enough for an Ed25519 trust anchor.
# Reject non-canonical points, the identity/low-order points, and points with a
# torsion component before any Vita object or package is built. The validator
# uses only Python's standard library and prints the fingerprint of the exact
# 32 raw bytes that will be compiled into the agent.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/validate_ed25519_public_key.py"
            "${VDD_PUBLIC_KEY_HEX}"
    RESULT_VARIABLE VDD_PUBLIC_KEY_VALIDATION_RESULT
    OUTPUT_VARIABLE VDD_PUBLIC_KEY_SHA256
    ERROR_VARIABLE VDD_PUBLIC_KEY_VALIDATION_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
string(LENGTH "${VDD_PUBLIC_KEY_SHA256}" VDD_PUBLIC_KEY_SHA256_LENGTH)
if(NOT "${VDD_PUBLIC_KEY_VALIDATION_RESULT}" STREQUAL "0" OR
   NOT VDD_PUBLIC_KEY_SHA256_LENGTH EQUAL 64 OR
   NOT VDD_PUBLIC_KEY_SHA256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
        "VDD_PUBLIC_KEY_HEX is not a canonical non-identity Ed25519 "
        "prime-subgroup public key: ${VDD_PUBLIC_KEY_VALIDATION_ERROR}")
endif()
message(STATUS
    "Validated Ed25519 trust key fingerprint: sha256:${VDD_PUBLIC_KEY_SHA256}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/vdd-public-key-fingerprint.txt"
    "${VDD_PUBLIC_KEY_SHA256}\n")

set(VITADEVDEPLOY_AGENT_SOURCES
    src/main.c
    src/crypto.c
    src/direct_intake.c
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
    find_library(VITA2D_LIBRARY vita2d REQUIRED)
endif()

add_executable(vitadevdeploy_agent ${VITADEVDEPLOY_AGENT_SOURCES})

target_include_directories(vitadevdeploy_agent PRIVATE
    src
    third_party/monocypher
)
target_compile_features(vitadevdeploy_agent PRIVATE c_std_99)
target_compile_options(vitadevdeploy_agent PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
)
target_compile_definitions(vitadevdeploy_agent PRIVATE
    VDEV_ENABLE_INSTALL=$<BOOL:${VDEV_ENABLE_INSTALL}>
    VDEV_ENABLE_DISPLAY_UI=$<BOOL:${VDEV_ENABLE_DISPLAY_UI}>
    VDEV_ENABLE_DIRECT_TCP=$<BOOL:${VDEV_ENABLE_DIRECT_TCP}>
    VDEV_DIRECT_TCP_PORT=${VDEV_DIRECT_TCP_PORT}
    VDEV_AGENT_TITLE_ID="${VDEV_AGENT_TITLE_ID}"
    VDEV_ONLY_TARGET_TITLE_ID="${VDEV_ONLY_TARGET_TITLE_ID}"
    VDD_PUBLIC_KEY_HEX="${VDD_PUBLIC_KEY_HEX}"
)

target_link_libraries(vitadevdeploy_agent
    crypto
    SceCtrl_stub
)

if(VDEV_ENABLE_DISPLAY_UI)
    target_link_libraries(vitadevdeploy_agent
        ${VITA2D_LIBRARY}
        SceDisplay_stub
        SceGxm_stub
        SceAppMgr_stub
        SceCommonDialog_stub
        SceSysmodule_stub
        ScePgf_stub
        ScePvf_stub
        m
    )
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

if(VDEV_ENABLE_DIRECT_TCP)
    target_link_libraries(vitadevdeploy_agent
        SceNet_stub
        SceSysmodule_stub
    )
endif()

# vita.cmake currently flattens FILE arguments into a shell-style string before
# splitting them again. Absolute source paths containing spaces are therefore
# split into multiple arguments. Build an equivalent quoted flag string and
# register the inputs as explicit dependencies so checkouts under paths such as
# "Super Mario 64" package and rebuild correctly.
if(NOT DEFINED VDEV_BASE_VITA_PACK_VPK_FLAGS)
    set(VDEV_BASE_VITA_PACK_VPK_FLAGS "${VITA_PACK_VPK_FLAGS}" CACHE STRING
        "Additional reviewed vita-pack-vpk flags before project assets")
endif()
set(VITA_PACK_VPK_FLAGS "${VDEV_BASE_VITA_PACK_VPK_FLAGS}")
list(LENGTH VDEV_LIVEAREA_VPK_ASSETS VDEV_LIVEAREA_VPK_ASSET_ITEM_COUNT)
while(VDEV_LIVEAREA_VPK_ASSET_ITEM_COUNT GREATER 0)
    list(GET VDEV_LIVEAREA_VPK_ASSETS 0 VDEV_LIVEAREA_VPK_SOURCE)
    list(GET VDEV_LIVEAREA_VPK_ASSETS 1 VDEV_LIVEAREA_VPK_DESTINATION)
    string(APPEND VITA_PACK_VPK_FLAGS
        " -a \"${VDEV_LIVEAREA_VPK_SOURCE}=${VDEV_LIVEAREA_VPK_DESTINATION}\"")
    list(APPEND resources "${VDEV_LIVEAREA_VPK_SOURCE}")
    list(REMOVE_AT VDEV_LIVEAREA_VPK_ASSETS 0 1)
    list(LENGTH VDEV_LIVEAREA_VPK_ASSETS VDEV_LIVEAREA_VPK_ASSET_ITEM_COUNT)
endwhile()
set(VITA_PACK_VPK_FLAGS "${VITA_PACK_VPK_FLAGS}" CACHE STRING
    "vita-pack-vpk flags including project artwork" FORCE)
set(VDEV_LIVEAREA_PACKAGING_STAMP
    "${CMAKE_CURRENT_BINARY_DIR}/vitadevdeploy-livearea-packaging.txt")
file(GENERATE OUTPUT "${VDEV_LIVEAREA_PACKAGING_STAMP}"
    CONTENT
        "livearea_assets=${VDEV_ENABLE_LIVEAREA_ASSETS}\npack_flags=${VITA_PACK_VPK_FLAGS}\n")
list(APPEND resources "${VDEV_LIVEAREA_PACKAGING_STAMP}")

vita_create_vpk(VitaDevDeployAgent.vpk ${VDEV_AGENT_TITLE_ID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
