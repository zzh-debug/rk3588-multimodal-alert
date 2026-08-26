set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(P2_SDK_ROOT "/rk3588_dev" CACHE PATH "ATK-DLRK3588 SDK root")
set(P2_BUILDROOT_OUTPUT
    "${P2_SDK_ROOT}/buildroot/output/alientek_rk3588"
    CACHE PATH "Buildroot output directory")

set(CMAKE_C_COMPILER
    "${P2_BUILDROOT_OUTPUT}/host/bin/aarch64-buildroot-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER
    "${P2_BUILDROOT_OUTPUT}/host/bin/aarch64-buildroot-linux-gnu-g++")
set(CMAKE_SYSROOT
    "${P2_BUILDROOT_OUTPUT}/host/aarch64-buildroot-linux-gnu/sysroot")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
