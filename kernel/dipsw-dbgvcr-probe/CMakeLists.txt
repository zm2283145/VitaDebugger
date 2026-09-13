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
project(vitadebug_dipsw_dbgvcr_probe C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VD_DEBUGSCREEN_SOURCE
  "${VITASDK}/share/gcc-arm-vita-eabi/samples/common/debugScreen.c"
)

add_executable(vd_dipsw_dbgvcr_kernel kernel_probe.c)
target_compile_options(vd_dipsw_dbgvcr_kernel PRIVATE
  -std=gnu11 -Wall -Wextra -Werror -O2 -ffreestanding
  -fno-optimize-sibling-calls -fno-tree-vectorize -fno-tree-slp-vectorize
  -fno-stack-protector -mgeneral-regs-only
)
target_link_options(vd_dipsw_dbgvcr_kernel PRIVATE -Wl,-q -nostdlib)
target_include_directories(vd_dipsw_dbgvcr_kernel PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vd_dipsw_dbgvcr_kernel
  SceCpuForDriver_stub
  SceDipswForDriver_stub
  SceIofilemgrForDriver_stub
  SceSysclibForDriver_stub
)
vita_create_self(vd-dipsw-dbgvcr-probe.skprx
  vd_dipsw_dbgvcr_kernel
  CONFIG kernel_exports.yml
  UNSAFE
)

set(VITA_ELF_CREATE_FLAGS "" CACHE STRING "vita-elf-create flags" FORCE)
set(VITA_MAKE_FSELF_FLAGS "" CACHE STRING "vita-make-fself flags" FORCE)

add_executable(vd_dipsw_dbgvcr_loader
  loader.c
  ${VD_DEBUGSCREEN_SOURCE}
)
target_compile_options(vd_dipsw_dbgvcr_loader PRIVATE
  -std=gnu11 -Wall -Wextra -Werror -O2
)
target_include_directories(vd_dipsw_dbgvcr_loader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  "${VITASDK}/share/gcc-arm-vita-eabi/samples/common"
)
target_link_libraries(vd_dipsw_dbgvcr_loader
  taihen_stub
  SceCtrl_stub
  SceDisplay_stub
  SceIofilemgr_stub
  SceKernelThreadMgr_stub
)
if(TARGET vd-dipsw-dbgvcr-probe.skprx-self)
  set(VD_DIPSW_DBGVCR_SKPRX_TARGET
    vd-dipsw-dbgvcr-probe.skprx-self
  )
else()
  set(VD_DIPSW_DBGVCR_SKPRX_TARGET vd-dipsw-dbgvcr-probe.skprx)
endif()
add_dependencies(vd_dipsw_dbgvcr_loader ${VD_DIPSW_DBGVCR_SKPRX_TARGET})
vita_create_self(vd-dipsw-dbgvcr-eboot.bin
  vd_dipsw_dbgvcr_loader UNSAFE)
vita_create_vpk(vitadebug-dipsw-dbgvcr-probe.vpk "VDCP00007"
  vd-dipsw-dbgvcr-eboot.bin
  VERSION "01.00"
  NAME "VD DIP 228 DBGVCR probe"
  FILE
    ${CMAKE_CURRENT_BINARY_DIR}/vd-dipsw-dbgvcr-probe.skprx
    module/vd-dipsw-dbgvcr-probe.skprx
)
if(TARGET vitadebug-dipsw-dbgvcr-probe.vpk-vpk)
  add_dependencies(vitadebug-dipsw-dbgvcr-probe.vpk-vpk
    ${VD_DIPSW_DBGVCR_SKPRX_TARGET}
  )
elseif(TARGET vitadebug-dipsw-dbgvcr-probe.vpk)
  add_dependencies(vitadebug-dipsw-dbgvcr-probe.vpk
    ${VD_DIPSW_DBGVCR_SKPRX_TARGET}
  )
endif()
