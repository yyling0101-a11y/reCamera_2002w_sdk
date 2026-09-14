cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(recamera_face_emotion_webrtc_example LANGUAGES CXX)
    find_package(recamera-sdk CONFIG REQUIRED)
endif()

set(CMAKE_SKIP_RPATH OFF)
add_executable(recamera_face_emotion_webrtc main.cpp)
target_compile_features(recamera_face_emotion_webrtc PRIVATE cxx_std_17)
target_link_libraries(recamera_face_emotion_webrtc PRIVATE recamera::sdk)
set_target_properties(recamera_face_emotion_webrtc PROPERTIES
    SKIP_BUILD_RPATH FALSE
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN")
