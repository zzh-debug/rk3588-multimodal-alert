cmake_minimum_required(VERSION 3.16)

project(rk3588_multimodal_alert VERSION 0.1.0 LANGUAGES CXX)

option(P2_BUILD_HARDWARE "Build Linux V4L2 capture application" ON)
option(P2_BUILD_CALIBRATION_GUI
    "Build the Qt cross-spectral calibration assistant" ON)

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
execute_process(
    COMMAND git status --porcelain --untracked-files=normal
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE P2_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(P2_GIT_STATUS AND NOT P2_GIT_COMMIT STREQUAL "unknown")
    string(APPEND P2_GIT_COMMIT "-dirty")
endif()

add_library(p2_calibration STATIC
    src/calibration/cross_spectral.cpp
    src/calibration/assistant.cpp
)
target_include_directories(p2_calibration PUBLIC include)
target_compile_options(p2_calibration PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

add_library(p2_inference_math STATIC
    src/inference/letterbox.cpp
    src/inference/yolov5_person.cpp
)
target_include_directories(p2_inference_math PUBLIC include)
target_compile_options(p2_inference_math PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

add_library(p2_person_evaluation STATIC
    src/evaluation/person_dataset.cpp
    src/evaluation/person_metrics.cpp
)
target_include_directories(p2_person_evaluation PUBLIC include)
target_link_libraries(p2_person_evaluation PUBLIC p2_inference_math)
target_compile_options(p2_person_evaluation PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

add_executable(p2_person_evaluate app/p2_person_evaluate.cpp)
target_link_libraries(p2_person_evaluate PRIVATE p2_person_evaluation)
target_compile_definitions(p2_person_evaluate PRIVATE
    P2_GIT_COMMIT="${P2_GIT_COMMIT}"
)
target_compile_options(p2_person_evaluate PRIVATE
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

add_library(p2_fusion STATIC
    src/fusion/multimodal_fusion.cpp
    src/fusion/temporal_joiner.cpp
)
target_include_directories(p2_fusion PUBLIC include)
target_link_libraries(p2_fusion PUBLIC
    p2_calibration p2_inference_math p2_sync p2_thermal
)
target_compile_options(p2_fusion PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

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

    add_executable(test_calibration_assistant
        tests/test_calibration_assistant.cpp)
    target_link_libraries(test_calibration_assistant PRIVATE p2_calibration)
    target_compile_options(test_calibration_assistant PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME calibration_assistant COMMAND test_calibration_assistant)

    add_executable(test_inference_geometry
        tests/test_inference_geometry.cpp)
    target_link_libraries(test_inference_geometry PRIVATE p2_inference_math)
    target_compile_options(test_inference_geometry PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    add_test(NAME inference_geometry COMMAND test_inference_geometry)

    add_executable(test_yolov5_person tests/test_yolov5_person.cpp)
    target_link_libraries(test_yolov5_person PRIVATE p2_inference_math)
    target_compile_options(test_yolov5_person PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    add_test(NAME yolov5_person_postprocess COMMAND test_yolov5_person)

    add_executable(test_person_evaluation
        tests/test_person_evaluation.cpp)
    target_link_libraries(test_person_evaluation PRIVATE
        p2_person_evaluation)
    target_compile_options(test_person_evaluation PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    add_test(NAME person_evaluation COMMAND test_person_evaluation)

    add_executable(test_bounded_queue tests/test_bounded_queue.cpp)
    target_include_directories(test_bounded_queue PRIVATE include)
    target_link_libraries(test_bounded_queue PRIVATE Threads::Threads)
    target_compile_options(test_bounded_queue PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    add_test(NAME bounded_latest_queue COMMAND test_bounded_queue)

    add_executable(test_multimodal_fusion
        tests/test_multimodal_fusion.cpp)
    target_link_libraries(test_multimodal_fusion PRIVATE p2_fusion)
    target_compile_options(test_multimodal_fusion PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    add_test(NAME multimodal_fusion COMMAND test_multimodal_fusion)
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

    find_path(P2_RKNN_INCLUDE_DIR rknn_api.h
        PATH_SUFFIXES rknn
        REQUIRED
    )
    find_library(P2_RKNN_LIBRARY NAMES rknnrt REQUIRED)
    find_path(P2_RGA_INCLUDE_DIR im2d.h
        PATH_SUFFIXES rga
        REQUIRED
    )
    find_library(P2_RGA_LIBRARY NAMES rga REQUIRED)

    add_library(p2_person_detector STATIC
        src/inference/rknn_person_detector.cpp
    )
    target_include_directories(p2_person_detector PUBLIC include)
    target_include_directories(p2_person_detector SYSTEM PRIVATE
        "${P2_RKNN_INCLUDE_DIR}"
        "${P2_RGA_INCLUDE_DIR}"
    )
    target_link_libraries(p2_person_detector PUBLIC p2_inference_math)
    target_link_libraries(p2_person_detector PRIVATE
        "${P2_RKNN_LIBRARY}"
        "${P2_RGA_LIBRARY}"
    )
    target_compile_options(p2_person_detector PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )

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

    add_executable(p2_calibration_capture
        app/p2_calibration_capture.cpp
        src/capture/device_discovery.cpp
        src/capture/visible_capture.cpp
        src/capture/thermal_capture.cpp
    )
    target_include_directories(p2_calibration_capture PRIVATE include)
    target_include_directories(p2_calibration_capture SYSTEM PRIVATE
        "${CAMERA_BSP_ROOT}/include/uapi"
    )
    target_link_libraries(p2_calibration_capture PRIVATE
        p2_thermal Threads::Threads
    )
    target_compile_definitions(p2_calibration_capture PRIVATE
        P2_GIT_COMMIT="${P2_GIT_COMMIT}"
    )
    target_compile_options(p2_calibration_capture PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )

    if(P2_BUILD_CALIBRATION_GUI)
        find_package(Qt5 COMPONENTS Core Gui Widgets REQUIRED)
        add_executable(p2_calibration_gui
            app/p2_calibration_gui.cpp
            src/capture/device_discovery.cpp
            src/capture/visible_capture.cpp
            src/capture/thermal_capture.cpp
        )
        target_include_directories(p2_calibration_gui PRIVATE include)
        target_include_directories(p2_calibration_gui SYSTEM PRIVATE
            "${CAMERA_BSP_ROOT}/include/uapi"
        )
        target_link_libraries(p2_calibration_gui PRIVATE
            p2_calibration
            p2_thermal
            Threads::Threads
            Qt5::Core
            Qt5::Gui
            Qt5::Widgets
        )
        target_compile_definitions(p2_calibration_gui PRIVATE
            P2_GIT_COMMIT="${P2_GIT_COMMIT}"
        )
        target_compile_options(p2_calibration_gui PRIVATE
            -Wall -Wextra -Wpedantic -Werror
        )
        install(TARGETS p2_calibration_gui RUNTIME DESTINATION bin)
    endif()

    add_executable(p2_person_detect
        app/p2_person_detect.cpp
        src/capture/device_discovery.cpp
        src/capture/visible_capture.cpp
    )
    target_include_directories(p2_person_detect PRIVATE include)
    target_include_directories(p2_person_detect SYSTEM PRIVATE
        "${CAMERA_BSP_ROOT}/include/uapi"
    )
    target_link_libraries(p2_person_detect PRIVATE
        p2_person_detector Threads::Threads
    )
    target_compile_definitions(p2_person_detect PRIVATE
        P2_GIT_COMMIT="${P2_GIT_COMMIT}"
    )
    target_compile_options(p2_person_detect PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )

    add_executable(p2_person_detect_async
        app/p2_person_detect_async.cpp
        src/capture/device_discovery.cpp
        src/capture/visible_capture.cpp
    )
    target_include_directories(p2_person_detect_async PRIVATE include)
    target_include_directories(p2_person_detect_async SYSTEM PRIVATE
        "${CAMERA_BSP_ROOT}/include/uapi"
    )
    target_link_libraries(p2_person_detect_async PRIVATE
        p2_person_detector Threads::Threads
    )
    target_compile_definitions(p2_person_detect_async PRIVATE
        P2_GIT_COMMIT="${P2_GIT_COMMIT}"
    )
    target_compile_options(p2_person_detect_async PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )

    add_executable(p2_multimodal_fusion
        app/p2_multimodal_fusion.cpp
        src/capture/device_discovery.cpp
        src/capture/visible_capture.cpp
        src/capture/thermal_capture.cpp
    )
    target_include_directories(p2_multimodal_fusion PRIVATE include)
    target_include_directories(p2_multimodal_fusion SYSTEM PRIVATE
        "${CAMERA_BSP_ROOT}/include/uapi"
    )
    target_link_libraries(p2_multimodal_fusion PRIVATE
        p2_fusion p2_person_detector p2_thermal Threads::Threads
    )
    target_compile_definitions(p2_multimodal_fusion PRIVATE
        P2_GIT_COMMIT="${P2_GIT_COMMIT}"
    )
    target_compile_options(p2_multimodal_fusion PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )

    add_executable(p2_person_detect_nv12
        app/p2_person_detect_nv12.cpp
    )
    target_link_libraries(p2_person_detect_nv12 PRIVATE p2_person_detector)
    target_compile_definitions(p2_person_detect_nv12 PRIVATE
        P2_GIT_COMMIT="${P2_GIT_COMMIT}"
    )
    target_compile_options(p2_person_detect_nv12 PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )

    add_executable(p2_person_dataset_capture
        app/p2_person_dataset_capture.cpp
        src/capture/device_discovery.cpp
        src/capture/visible_capture.cpp
    )
    target_include_directories(p2_person_dataset_capture PRIVATE include)
    target_include_directories(p2_person_dataset_capture SYSTEM PRIVATE
        "${CAMERA_BSP_ROOT}/include/uapi"
    )
    target_link_libraries(p2_person_dataset_capture PRIVATE
        p2_person_detector p2_person_evaluation Threads::Threads
    )
    target_compile_definitions(p2_person_dataset_capture PRIVATE
        P2_GIT_COMMIT="${P2_GIT_COMMIT}"
    )
    target_compile_options(p2_person_dataset_capture PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    install(TARGETS
        p2_capture_sync
        p2_thermal_export
        p2_thermal_math_probe
        p2_thermal_realtime_probe
        p2_calibration_capture
        p2_cross_spectral_probe
        p2_calibration_fit
        p2_person_detect
        p2_person_detect_async
        p2_multimodal_fusion
        p2_person_detect_nv12
        p2_person_dataset_capture
        p2_person_evaluate
        RUNTIME DESTINATION bin
    )
endif()
