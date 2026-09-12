cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK toolchain")
  else()
    message(FATAL_ERROR "VITASDK must point to the VitaSDK installation")
  endif()
endif()

project(vitadebug_kernel C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

option(VITADEBUG_EXPERIMENTAL_VFP_SNAPSHOT
  "Compile the unverified ksceKernelGetVfpRegisterForDebugger layout probe"
  OFF
)

add_executable(vitadebug_kernel
  src/main.c
)

target_compile_options(vitadebug_kernel PRIVATE
  -Wall -Wextra -Werror -O2 -ffreestanding -fno-optimize-sibling-calls
)
if(VITADEBUG_EXPERIMENTAL_VFP_SNAPSHOT)
  target_compile_definitions(vitadebug_kernel PRIVATE
    VD_KERNEL_ENABLE_EXPERIMENTAL_VFP_SNAPSHOT=1
  )
endif()
target_link_options(vitadebug_kernel PRIVATE -Wl,-q -nostdlib)

target_include_directories(vitadebug_kernel PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(vitadebug_kernel
  SceSysmemForDriver_stub
  SceThreadmgrForDriver_stub
)

vita_create_self(vitadebug.skprx vitadebug_kernel CONFIG exports.yml UNSAFE)
vita_create_stubs(vitadebug_stubs vitadebug_kernel ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml KERNEL)

add_custom_target(vitadebug_kernel_all ALL
  DEPENDS vitadebug.skprx
  DEPENDS libvitadebug_kernel_stub.a
  DEPENDS libvitadebug_kernel_stub_weak.a
)

add_executable(vitadebug_probe
  test/probe.c
  ${VITASDK}/share/gcc-arm-vita-eabi/samples/common/debugScreen.c
)
target_compile_options(vitadebug_probe PRIVATE -Wall -Wextra -Werror -O2)
add_dependencies(vitadebug_probe vitadebug_stubs)
target_include_directories(vitadebug_probe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${VITASDK}/share/gcc-arm-vita-eabi/samples/common
)
target_link_libraries(vitadebug_probe
  ${CMAKE_BINARY_DIR}/vitadebug_stubs/libvitadebug_kernel_stub.a
  SceDisplay_stub
)

# vita_create_self stores flags globally; clear the kernel export configuration
# before creating the ordinary user application in the same CMake project.
set(VITA_ELF_CREATE_FLAGS "" CACHE STRING "vita-elf-create flags" FORCE)
set(VITA_MAKE_FSELF_FLAGS "" CACHE STRING "vita-make-fself flags" FORCE)
vita_create_self(vitadebug-probe-eboot.bin vitadebug_probe UNSAFE)
vita_create_vpk(vitadebug-kernel-probe.vpk "VDCP00001"
  vitadebug-probe-eboot.bin
  VERSION "01.00"
  NAME "VitaDebugger kernel probe"
)

if(VITADEBUG_EXPERIMENTAL_VFP_SNAPSHOT)
  # Keep the undocumented VFP layout check in a separate, disposable probe.
  # Besides making the risk explicit, this keeps the stable boundary/watchdog
  # probe usable if a toolchain cannot package the VFP test instructions.
  add_executable(vitadebug_vfp_probe
    test/vfp_probe.c
    ${VITASDK}/share/gcc-arm-vita-eabi/samples/common/debugScreen.c
  )
  target_compile_options(vitadebug_vfp_probe PRIVATE
    -Wall -Wextra -Werror -O2
  )
  add_dependencies(vitadebug_vfp_probe vitadebug_stubs)
  target_include_directories(vitadebug_vfp_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VITASDK}/share/gcc-arm-vita-eabi/samples/common
  )
  target_link_libraries(vitadebug_vfp_probe
    ${CMAKE_BINARY_DIR}/vitadebug_stubs/libvitadebug_kernel_stub.a
    SceDisplay_stub
  )
  vita_create_self(vitadebug-vfp-probe-eboot.bin vitadebug_vfp_probe UNSAFE)
  vita_create_vpk(vitadebug-vfp-probe.vpk "VDCP00002"
    vitadebug-vfp-probe-eboot.bin
    VERSION "01.00"
    NAME "VitaDebugger guarded VFP probe"
  )
endif()

install(DIRECTORY ${CMAKE_BINARY_DIR}/vitadebug_stubs/
  DESTINATION lib
  FILES_MATCHING PATTERN "*.a"
)

install(FILES include/vitadebug_kernel.h DESTINATION include)
