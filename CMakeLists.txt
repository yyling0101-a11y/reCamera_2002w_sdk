cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain-sg2002w.cmake"
        CACHE FILEPATH "reCamera SG2002W cross-compilation toolchain")
endif()

project(recamera_cpp_sdk VERSION 0.1.0 LANGUAGES C CXX)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/install" CACHE PATH
        "Install prefix" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Firmware libraries live in the board's configured loader paths. Never embed
# workstation SDK paths in deployable RISC-V artifacts.
set(CMAKE_SKIP_RPATH ON)

if(NOT DEFINED SG200X_SDK_PATH AND DEFINED ENV{SG200X_SDK_PATH})
    set(SG200X_SDK_PATH "$ENV{SG200X_SDK_PATH}")
endif()
if(NOT SG200X_SDK_PATH)
    message(FATAL_ERROR "Set SG200X_SDK_PATH to the reCamera 2002W SG2002 SDK root")
endif()

set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sscma")
include("${ROOT_DIR}/cmake/macro.cmake")

# The upstream Sophgo component is built unchanged. Its CMake currently uses
# directory-scoped include/link settings, so it must be included before the SDK
# target is declared.
set(SYSROOT "${SG200X_SDK_PATH}/buildroot-2021.05/output/cvitek_CV181X_musl_riscv64/host/riscv64-buildroot-linux-musl/sysroot")
include_directories("${SYSROOT}/usr/include")
link_directories("${SYSROOT}/usr/lib")
include_directories("${SG200X_SDK_PATH}/install/soc_sg2002_recamera_emmc/tpu_musl_riscv64/cvitek_tpu_sdk/include")
link_directories("${SG200X_SDK_PATH}/install/soc_sg2002_recamera_emmc/tpu_musl_riscv64/cvitek_tpu_sdk/lib")
link_directories("${SG200X_SDK_PATH}/install/soc_sg2002_recamera_emmc/rootfs/mnt/system/lib")

include("${ROOT_DIR}/components/sophgo/CMakeLists.txt")

include(GNUInstallDirs)
add_subdirectory(third_party/cvi_rtsp)

# Lightweight in-process WebRTC media transport. WebSocket, bundled examples,
# and tests are disabled; signaling is provided by the SDK's small HTTP server.
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "" FORCE)
set(OPENSSL_ROOT_DIR "${SYSROOT}/usr" CACHE PATH "" FORCE)
set(OPENSSL_INCLUDE_DIR "${SYSROOT}/usr/include" CACHE PATH "" FORCE)
set(OPENSSL_SSL_LIBRARY "${SYSROOT}/usr/lib/libssl.so" CACHE FILEPATH "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${SYSROOT}/usr/lib/libcrypto.so" CACHE FILEPATH "" FORCE)
add_subdirectory(third_party/libdatachannel EXCLUDE_FROM_ALL)

option(RECAMERA_BUILD_EXAMPLES "Build SDK examples" ON)

add_library(recamera_sdk SHARED
    src/ai.cpp
    src/camera.cpp
    src/frame.cpp
    src/onvif.cpp
    src/osd.cpp
    src/rtsp.cpp
    src/webrtc.cpp
    src/status.cpp
    backend/sg200x/camera_backend.cpp
    backend/sg200x/rtsp_backend.cpp
    backend/sg200x/webrtc_backend.cpp
)
add_library(recamera::sdk ALIAS recamera_sdk)
set_target_properties(recamera_sdk PROPERTIES EXPORT_NAME sdk)

target_include_directories(recamera_sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(recamera_sdk
    PUBLIC
        cvi_rtsp
        ${VIDEO_RREQIRDS}
        "$<BUILD_INTERFACE:${OPENSSL_SSL_LIBRARY}>"
        "$<BUILD_INTERFACE:${OPENSSL_CRYPTO_LIBRARY}>"
        "$<INSTALL_INTERFACE:ssl>"
        "$<INSTALL_INTERFACE:crypto>"
        cviruntime
    PRIVATE sophgo pthread datachannel-static
)
set_target_properties(recamera_sdk PROPERTIES
    OUTPUT_NAME recamera_sdk
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
)

if(RECAMERA_BUILD_EXAMPLES)
    add_subdirectory(examples/camera_rtsp)
    add_subdirectory(examples/camera_process_rtsp)
    add_subdirectory(examples/camera_process_webrtc)
    add_subdirectory(examples/camera_onvif_rtsp)
    add_subdirectory(examples/model_inference)
endif()

install(TARGETS recamera_sdk
    EXPORT recamera-sdk-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/recamera-sdk-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/recamera-sdk-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/recamera-sdk-config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/recamera-sdk"
)
install(EXPORT recamera-sdk-targets
    FILE recamera-sdk-targets.cmake
    NAMESPACE recamera::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/recamera-sdk"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/recamera-sdk-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/recamera-sdk-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/recamera-sdk"
)
install(FILES cmake/toolchain-sg2002w.cmake
    DESTINATION "${CMAKE_INSTALL_DATADIR}/recamera-sdk/toolchain")
install(FILES third_party/sscma/LICENSE third_party/cvi_rtsp/NOTICE.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/recamera-sdk/licenses")
install(DIRECTORY examples/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/recamera-sdk/examples"
    FILES_MATCHING PATTERN "CMakeLists.txt" PATTERN "*.cpp")
