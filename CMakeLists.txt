cmake_minimum_required(VERSION 3.16)

project(rk3588_multimodal_alert VERSION 0.1.0 LANGUAGES CXX)

option(P2_BUILD_HARDWARE "Build Linux V4L2 capture application" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(Threads REQUIRED)

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

add_library(p2_calibration STATIC
    src/calibration/cross_spectral.cpp
)
target_include_directories(p2_calibration PUBLIC include)
target_compile_options(p2_calibration PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

add_executable(p2_cross_spectral_probe
    app/p2_cross_spectral_probe.cpp
)
target_link_libraries(p2_cross_spectral_probe PRIVATE p2_calibration)
target_compile_definitions(p2_cross_spectral_probe PRIVATE
    P2_GIT_COMMIT="${P2_GIT_COMMIT}"
)
target_compile_options(p2_cross_spectral_probe PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

add_executable(p2_calibration_fit
    app/p2_calibration_fit.cpp
)
target_link_libraries(p2_calibration_fit PRIVATE p2_calibration)
target_compile_definitions(p2_calibration_fit PRIVATE
    P2_GIT_COMMIT="${P2_GIT_COMMIT}"
)
target_compile_options(p2_calibration_fit PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

add_library(melexis_mlx90640 STATIC
    third_party/melexis/MLX90640_API.cpp
    src/thermal/mlx90640_i2c_disabled.cpp
)
target_include_directories(melexis_mlx90640 SYSTEM PUBLIC
    third_party/melexis
)

add_library(p2_thermal STATIC
    src/thermal/mlx90640_math.cpp
    src/thermal/temporal_filter.cpp
    src/thermal/colormap.cpp
)
target_include_directories(p2_thermal PUBLIC include)
target_link_libraries(p2_thermal PUBLIC melexis_mlx90640)
target_compile_options(p2_thermal PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

add_executable(p2_thermal_math_probe app/p2_thermal_math_probe.cpp)
target_link_libraries(p2_thermal_math_probe PRIVATE p2_thermal)
target_compile_options(p2_thermal_math_probe PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

add_library(p2_sync STATIC
    src/sync/event_queue.cpp
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

    add_executable(test_mlx90640_math tests/test_mlx90640_math.cpp)
    target_link_libraries(test_mlx90640_math PRIVATE p2_thermal)
    target_compile_options(test_mlx90640_math PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    add_test(NAME mlx90640_validation COMMAND test_mlx90640_math)

    add_executable(test_thermal_processing
        tests/test_thermal_processing.cpp)
    target_link_libraries(test_thermal_processing PRIVATE p2_thermal)
    target_compile_options(test_thermal_processing PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    add_test(NAME thermal_processing COMMAND test_thermal_processing)

    add_executable(test_cross_spectral tests/test_cross_spectral.cpp)
    target_link_libraries(test_cross_spectral PRIVATE p2_calibration)
    target_compile_options(test_cross_spectral PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    add_test(NAME cross_spectral_mapping COMMAND test_cross_spectral)
    set(P2_STAGE2_GOLDEN_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/evidence/stage2/2026-08-27/raw"
        CACHE PATH "Private P2.2 golden input directory")
    if(EXISTS "${P2_STAGE2_GOLDEN_DIR}/mlx90640-eeprom-be.bin" AND
       EXISTS "${P2_STAGE2_GOLDEN_DIR}/zmlx-golden-pair.bin")
        add_test(NAME mlx90640_golden
            COMMAND test_mlx90640_math
                "${P2_STAGE2_GOLDEN_DIR}/mlx90640-eeprom-be.bin"
                "${P2_STAGE2_GOLDEN_DIR}/zmlx-golden-pair.bin")
    else()
        message(STATUS
            "P2.2 private golden inputs absent; mlx90640_golden not registered")
    endif()
endif()

if(P2_BUILD_HARDWARE)
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

    add_executable(p2_thermal_export
        app/p2_thermal_export.cpp
        src/capture/device_discovery.cpp
        src/capture/thermal_capture.cpp
    )
    target_include_directories(p2_thermal_export PRIVATE include)
    target_include_directories(p2_thermal_export SYSTEM PRIVATE
        "${CAMERA_BSP_ROOT}/include/uapi"
    )
    target_link_libraries(p2_thermal_export PRIVATE Threads::Threads)
    target_compile_options(p2_thermal_export PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )

    add_executable(p2_thermal_realtime_probe
        app/p2_thermal_realtime_probe.cpp
        src/capture/device_discovery.cpp
        src/capture/thermal_capture.cpp
    )
    target_include_directories(p2_thermal_realtime_probe PRIVATE include)
    target_include_directories(p2_thermal_realtime_probe SYSTEM PRIVATE
        "${CAMERA_BSP_ROOT}/include/uapi"
    )
    target_link_libraries(p2_thermal_realtime_probe PRIVATE
        p2_thermal Threads::Threads
    )
    target_compile_definitions(p2_thermal_realtime_probe PRIVATE
        P2_GIT_COMMIT="${P2_GIT_COMMIT}"
    )
    target_compile_options(p2_thermal_realtime_probe PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    install(TARGETS
        p2_capture_sync
        p2_thermal_export
        p2_thermal_math_probe
        p2_thermal_realtime_probe
        p2_cross_spectral_probe
        p2_calibration_fit
        RUNTIME DESTINATION bin
    )
endif()
