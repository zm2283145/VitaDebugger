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

project(vitadebug_one_shot_hw_probe C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VD_SHARED_KERNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(VD_DEBUGSCREEN_SOURCE
  "${VITASDK}/share/gcc-arm-vita-eabi/samples/common/debugScreen.c"
)

add_executable(vd_hw_disabled_probe_kernel
  kernel_probe.c
  ${VD_SHARED_KERNEL_DIR}/src/armv7_debug_codec.c
)
target_compile_options(vd_hw_disabled_probe_kernel PRIVATE
  -std=gnu11 -Wall -Wextra -Werror -O2 -ffreestanding
  -fno-optimize-sibling-calls -fno-tree-vectorize -fno-tree-slp-vectorize
  -mgeneral-regs-only
)
target_link_options(vd_hw_disabled_probe_kernel PRIVATE -Wl,-q -nostdlib)
target_include_directories(vd_hw_disabled_probe_kernel PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${VD_SHARED_KERNEL_DIR}/include
)
target_link_libraries(vd_hw_disabled_probe_kernel
  SceCpuForDriver_stub
  SceIofilemgrForDriver_stub
  SceSysclibForDriver_stub
)
vita_create_self(vd-hw-disabled-probe.skprx
  vd_hw_disabled_probe_kernel
  CONFIG kernel_exports.yml
  UNSAFE
)

# vita_create_self stores conversion flags globally. Clear the kernel export
# configuration before creating the ordinary unsafe user application.
set(VITA_ELF_CREATE_FLAGS "" CACHE STRING "vita-elf-create flags" FORCE)
set(VITA_MAKE_FSELF_FLAGS "" CACHE STRING "vita-make-fself flags" FORCE)

add_executable(vd_hw_probe_loader
  loader.c
  ${VD_DEBUGSCREEN_SOURCE}
)
target_compile_options(vd_hw_probe_loader PRIVATE
  -std=gnu11 -Wall -Wextra -Werror -O2
)
target_include_directories(vd_hw_probe_loader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  "${VITASDK}/share/gcc-arm-vita-eabi/samples/common"
)
target_link_libraries(vd_hw_probe_loader
  taihen_stub
  SceCtrl_stub
  SceDisplay_stub
  SceIofilemgr_stub
  SceKernelThreadMgr_stub
)
if(TARGET vd-hw-disabled-probe.skprx-self)
  set(VD_HW_PROBE_SKPRX_TARGET vd-hw-disabled-probe.skprx-self)
else()
  set(VD_HW_PROBE_SKPRX_TARGET vd-hw-disabled-probe.skprx)
endif()
add_dependencies(vd_hw_probe_loader ${VD_HW_PROBE_SKPRX_TARGET})
vita_create_self(vd-hw-probe-eboot.bin vd_hw_probe_loader UNSAFE)
vita_create_vpk(vitadebug-one-shot-hw-probe.vpk "VDCP00003"
  vd-hw-probe-eboot.bin
  VERSION "01.00"
  NAME "VitaDebugger safe HW probe"
  FILE
    ${CMAKE_CURRENT_BINARY_DIR}/vd-hw-disabled-probe.skprx
    module/vd-hw-disabled-probe.skprx
)
if(TARGET vitadebug-one-shot-hw-probe.vpk-vpk)
  add_dependencies(vitadebug-one-shot-hw-probe.vpk-vpk
    ${VD_HW_PROBE_SKPRX_TARGET}
  )
elseif(TARGET vitadebug-one-shot-hw-probe.vpk)
  add_dependencies(vitadebug-one-shot-hw-probe.vpk
    ${VD_HW_PROBE_SKPRX_TARGET}
  )
endif()
