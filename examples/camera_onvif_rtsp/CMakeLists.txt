cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(recamera_camera_onvif_rtsp_example LANGUAGES CXX)
    find_package(recamera-sdk CONFIG REQUIRED)
endif()

add_executable(recamera_camera_onvif_rtsp main.cpp)
target_compile_features(recamera_camera_onvif_rtsp PRIVATE cxx_std_17)
target_link_libraries(recamera_camera_onvif_rtsp PRIVATE recamera::sdk)
