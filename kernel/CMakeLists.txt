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
option(VITADEBUG_EXPERIMENTAL_HW_DEBUG
  "Compile the guarded ARMv7 hardware-breakpoint/watchpoint session engine"
  OFF
)
option(VITADEBUG_EXPERIMENTAL_PMU_PROFILER
  "Compile the lease-protected fixed-core/lane PMU profiler transport"
  OFF
)
option(VITADEBUG_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS
  "Admit the three reviewed real PMU events (requires PMU profiler)"
  OFF
)
set(VITADEBUG_PMU_PROFILER_GATE_EVENT "0x01" CACHE STRING
  "One real event compiled into the disposable PMU profiler gate"
)

if(VITADEBUG_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS AND
   NOT VITADEBUG_EXPERIMENTAL_PMU_PROFILER)
  message(FATAL_ERROR
    "VITADEBUG_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS requires VITADEBUG_EXPERIMENTAL_PMU_PROFILER")
endif()
if(VITADEBUG_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS AND
   NOT (VITADEBUG_PMU_PROFILER_GATE_EVENT STREQUAL "0x01" OR
        VITADEBUG_PMU_PROFILER_GATE_EVENT STREQUAL "0x03" OR
        VITADEBUG_PMU_PROFILER_GATE_EVENT STREQUAL "0x10"))
  message(FATAL_ERROR
    "VITADEBUG_PMU_PROFILER_GATE_EVENT must be exactly 0x01, 0x03, or 0x10")
endif()

add_executable(vitadebug_kernel
  src/main.c
  src/thread_mutation.c
)
if(VITADEBUG_EXPERIMENTAL_HW_DEBUG)
  target_sources(vitadebug_kernel PRIVATE
    src/hw_debug.c
    src/armv7_debug_codec.c
  )
endif()
if(VITADEBUG_EXPERIMENTAL_PMU_PROFILER)
  target_sources(vitadebug_kernel PRIVATE
    src/pmu_session.c
    src/pmu_backend.c
    src/pmu_profiler_bridge.c
    src/pmu_profiler_transport.c
  )
endif()

target_compile_options(vitadebug_kernel PRIVATE
  -Wall -Wextra -Werror -O2 -ffreestanding -fno-optimize-sibling-calls
  -mgeneral-regs-only -fno-tree-vectorize -fno-tree-slp-vectorize
)
if(VITADEBUG_EXPERIMENTAL_VFP_SNAPSHOT)
  target_compile_definitions(vitadebug_kernel PRIVATE
    VD_KERNEL_ENABLE_EXPERIMENTAL_VFP_SNAPSHOT=1
  )
endif()
if(VITADEBUG_EXPERIMENTAL_HW_DEBUG)
  target_compile_definitions(vitadebug_kernel PRIVATE
    VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG=1
  )
endif()
if(VITADEBUG_EXPERIMENTAL_PMU_PROFILER)
  target_compile_definitions(vitadebug_kernel PRIVATE
    VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION=1
    VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_TRANSPORT=1
    VD_PMU_PROFILER_BRIDGE_NO_DICTIONARY=1
  )
endif()
if(VITADEBUG_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS)
  target_compile_definitions(vitadebug_kernel PRIVATE
    VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS=1
  )
endif()
target_link_options(vitadebug_kernel PRIVATE -Wl,-q -nostdlib)

target_include_directories(vitadebug_kernel PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../profiler/include
)

target_link_libraries(vitadebug_kernel
  SceCpuForDriver_stub
  SceSysclibForDriver_stub
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

if(VITADEBUG_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS)
  add_executable(vitadebug_pmu_profiler_gate
    pmu-profiler-gate/main.c
    ${VITASDK}/share/gcc-arm-vita-eabi/samples/common/debugScreen.c
  )
  target_compile_options(vitadebug_pmu_profiler_gate PRIVATE
    -Wall -Wextra -Werror -O2
  )
  target_compile_definitions(vitadebug_pmu_profiler_gate PRIVATE
    VD_PMU_PROFILER_GATE_EVENT_CODE=${VITADEBUG_PMU_PROFILER_GATE_EVENT}
  )
  add_dependencies(vitadebug_pmu_profiler_gate vitadebug_stubs)
  target_include_directories(vitadebug_pmu_profiler_gate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VITASDK}/share/gcc-arm-vita-eabi/samples/common
  )
  target_link_libraries(vitadebug_pmu_profiler_gate
    ${CMAKE_BINARY_DIR}/vitadebug_stubs/libvitadebug_kernel_stub.a
    SceCtrl_stub
    SceDisplay_stub
    SceIofilemgr_stub
    SceKernelThreadMgr_stub
  )
  vita_create_self(vitadebug-pmu-profiler-gate-eboot.bin
    vitadebug_pmu_profiler_gate UNSAFE)
  vita_create_vpk(vitadebug-pmu-profiler-gate.vpk "VDCP00010"
    vitadebug-pmu-profiler-gate-eboot.bin
    VERSION "01.00"
    NAME "VD PMU profiler gate"
  )
endif()

install(DIRECTORY ${CMAKE_BINARY_DIR}/vitadebug_stubs/
  DESTINATION lib
  FILES_MATCHING PATTERN "*.a"
)

install(FILES include/vitadebug_kernel.h DESTINATION include)
install(FILES include/vitadebug_pmu_profiler.h DESTINATION include)
