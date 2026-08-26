cmake_minimum_required(VERSION 3.16)

project(rk3588_multimodal_alert VERSION 0.1.0 LANGUAGES CXX)

option(P2_BUILD_HARDWARE "Build Linux V4L2 capture application" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(Threads REQUIRED)

add_library(p2_sync STATIC
    src/sync/frame_synchronizer.cpp
)
target_include_directories(p2_sync PUBLIC include)
target_compile_options(p2_sync PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
    add_executable(test_frame_synchronizer tests/test_frame_synchronizer.cpp)
    target_link_libraries(test_frame_synchronizer PRIVATE p2_sync)
    target_compile_options(test_frame_synchronizer PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    add_test(NAME frame_synchronizer COMMAND test_frame_synchronizer)
endif()

if(P2_BUILD_HARDWARE)
    execute_process(
        COMMAND git rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE P2_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT P2_GIT_COMMIT)
        set(P2_GIT_COMMIT "unknown")
    endif()
    set(CAMERA_BSP_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/../rk3588_camera_bsp"
        CACHE PATH "Path to the project-one camera BSP repository")
    set(P2_MLX_UAPI
        "${CAMERA_BSP_ROOT}/include/uapi/linux/zzh_mlx90640_meta.h")
    if(NOT EXISTS "${P2_MLX_UAPI}")
        message(FATAL_ERROR "Missing project-one ZMLX UAPI: ${P2_MLX_UAPI}")
    endif()

    add_executable(p2_capture_sync
        app/p2_capture_sync.cpp
        src/capture/device_discovery.cpp
        src/capture/visible_capture.cpp
        src/capture/thermal_capture.cpp
    )
    target_include_directories(p2_capture_sync PRIVATE include)
    target_include_directories(p2_capture_sync SYSTEM PRIVATE
        "${CAMERA_BSP_ROOT}/include/uapi"
    )
    target_link_libraries(p2_capture_sync PRIVATE p2_sync Threads::Threads)
    target_compile_definitions(p2_capture_sync PRIVATE
        P2_GIT_COMMIT="${P2_GIT_COMMIT}"
    )
    target_compile_options(p2_capture_sync PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    install(TARGETS p2_capture_sync RUNTIME DESTINATION bin)
endif()
