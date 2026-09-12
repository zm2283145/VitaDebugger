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
project(vitadebug_dipsw_read_probe C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VD_DEBUGSCREEN_SOURCE
  "${VITASDK}/share/gcc-arm-vita-eabi/samples/common/debugScreen.c"
)

add_executable(vd_dipsw_read_kernel kernel_probe.c)
target_compile_options(vd_dipsw_read_kernel PRIVATE
  -std=gnu11 -Wall -Wextra -Werror -O2 -ffreestanding
  -fno-optimize-sibling-calls -fno-tree-vectorize -fno-tree-slp-vectorize
  -fno-stack-protector -mgeneral-regs-only
)
target_link_options(vd_dipsw_read_kernel PRIVATE -Wl,-q -nostdlib)
target_include_directories(vd_dipsw_read_kernel PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vd_dipsw_read_kernel
  SceCpuForDriver_stub
  SceDipswForDriver_stub
  SceIofilemgrForDriver_stub
  SceSysclibForDriver_stub
)
vita_create_self(vd-dipsw-read-probe.skprx
  vd_dipsw_read_kernel
  CONFIG kernel_exports.yml
  UNSAFE
)

set(VITA_ELF_CREATE_FLAGS "" CACHE STRING "vita-elf-create flags" FORCE)
set(VITA_MAKE_FSELF_FLAGS "" CACHE STRING "vita-make-fself flags" FORCE)

add_executable(vd_dipsw_read_loader
  loader.c
  ${VD_DEBUGSCREEN_SOURCE}
)
target_compile_options(vd_dipsw_read_loader PRIVATE
  -std=gnu11 -Wall -Wextra -Werror -O2
)
target_include_directories(vd_dipsw_read_loader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  "${VITASDK}/share/gcc-arm-vita-eabi/samples/common"
)
target_link_libraries(vd_dipsw_read_loader
  taihen_stub
  SceCtrl_stub
  SceDisplay_stub
  SceIofilemgr_stub
  SceKernelThreadMgr_stub
)
if(TARGET vd-dipsw-read-probe.skprx-self)
  set(VD_DIPSW_SKPRX_TARGET vd-dipsw-read-probe.skprx-self)
else()
  set(VD_DIPSW_SKPRX_TARGET vd-dipsw-read-probe.skprx)
endif()
add_dependencies(vd_dipsw_read_loader ${VD_DIPSW_SKPRX_TARGET})
vita_create_self(vd-dipsw-read-eboot.bin vd_dipsw_read_loader UNSAFE)
vita_create_vpk(vitadebug-dipsw-read-probe.vpk "VDCP00005"
  vd-dipsw-read-eboot.bin
  VERSION "01.00"
  NAME "VD DIPSW read probe"
  FILE
    ${CMAKE_CURRENT_BINARY_DIR}/vd-dipsw-read-probe.skprx
    module/vd-dipsw-read-probe.skprx
)
if(TARGET vitadebug-dipsw-read-probe.vpk-vpk)
  add_dependencies(vitadebug-dipsw-read-probe.vpk-vpk
    ${VD_DIPSW_SKPRX_TARGET}
  )
elseif(TARGET vitadebug-dipsw-read-probe.vpk)
  add_dependencies(vitadebug-dipsw-read-probe.vpk
    ${VD_DIPSW_SKPRX_TARGET}
  )
endif()
