cmake_minimum_required(VERSION 3.16)

project(camera_detection LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CAMERA_DETECTION_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(CAMERA_DETECTION_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(CMAKE_CROSSCOMPILING)
    set(CAMERA_DETECTION_PI_DEFAULT ON)
else()
    set(CAMERA_DETECTION_PI_DEFAULT OFF)
endif()

option(CAMERA_DETECTION_BUILD_PI_APP
    "Build Raspberry Pi hardware and WebRTC modules"
    ${CAMERA_DETECTION_PI_DEFAULT}
)

if(CAMERA_DETECTION_BUILD_PI_APP)
    set(OPENCV_INCLUDE_DIR
        "/home/rz/Learning/Raspberry_Pi/rpi-sysroot/usr/include/opencv4"
        CACHE PATH "Path to Raspberry Pi OpenCV headers"
    )

    if(NOT EXISTS "${OPENCV_INCLUDE_DIR}/opencv2/opencv.hpp")
        message(FATAL_ERROR "OpenCV header not found at: ${OPENCV_INCLUDE_DIR}")
    endif()

    find_package(OpenCV REQUIRED CONFIG
        COMPONENTS core imgproc highgui videoio imgcodecs
        PATHS "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/cmake/opencv4"
        NO_DEFAULT_PATH
    )
else()
    find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui videoio imgcodecs)
endif()

find_package(Threads REQUIRED)

add_library(perception_core STATIC
    "${CAMERA_DETECTION_SRC_DIR}/camera/camera_pipeline.cpp"
    "${CAMERA_DETECTION_SRC_DIR}/camera/frame_source.cpp"
    "${CAMERA_DETECTION_SRC_DIR}/detection/ball_detector.cpp"
)

target_include_directories(perception_core
    PUBLIC
        "${CAMERA_DETECTION_INCLUDE_DIR}"
        ${OpenCV_INCLUDE_DIRS}
)

if(CAMERA_DETECTION_BUILD_PI_APP)
    target_include_directories(perception_core PRIVATE "${OPENCV_INCLUDE_DIR}")
endif()

target_link_libraries(perception_core PUBLIC ${OpenCV_LIBS} Threads::Threads)

add_executable(camera_detection
    "${CAMERA_DETECTION_SRC_DIR}/main.cpp"
    "${CAMERA_DETECTION_SRC_DIR}/utils/signal_utils.cpp"
)

target_link_libraries(camera_detection PRIVATE perception_core)

if(CAMERA_DETECTION_BUILD_PI_APP)
    set(GSTREAMER_ROOT "${CMAKE_SYSROOT}/usr")
    set(GSTREAMER_INCLUDE_DIR "${GSTREAMER_ROOT}/include/gstreamer-1.0")
    set(GLIB_INCLUDE_DIR "${GSTREAMER_ROOT}/include/glib-2.0")
    set(GLIB_CONFIG_INCLUDE_DIR
        "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/glib-2.0/include"
    )

    if(NOT EXISTS "${GSTREAMER_INCLUDE_DIR}/gst/gst.h")
        message(FATAL_ERROR "GStreamer headers not found at: ${GSTREAMER_INCLUDE_DIR}")
    endif()

    if(NOT EXISTS "${GSTREAMER_INCLUDE_DIR}/gst/app/gstappsrc.h")
        message(FATAL_ERROR
            "GStreamer appsrc headers not found at: ${GSTREAMER_INCLUDE_DIR}/gst/app"
        )
    endif()

    if(NOT EXISTS "${GSTREAMER_INCLUDE_DIR}/gst/sdp/gstsdpmessage.h")
        message(FATAL_ERROR
            "GStreamer SDP headers not found at: ${GSTREAMER_INCLUDE_DIR}/gst/sdp"
        )
    endif()

    if(NOT EXISTS "${GSTREAMER_INCLUDE_DIR}/gst/webrtc/webrtc.h")
        message(FATAL_ERROR
            "Missing gstwebrtc development headers in the Raspberry Pi sysroot. "
            "Sync the gstreamer WebRTC dev package into ${CMAKE_SYSROOT} before "
            "building camera_detection."
        )
    endif()

    foreach(lib_name
            gstreamer-1.0
            gstapp-1.0
            gstsdp-1.0
            gstrtp-1.0
            gstvideo-1.0
            gstwebrtc-1.0
            glib-2.0
            gobject-2.0)
        string(REPLACE "-" "_" lib_var "${lib_name}")
        string(REPLACE "." "_" lib_var "${lib_var}")
        string(TOUPPER "${lib_var}" lib_var)
        find_library(${lib_var}_LIBRARY
            NAMES ${lib_name}
            PATHS
                "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu"
                "${CMAKE_SYSROOT}/usr/local/lib"
            NO_DEFAULT_PATH
        )

        if(NOT ${lib_var}_LIBRARY)
            message(FATAL_ERROR
                "Required library ${lib_name} was not found in the Raspberry Pi sysroot"
            )
        endif()
    endforeach()

    target_sources(camera_detection PRIVATE
        "${CAMERA_DETECTION_SRC_DIR}/control/pan_tracker.cpp"
        "${CAMERA_DETECTION_SRC_DIR}/hardware/PCA9685.c"
        "${CAMERA_DETECTION_SRC_DIR}/hardware/http_server.cpp"
        "${CAMERA_DETECTION_SRC_DIR}/streaming/webrtc_streamer.cpp"
        "${CAMERA_DETECTION_SRC_DIR}/utils/file_utils.cpp"
        "${CAMERA_DETECTION_SRC_DIR}/utils/http_utils.cpp"
    )

    target_compile_definitions(camera_detection
        PRIVATE
            CAMERA_DETECTION_WITH_PI_MODULES=1
            GST_USE_UNSTABLE_API
    )

    target_include_directories(camera_detection
        PRIVATE
            "${OPENCV_INCLUDE_DIR}"
            "${GSTREAMER_INCLUDE_DIR}"
            "${GLIB_INCLUDE_DIR}"
            "${GLIB_CONFIG_INCLUDE_DIR}"
    )

    target_link_libraries(camera_detection
        PRIVATE
            ${GSTREAMER_1_0_LIBRARY}
            ${GSTAPP_1_0_LIBRARY}
            ${GSTSDP_1_0_LIBRARY}
            ${GSTRTP_1_0_LIBRARY}
            ${GSTVIDEO_1_0_LIBRARY}
            ${GSTWEBRTC_1_0_LIBRARY}
            ${GLIB_2_0_LIBRARY}
            ${GOBJECT_2_0_LIBRARY}
    )

    target_link_options(camera_detection PRIVATE -Wl,--allow-shlib-undefined)

    set(CAMERA_DETECTION_FRONTEND
        "${CAMERA_DETECTION_SRC_DIR}/streaming/index.html"
    )

    add_custom_command(TARGET camera_detection POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CAMERA_DETECTION_FRONTEND}"
            "$<TARGET_FILE_DIR:camera_detection>/index.html"
    )

    install(FILES "${CAMERA_DETECTION_FRONTEND}" DESTINATION bin)
else()
    target_compile_definitions(camera_detection
        PRIVATE CAMERA_DETECTION_WITH_PI_MODULES=0
    )
endif()

install(TARGETS camera_detection RUNTIME DESTINATION bin)
