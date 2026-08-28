# Architecture

## Layers

```text
Application
    |
Public C++ API (Camera, VideoFrame, EncodedFrame, RtspStream, StreamStatus)
    |
Camera service / RAII lifecycle (src/)
    |
SG200x backend (backend/sg200x/)
    |
SSCMA Sophgo component wrappers (video.h, app_ipcam_*)
    |
Sophgo CVI SYS / VI / VPSS / VENC / RTSP APIs
    |
SG2002W hardware and firmware
```

The public wrapper validates configuration and hides ownership. The service
layer coordinates Camera and RTSP state and enforces teardown order. The SG200x
backend translates C++ values to existing component calls and owns all vendor
types. Vendor APIs continue to implement capture, processing, encoding, and the
RTSP server.

No SDK public header includes a CVI or Sophgo header. No source under
`components/sophgo`, `cvi_mpi`, `cvi_rtsp`, or another vendor directory is
copied or modified.

## Existing video_demo call chain

```text
main
  -> initVideo
       -> app_ipcam_Param_Load
  -> setupVideo(VIDEO_CH2, H264)
       -> configure VB pool
       -> configure VPSS group/channel
       -> configure VENC channel
  -> registerVideoFrameHandler
       -> app_ipcam_Venc_Consumes_Set
  -> initRtsp
       -> CVI_RTSP_Create
       -> CVI_RTSP_Start
       -> CVI_RTSP_CreateSession
       -> CVI_RTSP_SetListener
  -> startVideo
       -> app_ipcam_Sys_Init
       -> app_ipcam_Vi_Init
       -> app_ipcam_Vpss_Init
       -> app_ipcam_Venc_Init
       -> app_ipcam_Venc_Start
            -> VENC worker thread
                 -> CVI_VENC_GetStream
                 -> registered consumer callback
                      -> CVI_RTSP_WriteFrame
                 -> CVI_VENC_ReleaseStream
```

The SDK keeps this media path. It replaces only application-level setup,
ownership, callback routing, status collection, and recovery policy.

## CMake dependency chain

The upstream solution includes `cmake/project.cmake`, which discovers requested
components through `component_register`. `video_demo/main` requests `sophgo`
and `cvi_rtsp`. The `sophgo` target compiles common + video sources and links
the MPI, ISP, sensor, and helper libraries listed in `VIDEO_RREQIRDS`.

This SDK includes the upstream `components/sophgo/CMakeLists.txt` directly and
links it privately. The board shared libraries are carried by the
`recamera_sdk` target's link interface so a consumer names only
`recamera::sdk`. `solutions/video_demo` is a reference, never a build
dependency. `sscma-micro` is not linked in phase 1 because Camera + RTSP does
not use it.

## Ownership and shutdown

The composable path uses independent `Camera` and `RtspStream` objects:

```text
VPSS CH1 -> one in-flight leased NV21 frame -> Camera::read -> user processing
VENC CH2 -> bounded owned H.264 queue -> Camera::readEncoded
          -> RtspStream::write -> CVI_RTSP_WriteFrame
```

The raw path maps the VPSS planes and lends them to `VideoFrame` without a
multi-megabyte CPU copy. The upstream callback remains blocked while the lease
is held and releases the VPSS frame after the `VideoFrame` is destroyed. This
provides natural backpressure: applications must release each frame promptly
and before stopping the camera. The encoded queue remains bounded and discards
the oldest frame.

For backward compatibility, `Camera::Impl` also owns one internal `RtspBackend`
used only by `Camera::startRtsp()`. Shutdown order is:

```text
mark VENC stopped -> join VENC worker -> destroy RTSP session/server
-> unregister callback -> close video state
```

Joining VENC first guarantees no frame callback can enter a destroyed RTSP
object. RTSP write/start/stop/restart operations share one mutex. Status uses a
separate mutex so connect/disconnect callbacks can update counters safely.

`VideoFrame` owns a lease rather than copied pixels. The callback waits for the
lease to end before returning, which prevents the upstream component from
releasing the VPSS buffer too early. `EncodedFrame` preserves the VENC pack
boundaries required by `CVI_RTSP_DATA`; this path still copies the much smaller
encoded access unit because the upstream component frees it after the callback.
Arbitrary processed-frame re-encoding is deliberately not claimed until a
dedicated SG200x hardware Encoder backend exists.

The optional WebRTC backend embeds libdatachannel 0.24.5 with its examples,
tests, and WebSocket support disabled. Its small HTTP signaling server exchanges
non-trickle SDP and serves the test player. Encoded H.264 access units are
packetized into RTP and protected by DTLS-SRTP; camera encoding remains on CVI
VENC. The default configuration is direct USB/LAN host ICE. Public headers do
not expose libdatachannel, OpenSSL, SRTP, or vendor types.

## RTSP correctness and health

The backend validates pack count against `CVI_RTSP_DATA_MAX_BLOCK`, validates
every offset, checks session creation, and never defaults a missing VENC/session
mapping to session zero. Session pointers and running state are cleared during
destruction.

Each write uses `steady_clock`. A single slow write only records a warning.
Consecutive unhealthy writes trigger Level 1 (IDR), followed by Level 2 (RTSP
server/session recreation) only at the configured sustained threshold. A
successful write resets the consecutive-health counter.

There is intentionally no RTP history or retransmission implementation.
