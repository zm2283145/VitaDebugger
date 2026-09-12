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
project(vitadebug_staged_readonly_probe C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VD_DEBUGSCREEN_SOURCE
  "${VITASDK}/share/gcc-arm-vita-eabi/samples/common/debugScreen.c"
)

add_executable(vd_read_ladder_kernel kernel_probe.c)
target_compile_options(vd_read_ladder_kernel PRIVATE
  -std=gnu11 -Wall -Wextra -Werror -O2 -ffreestanding
  -fno-optimize-sibling-calls -fno-tree-vectorize -fno-tree-slp-vectorize
  -fno-stack-protector -mgeneral-regs-only
)
target_link_options(vd_read_ladder_kernel PRIVATE -Wl,-q -nostdlib)
target_include_directories(vd_read_ladder_kernel PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vd_read_ladder_kernel
  SceCpuForDriver_stub
  SceIofilemgrForDriver_stub
)
vita_create_self(vd-read-ladder-step.skprx
  vd_read_ladder_kernel
  CONFIG kernel_exports.yml
  UNSAFE
)

# vita_create_self stores conversion flags globally. Clear the kernel export
# configuration before creating the ordinary unsafe user application.
set(VITA_ELF_CREATE_FLAGS "" CACHE STRING "vita-elf-create flags" FORCE)
set(VITA_MAKE_FSELF_FLAGS "" CACHE STRING "vita-make-fself flags" FORCE)

add_executable(vd_read_ladder_loader
  loader.c
  ${VD_DEBUGSCREEN_SOURCE}
)
target_compile_options(vd_read_ladder_loader PRIVATE
  -std=gnu11 -Wall -Wextra -Werror -O2
)
target_include_directories(vd_read_ladder_loader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  "${VITASDK}/share/gcc-arm-vita-eabi/samples/common"
)
target_link_libraries(vd_read_ladder_loader
  taihen_stub
  SceCtrl_stub
  SceDisplay_stub
  SceIofilemgr_stub
  SceKernelThreadMgr_stub
)
if(TARGET vd-read-ladder-step.skprx-self)
  set(VD_READ_LADDER_SKPRX_TARGET vd-read-ladder-step.skprx-self)
else()
  set(VD_READ_LADDER_SKPRX_TARGET vd-read-ladder-step.skprx)
endif()
add_dependencies(vd_read_ladder_loader ${VD_READ_LADDER_SKPRX_TARGET})
vita_create_self(vd-read-ladder-eboot.bin vd_read_ladder_loader UNSAFE)
vita_create_vpk(vitadebug-staged-readonly-probe.vpk "VDCP00004"
  vd-read-ladder-eboot.bin
  VERSION "01.00"
  NAME "VD staged read-only probe"
  FILE
    ${CMAKE_CURRENT_BINARY_DIR}/vd-read-ladder-step.skprx
    module/vd-read-ladder-step.skprx
)
if(TARGET vitadebug-staged-readonly-probe.vpk-vpk)
  add_dependencies(vitadebug-staged-readonly-probe.vpk-vpk
    ${VD_READ_LADDER_SKPRX_TARGET}
  )
elseif(TARGET vitadebug-staged-readonly-probe.vpk)
  add_dependencies(vitadebug-staged-readonly-probe.vpk
    ${VD_READ_LADDER_SKPRX_TARGET}
  )
endif()
