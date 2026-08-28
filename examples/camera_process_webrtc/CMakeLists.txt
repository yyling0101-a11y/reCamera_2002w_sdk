cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(recamera_camera_process_webrtc_example LANGUAGES CXX)
    find_package(recamera-sdk CONFIG REQUIRED)
endif()

add_executable(recamera_camera_process_webrtc main.cpp)
target_compile_features(recamera_camera_process_webrtc PRIVATE cxx_std_17)
target_link_libraries(recamera_camera_process_webrtc PRIVATE recamera::sdk)
