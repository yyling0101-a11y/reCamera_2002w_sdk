# reCamera C++ SDK (SG2002W)

The SDK provides C++17 Camera, authenticated RTSP, WebRTC, and ONVIF services
for reCamera 2002W. Camera capture and transports remain separate so an
application can inspect a frame, perform inference or other work, and then
choose which encoded frames to publish. The SG2002 TPU has a generic CVIMODEL
tensor API; audio is not yet wrapped.

The required SSCMA Sophgo media wrapper source is vendored under
`third_party/sscma` from upstream commit
`c705d180571e11ff41cd56098c0d95734daff521`. The CVI RTSP source is also
vendored and carries the SDK's Digest-authentication and NVR-compatible H.264
SDP changes. No separate SSCMA checkout is required.

## Build

```bash
export SG200X_SDK_PATH=/path/to/sg2002_recamera_emmc
export PATH=/path/to/host-tools/gcc/riscv64-linux-musl-x86_64/bin:$PATH

mkdir -p build install
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
make install
```

The repository's toolchain file is selected automatically. The default install
prefix is the repository's `install/` directory, so no prefix argument is
needed for the workflow above. `SG200X_SDK_PATH` remains necessary because it
contains Sophgo's proprietary board headers and shared libraries; SSCMA source
is not required externally.

Installed outputs include:

```text
install/
├── include/recamera/*.hpp
├── include/cvi_rtsp/*.h
├── lib/librecamera_sdk.so*
├── lib/libcvi_rtsp.so*
├── lib/cmake/recamera-sdk/*.cmake
└── share/recamera-sdk/{examples,toolchain,licenses}/
```

An independent application can consume only the installed package:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_app LANGUAGES CXX)

find_package(recamera-sdk CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE recamera::sdk)
```

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/recamera_sdk/install/share/recamera-sdk/toolchain/toolchain-sg2002w.cmake \
  -DCMAKE_PREFIX_PATH=/path/to/recamera_sdk/install
cmake --build build -j
```

The exported SDK target carries the required SG2002W link dependencies. Public
reCamera headers do not expose CVI or Sophgo types. Every installed example is
also a standalone CMake project and can be configured against this package.

## Composable Camera → processing → RTSP flow

The core API keeps capture and transport separate:

```cpp
using namespace std::chrono_literals;

recamera::Camera camera;
recamera::RtspStream rtsp;

camera.open({1920, 1080, 30, 2});
camera.setGop(60);
camera.start();
rtsp.start({8554, "/live0", 60});

while (running) {
    auto raw = camera.read(1s);          // zero-copy leased NV21
    if (!raw) continue;

    runInference(*raw);                 // AI or other user work

    auto encoded = camera.readEncoded(1s); // owned hardware H.264
    if (encoded) rtsp.write(*encoded);  // explicit publish decision
}
```

The raw path allows one leased frame in flight and naturally applies
backpressure. The encoded queue is bounded by `CameraConfig::queueDepth` and
drops the oldest frame when the consumer falls behind. No DMA or CVI type
crosses the public API.

This phase exposes the original hardware-encoded camera stream alongside the
raw frame. Editing `VideoFrame` pixels does **not** change the paired H.264
stream. Encoding an arbitrary modified CPU frame requires the separate hardware
Encoder backend planned next; the SDK does not pretend that passthrough is
re-encoding.

See `examples/camera_process_rtsp` for the complete explicit flow.

## Authenticated RTSP + ONVIF

RTSP authentication is optional. When enabled, the underlying RTSP server uses
Digest authentication. ONVIF protects device/media operations with WS-Security
UsernameToken and leaves only clock synchronization available anonymously, as
required by common NVR discovery flows.

```cpp
recamera::Camera camera;
recamera::RtspStream rtsp;
recamera::OnvifService onvif;

recamera::RtspConfig rc;
rc.port = 8554;
rc.path = "/onvif";
rc.gop = 30;
rc.authenticationEnabled = true;
rc.username = "admin";
rc.password = "change-me";

camera.open({1920, 1080, 30, 2});
camera.setGop(rc.gop);
camera.start();
rtsp.start(rc);

recamera::OnvifConfig oc;
oc.username = rc.username;
oc.password = rc.password;
oc.rtspPort = rc.port;
oc.rtspPath = rc.path;
oc.gop = rc.gop;
oc.requestKeyframe = [&camera] { return camera.requestIdr(); };
onvif.start(oc);
```

`OnvifConfig::address` may be left empty to advertise a non-loopback IPv4
address. Set it explicitly on devices with multiple active addresses. The
service exposes WS-Discovery on UDP 3702 and the ONVIF device/media endpoint on
port 8000 by default. `requestKeyframe` makes `SetSynchronizationPoint` request
an IDR from the hardware encoder. See `examples/camera_onvif_rtsp` for a
complete explicit Camera → RTSP + ONVIF example.

## WebRTC flow

`WebRtcStream` accepts the same owned hardware H.264 frames as `RtspStream` and
adds in-process ICE, DTLS, SRTP, RTP packetization, HTTP signaling, and a small
browser player. No software video encoder is used.

```cpp
#include <recamera/camera.hpp>
#include <recamera/webrtc.hpp>

recamera::Camera camera;
recamera::WebRtcStream webrtc;

camera.open({1920, 1080, 30, 2});
webrtc.start({8080, "/live0", "0.0.0.0", "", 30, 2});
camera.start();

while (running) {
    auto raw = camera.read(1s);
    if (!raw) continue;
    runInference(*raw);
    raw.reset();

    auto h264 = camera.readEncoded(1s);
    if (h264) webrtc.write(*h264);
}
```

Open `http://192.168.42.1:8080/live0` in a browser on the USB/LAN network.
The default empty `stunServer` uses host ICE candidates only. Set a STUN URL
for routed networks; TURN is not included in phase 1. `WebRtcStatus` reports
clients, connections, frames, bytes, and write errors.

## Convenience flow

```cpp
#include <recamera/camera.hpp>

recamera::Camera camera;
if (!camera.open({1920, 1080, 30})) return 1;
if (!camera.startRtsp({8554, "/live0", 60})) return 1;
if (!camera.start()) return 1;
```

`Camera::startRtsp()` remains a convenience pipeline for applications that do
not need to decide when encoded frames are published.

The destructor stops the VENC worker, destroys RTSP, and releases the video
pipeline. `lastError()` returns a stable SDK error rather than a raw CVI code.

## Diagnostics and recovery

`streamStatus()` reports sent frames, write errors, slow writes, latency,
clients, connection totals, IDR requests, restarts, and recovery level.
Writes slower than 100 ms are warnings by default. Consecutive unhealthy writes
first request an IDR; only sustained failures restart the RTSP service. This is
decoder resynchronization, not RTP retransmission.

New client connections request an IDR by default. Recovery thresholds and GOP
are configurable in `RtspConfig`.

`Camera::read()` returns a lease over the VPSS frame. No full-frame CPU copy is
performed. `data()` addresses plane zero; use `planeCount()` and `plane()` for
formats with multiple non-contiguous planes. Release the frame promptly after
processing and before stopping or destroying the camera so capture can advance.

The optional frame callback receives borrowed H.264 pack memory on the vendor
VENC worker thread. It must return quickly and copy any bytes it needs later.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the dependency and lifetime model and
[MIGRATION.md](MIGRATION.md) for conversion from `video_demo`.

## SG2002 TPU inference

Include only the SDK's public header and run CVIMODEL files through the
firmware-provided `libcviruntime.so`:

```cpp
#include <recamera/ai.hpp>

recamera::Model model;
if (!model.load("/path/to/model.cvimodel")) {
    // model.lastError().message
}

const auto &input = model.inputs().at(0);
std::vector<std::uint8_t> data(input.byteSize);
// Apply the model's documented resize, color, normalization and quantization.
model.setInput(0, data);
model.run();
recamera::Tensor output = model.output(0);
```

`Model` is a generic tensor runner and deliberately does not guess a model's
pre/post-processing contract. `examples/model_inference` is a complete YOLOv8
pipeline: VPSS CH1 emits model-sized RGB888 frames, the TPU runs inference and
YOLOv8 post-processing, a hardware Region overlay draws detections on the
1080p VENC CH2 stream, and WebRTC publishes the encoded frames.

```bash
./recamera_model_inference ./yolov8n.cvimodel 0.5 8080
```

Open `http://device-ip:8080/live0` in a browser.
