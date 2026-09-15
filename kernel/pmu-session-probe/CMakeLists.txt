cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE
      "$ENV{VITASDK}/share/vita.toolchain.cmake"
      CACHE PATH "VitaSDK toolchain"
    )
  else()
    message(FATAL_ERROR "VITASDK must point to the VitaSDK installation")
  endif()
endif()
project(vitadebug_pmu_session_probe C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VD_DEBUGSCREEN_SOURCE
  "${VITASDK}/share/gcc-arm-vita-eabi/samples/common/debugScreen.c"
)
set(VD_PMU_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_executable(vd_pmu_session_probe_kernel
  kernel_probe.c
  "${VD_PMU_SOURCE_DIR}/pmu_backend.c"
  "${VD_PMU_SOURCE_DIR}/pmu_session.c"
)
target_compile_definitions(vd_pmu_session_probe_kernel PRIVATE
  VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION=1
)
target_compile_options(vd_pmu_session_probe_kernel PRIVATE
  -std=gnu11 -Wall -Wextra -Werror -O2 -ffreestanding
  -fno-optimize-sibling-calls -fno-tree-vectorize -fno-tree-slp-vectorize
  -fno-stack-protector -mgeneral-regs-only
)
target_link_options(vd_pmu_session_probe_kernel PRIVATE -Wl,-q -nostdlib)
target_include_directories(vd_pmu_session_probe_kernel PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${VD_PMU_SOURCE_DIR}
)
target_link_libraries(vd_pmu_session_probe_kernel
  SceCpuForDriver_stub
  SceIofilemgrForDriver_stub
  SceSysmemForDriver_stub
  SceSysclibForDriver_stub
  SceThreadmgrForDriver_stub
)
vita_create_self(vitadebug-pmu-session-probe.skprx
  vd_pmu_session_probe_kernel
  CONFIG kernel_exports.yml
  UNSAFE
)
vita_create_stubs(vd_pmu_session_probe_stubs
  vd_pmu_session_probe_kernel
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel_exports.yml
  KERNEL
)

# vita_create_self stores kernel export flags globally. Clear them before
# producing the ordinary user-mode UI.
set(VITA_ELF_CREATE_FLAGS "" CACHE STRING "vita-elf-create flags" FORCE)
set(VITA_MAKE_FSELF_FLAGS "" CACHE STRING "vita-make-fself flags" FORCE)

add_executable(vd_pmu_session_probe_loader
  loader.c
  ${VD_DEBUGSCREEN_SOURCE}
)
target_compile_definitions(vd_pmu_session_probe_loader PRIVATE
  VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION=1
)
target_compile_options(vd_pmu_session_probe_loader PRIVATE
  -std=gnu11 -Wall -Wextra -Werror -O2
)
target_include_directories(vd_pmu_session_probe_loader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${VD_PMU_SOURCE_DIR}
  "${VITASDK}/share/gcc-arm-vita-eabi/samples/common"
)
add_dependencies(vd_pmu_session_probe_loader
  vd_pmu_session_probe_stubs)
target_link_libraries(vd_pmu_session_probe_loader
  ${CMAKE_BINARY_DIR}/vd_pmu_session_probe_stubs/libvitadebug_pmu_probe_stub.a
  SceCtrl_stub
  SceDisplay_stub
  SceKernelThreadMgr_stub
)

if(TARGET vitadebug-pmu-session-probe.skprx-self)
  set(VD_PMU_SESSION_SKPRX_TARGET
    vitadebug-pmu-session-probe.skprx-self)
else()
  set(VD_PMU_SESSION_SKPRX_TARGET
    vitadebug-pmu-session-probe.skprx)
endif()
add_dependencies(vd_pmu_session_probe_loader ${VD_PMU_SESSION_SKPRX_TARGET})

vita_create_self(vitadebug-pmu-session-probe-eboot.bin
  vd_pmu_session_probe_loader UNSAFE)
vita_create_vpk(vitadebug-pmu-session-probe.vpk "VDCP00009"
  vitadebug-pmu-session-probe-eboot.bin
  VERSION "01.00"
  NAME "VD PMU gate"
  FILE
    ${CMAKE_CURRENT_BINARY_DIR}/vitadebug-pmu-session-probe.skprx
    module/vitadebug-pmu-session-probe.skprx
)
if(TARGET vitadebug-pmu-session-probe.vpk-vpk)
  add_dependencies(vitadebug-pmu-session-probe.vpk-vpk
    ${VD_PMU_SESSION_SKPRX_TARGET})
elseif(TARGET vitadebug-pmu-session-probe.vpk)
  add_dependencies(vitadebug-pmu-session-probe.vpk
    ${VD_PMU_SESSION_SKPRX_TARGET})
endif()
