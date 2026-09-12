cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
            CACHE PATH "VitaSDK toolchain")
    else()
        message(FATAL_ERROR "Set VITASDK before configuring the disposable test target")
    endif()
endif()

project(VitaDevDeployDisposableTest C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaDevDeploy Test Target")
set(VITA_TITLE_ID "VDDT00001")
set(VITA_VERSION "01.00")

add_executable(vitadevdeploy_disposable_test src/main.c)
target_compile_features(vitadevdeploy_disposable_test PRIVATE c_std_99)
target_compile_options(vitadevdeploy_disposable_test PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    -ffunction-sections -fdata-sections
)
target_link_options(vitadevdeploy_disposable_test PRIVATE -Wl,--gc-sections)
target_link_libraries(vitadevdeploy_disposable_test
    SceIofilemgr_stub
)

# Deliberately omit UNSAFE: this disposable target is an ordinary safe fSELF.
vita_create_self(eboot.bin vitadevdeploy_disposable_test)
vita_create_vpk(VitaDevDeployDisposableTest.vpk ${VITA_TITLE_ID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
