cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
            CACHE PATH "VitaSDK toolchain")
    else()
        message(FATAL_ERROR "Set VITASDK before configuring the auth gate")
    endif()
endif()

project(VitaDebuggerAuthGate C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaDebugger Auth Gate")
set(VITA_TITLE_ID "VDAT00001")
set(VITA_VERSION "01.00")

set(VD_ATTACH_AUTH_LISTENER_PORT "18195" CACHE STRING
    "Authentication listener TCP port (18000-18999)")
set(VD_ATTACH_AUTH_BIND_IPV4_HEX "0a0101d9" CACHE STRING
    "Private bind IPv4 as eight lowercase network-order hexadecimal digits")
set(VD_ATTACH_AUTH_PEER_NETWORK_HEX "0a010100" CACHE STRING
    "Allowed peer network as eight lowercase network-order hexadecimal digits")
set(VD_ATTACH_AUTH_PEER_NETMASK_HEX "ffffff00" CACHE STRING
    "Contiguous peer netmask as eight lowercase network-order hexadecimal digits")
set(VD_ATTACH_AUTH_HANDSHAKE_MS "3000" CACHE STRING
    "One absolute authentication handshake deadline in milliseconds")
set(VD_ATTACH_AUTH_ASSURANCE_SOURCE "" CACHE FILEPATH
    "Reviewed source defining the three hardware assurance callbacks")

if(NOT VD_ATTACH_AUTH_LISTENER_PORT MATCHES "^[0-9]+$" OR
   VD_ATTACH_AUTH_LISTENER_PORT LESS 18000 OR
   VD_ATTACH_AUTH_LISTENER_PORT GREATER 18999)
    message(FATAL_ERROR
        "VD_ATTACH_AUTH_LISTENER_PORT must be from 18000 through 18999")
endif()
if(NOT VD_ATTACH_AUTH_HANDSHAKE_MS MATCHES "^[0-9]+$" OR
   VD_ATTACH_AUTH_HANDSHAKE_MS LESS 100 OR
   VD_ATTACH_AUTH_HANDSHAKE_MS GREATER 30000)
    message(FATAL_ERROR
        "VD_ATTACH_AUTH_HANDSHAKE_MS must be from 100 through 30000")
endif()
foreach(IP_VARIABLE
        VD_ATTACH_AUTH_BIND_IPV4_HEX
        VD_ATTACH_AUTH_PEER_NETWORK_HEX
        VD_ATTACH_AUTH_PEER_NETMASK_HEX)
    string(LENGTH "${${IP_VARIABLE}}" IP_HEX_LENGTH)
    if(NOT IP_HEX_LENGTH EQUAL 8 OR
       NOT "${${IP_VARIABLE}}" MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "${IP_VARIABLE} must be eight lowercase hexadecimal digits")
    endif()
endforeach()

set(AUTH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(REPO_ROOT "${AUTH_ROOT}/..")
set(MONOCYPHER_ROOT
    "${REPO_ROOT}/deploy/agent/third_party/monocypher")
set(AUTH_GATE_SOURCES
    src/main.c
    "${AUTH_ROOT}/broker/src/vitadebug_attach_auth.c"
    "${AUTH_ROOT}/broker/src/vitadebug_attach_auth_listener.c"
    "${AUTH_ROOT}/broker/src/vitadebug_attach_auth_listener_vita.c"
    "${AUTH_ROOT}/broker/src/vitadebug_attach_auth_store.c"
    "${AUTH_ROOT}/broker/src/vitadebug_attach_auth_store_vita.c"
    "${MONOCYPHER_ROOT}/monocypher.c"
    "${MONOCYPHER_ROOT}/monocypher-ed25519.c"
)
if(NOT "${VD_ATTACH_AUTH_ASSURANCE_SOURCE}" STREQUAL "")
    if(NOT EXISTS "${VD_ATTACH_AUTH_ASSURANCE_SOURCE}" OR
       IS_DIRECTORY "${VD_ATTACH_AUTH_ASSURANCE_SOURCE}")
        message(FATAL_ERROR
            "VD_ATTACH_AUTH_ASSURANCE_SOURCE must name a reviewed source file")
    endif()
    list(APPEND AUTH_GATE_SOURCES "${VD_ATTACH_AUTH_ASSURANCE_SOURCE}")
    set(VD_ATTACH_AUTH_HAS_ASSURANCE 1)
else()
    set(VD_ATTACH_AUTH_HAS_ASSURANCE 0)
endif()

add_executable(vitadebug_attach_auth_gate ${AUTH_GATE_SOURCES})
target_include_directories(vitadebug_attach_auth_gate PRIVATE
    "${AUTH_ROOT}/broker/include"
    "${MONOCYPHER_ROOT}"
)
target_compile_features(vitadebug_attach_auth_gate PRIVATE c_std_11)
target_compile_options(vitadebug_attach_auth_gate PRIVATE
    -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes
    -ffunction-sections -fdata-sections
)
target_compile_definitions(vitadebug_attach_auth_gate PRIVATE
    VD_ATTACH_AUTH_LISTENER_PORT=${VD_ATTACH_AUTH_LISTENER_PORT}
    VD_ATTACH_AUTH_BIND_IPV4=0x${VD_ATTACH_AUTH_BIND_IPV4_HEX}u
    VD_ATTACH_AUTH_PEER_NETWORK=0x${VD_ATTACH_AUTH_PEER_NETWORK_HEX}u
    VD_ATTACH_AUTH_PEER_NETMASK=0x${VD_ATTACH_AUTH_PEER_NETMASK_HEX}u
    VD_ATTACH_AUTH_HANDSHAKE_MS=${VD_ATTACH_AUTH_HANDSHAKE_MS}u
    VD_ATTACH_AUTH_HAS_ASSURANCE=${VD_ATTACH_AUTH_HAS_ASSURANCE}
)
target_link_options(vitadebug_attach_auth_gate PRIVATE
    -Wl,--gc-sections
)
target_link_libraries(vitadebug_attach_auth_gate
    SceCtrl_stub
    SceIofilemgr_stub
    SceKernelThreadMgr_stub
    SceNet_stub
    SceSysmodule_stub
)

# This remains an ordinary safe user-mode fSELF. It has no AppMgr, kernel
# loader, target-process, module-injection, or GDB import.
vita_create_self(eboot.bin vitadebug_attach_auth_gate)
vita_create_vpk(VitaDebuggerAuthGate.vpk ${VITA_TITLE_ID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
