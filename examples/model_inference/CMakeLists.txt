cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(recamera_model_inference_example LANGUAGES CXX)
    find_package(recamera-sdk CONFIG REQUIRED)
endif()

# The top-level SDK disables RPATH for libraries. This deployable example is
# intentionally self-contained and only searches its own directory.
set(CMAKE_SKIP_RPATH OFF)
add_executable(recamera_model_inference main.cpp)
target_compile_features(recamera_model_inference PRIVATE cxx_std_17)
target_link_libraries(recamera_model_inference PRIVATE recamera::sdk)
# Deployment keeps the executable and SDK shared objects together. $ORIGIN is
# resolved by the target loader and does not embed a workstation path.
set_target_properties(recamera_model_inference PROPERTIES
    SKIP_BUILD_RPATH FALSE
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN")
