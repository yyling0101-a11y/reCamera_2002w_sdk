# CVI RTSP is derived from the Apache-2.0 SSCMA SG2002W source. This local
# target includes reCamera changes for Digest authentication and standards-
# compliant H.264 fmtp/sprop-parameter-sets generation.
set(_cvi_media_sdk "${SG200X_SDK_PATH}/install/soc_sg2002_recamera_emmc/tpu_musl_riscv64/cvitek_tpu_sdk")

add_library(cvi_rtsp SHARED src/api.cpp src/tcp_send_recovery.cpp)
add_library(recamera::cvi_rtsp ALIAS cvi_rtsp)
set_target_properties(cvi_rtsp PROPERTIES
    OUTPUT_NAME cvi_rtsp
    EXPORT_NAME cvi_rtsp
    POSITION_INDEPENDENT_CODE ON
)
target_compile_definitions(cvi_rtsp PRIVATE NO_OPENSSL=1 ARCH_CV182X)
target_include_directories(cvi_rtsp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        "${_cvi_media_sdk}/include/BasicUsageEnvironment"
        "${_cvi_media_sdk}/include/groupsock"
        "${_cvi_media_sdk}/include/liveMedia"
        "${_cvi_media_sdk}/include/UsageEnvironment"
)
target_link_libraries(cvi_rtsp PRIVATE
    "${_cvi_media_sdk}/lib/libliveMedia.a"
    "${_cvi_media_sdk}/lib/libBasicUsageEnvironment.a"
    "${_cvi_media_sdk}/lib/libUsageEnvironment.a"
    "${_cvi_media_sdk}/lib/libgroupsock.a"
    pthread
)
# The vendor live555 is supplied as a static archive. Interpose its send(2)
# calls so repeated RTP-over-TCP backpressure can be surfaced and the stale
# client socket can be shut down instead of remaining wedged indefinitely.
target_link_options(cvi_rtsp PRIVATE "-Wl,--wrap=send")

install(TARGETS cvi_rtsp
    EXPORT recamera-sdk-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/cvi_rtsp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
