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
project(vitadebug_thread_setter_resolver_probe C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VD_DEBUGSCREEN_SOURCE
  "${VITASDK}/share/gcc-arm-vita-eabi/samples/common/debugScreen.c"
)

add_executable(vd_thread_setter_resolver_kernel kernel_probe.c)
target_compile_options(vd_thread_setter_resolver_kernel PRIVATE
  -std=gnu11 -Wall -Wextra -Werror -O2 -ffreestanding
  -fno-optimize-sibling-calls -fno-tree-vectorize -fno-tree-slp-vectorize
  -fno-stack-protector -mgeneral-regs-only
)
target_link_options(vd_thread_setter_resolver_kernel PRIVATE -Wl,-q -nostdlib)
target_include_directories(vd_thread_setter_resolver_kernel PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vd_thread_setter_resolver_kernel
  taihenForKernel_stub
  taihenModuleUtils_stub
  SceIofilemgrForDriver_stub
  SceModulemgrForDriver_stub
  SceSysclibForDriver_stub
)
vita_create_self(vd-thread-setter-resolver.skprx
  vd_thread_setter_resolver_kernel
  CONFIG kernel_exports.yml
  UNSAFE
)

set(VITA_ELF_CREATE_FLAGS "" CACHE STRING "vita-elf-create flags" FORCE)
set(VITA_MAKE_FSELF_FLAGS "" CACHE STRING "vita-make-fself flags" FORCE)

add_executable(vd_ts_resolver_loader
  loader.c
  ${VD_DEBUGSCREEN_SOURCE}
)
target_compile_options(vd_ts_resolver_loader PRIVATE
  -std=gnu11 -Wall -Wextra -Werror -Os
)
target_include_directories(vd_ts_resolver_loader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  "${VITASDK}/share/gcc-arm-vita-eabi/samples/common"
)
target_link_libraries(vd_ts_resolver_loader
  taihen_stub
  SceCtrl_stub
  SceDisplay_stub
  SceIofilemgr_stub
  SceKernelThreadMgr_stub
)
if(TARGET vd-thread-setter-resolver.skprx-self)
  set(VD_THREAD_SETTER_RESOLVER_SKPRX_TARGET
    vd-thread-setter-resolver.skprx-self)
else()
  set(VD_THREAD_SETTER_RESOLVER_SKPRX_TARGET
    vd-thread-setter-resolver.skprx)
endif()
add_dependencies(vd_ts_resolver_loader
  ${VD_THREAD_SETTER_RESOLVER_SKPRX_TARGET})
vita_create_self(vd-thread-setter-resolver-eboot.bin
  vd_ts_resolver_loader CONFIG loader_exports.yml UNSAFE)
vita_create_vpk(vitadebug-thread-setter-resolver-probe.vpk "VDCP00008"
  vd-thread-setter-resolver-eboot.bin
  VERSION "01.00"
  NAME "VD ThreadMgr resolver"
  FILE
    ${CMAKE_CURRENT_BINARY_DIR}/vd-thread-setter-resolver.skprx
    module/vd-thread-setter-resolver.skprx
)
if(TARGET vitadebug-thread-setter-resolver-probe.vpk-vpk)
  add_dependencies(vitadebug-thread-setter-resolver-probe.vpk-vpk
    ${VD_THREAD_SETTER_RESOLVER_SKPRX_TARGET})
elseif(TARGET vitadebug-thread-setter-resolver-probe.vpk)
  add_dependencies(vitadebug-thread-setter-resolver-probe.vpk
    ${VD_THREAD_SETTER_RESOLVER_SKPRX_TARGET})
endif()
