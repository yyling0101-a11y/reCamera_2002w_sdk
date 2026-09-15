# reCamera C++ SDK (SG2002W)

The SDK provides C++17 Camera, authenticated RTSP, WebRTC, ONVIF, audio, and
fill-light APIs for reCamera 2002W. Camera capture and transports remain separate so an
application can inspect a frame, perform inference or other work, and then
choose which encoded frames to publish. The SG2002 TPU has a generic CVIMODEL
tensor API.

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

## Deploying the ONVIF/Hikvision NVR service on reCamera

`examples/camera_onvif_rtsp` is the deployable Camera → H.264 RTSP → ONVIF
application. It listens on ONVIF HTTP `8000`, WS-Discovery UDP `3702`, and
RTSP `8554` at `/onvif`. Change the example's RTSP/ONVIF credentials before
building; never commit production credentials.

### Compatibility build for older firmware

Some production firmware versions lack the `CVI_NN_*` symbols used by newer
AI components. For an ONVIF/RTSP-only deployment, build the compatibility
variant so AI, audio, OSD, and fill-light modules cannot prevent startup:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DRECAMERA_ENABLE_EXTENDED_APIS=OFF
cmake --build build -j
```

Deploy these artifacts:

```text
build/examples/camera_onvif_rtsp/recamera_camera_onvif_rtsp
build/librecamera_sdk.so.0.1.0
build/third_party/cvi_rtsp/libcvi_rtsp.so
```

The supplied `libcvi_rtsp.so` splits Annex-B H.264 VENC access units into
individual SPS, PPS, and IDR NALs before passing them to Live555. Without it,
strict NVR clients can report `non-existing PPS`, unknown video dimensions,
blank preview, or snapshot failure.

### Upload, backup, and start

Use a staging directory and make a timestamped backup before stopping the
service. Never replace `/mnt/system/usr/lib/libcvi_rtsp.so`: it belongs to the
firmware. Put the fixed library in `/root/libcvi_rtsp.so` and load it first.

```bash
ssh root@DEVICE_IP 'mkdir -p /root/onvif-stage'
scp build/examples/camera_onvif_rtsp/recamera_camera_onvif_rtsp \
    build/librecamera_sdk.so.0.1.0 \
    build/third_party/cvi_rtsp/libcvi_rtsp.so \
    root@DEVICE_IP:/root/onvif-stage/

ssh root@DEVICE_IP
stamp=$(date +%Y%m%d%H%M%S)
pid=$(pidof recamera_camera_onvif_rtsp) && kill -TERM "$pid"
sleep 3
mv /root/recamera_camera_onvif_rtsp /root/recamera_camera_onvif_rtsp.bak-$stamp
mv /root/librecamera_sdk.so.0 /root/librecamera_sdk.so.0.bak-$stamp
[ ! -e /root/libcvi_rtsp.so ] || mv /root/libcvi_rtsp.so /root/libcvi_rtsp.so.bak-$stamp
mv /root/onvif-stage/recamera_camera_onvif_rtsp /root/
mv /root/onvif-stage/librecamera_sdk.so.0.1.0 /root/librecamera_sdk.so.0
ln -sfn librecamera_sdk.so.0 /root/librecamera_sdk.so
mv /root/onvif-stage/libcvi_rtsp.so /root/
chmod 700 /root/recamera_camera_onvif_rtsp /root/librecamera_sdk.so.0 /root/libcvi_rtsp.so

LD_LIBRARY_PATH=/root:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/mnt/system/lib \
  nohup /root/recamera_camera_onvif_rtsp \
  >>/userdata/recamera_sdk/onvif-sdk.log 2>&1 </dev/null &
```

Do not omit `LD_LIBRARY_PATH`: firmware `libini.so` is under
`/mnt/system/usr/lib/3rd` and `libcviruntime.so` is under `/mnt/system/lib`.
After startup, verify both listeners and the loaded libraries:

```bash
pid=$(pidof recamera_camera_onvif_rtsp)
ss -ltnup | grep -E ':(8000|8554|3702)'
grep -E 'librecamera_sdk|libcvi_rtsp' /proc/$pid/maps
tail -n 50 /userdata/recamera_sdk/onvif-sdk.log
```

For a Hikvision ONVIF channel, use management port `8000` and matching ONVIF
credentials. The following read-only checks confirm that its proxy channel is
online and snapshot output is a JPEG:

```bash
curl --digest -u 'NVR_USER:NVR_PASSWORD' \
  http://NVR_IP/ISAPI/ContentMgmt/InputProxy/channels/CHANNEL_ID/status
curl --digest -u 'NVR_USER:NVR_PASSWORD' \
  -o snapshot.jpg http://NVR_IP/ISAPI/Streaming/channels/PROXY_CHANNEL_ID/picture
file snapshot.jpg
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

`streamStatus()` reports sent frames, write errors, RTSP/TCP send errors,
stalled-client disconnects, slow writes, latency, clients, connection totals,
IDR requests, restarts, and recovery level.
Writes slower than 100 ms are warnings by default. Consecutive unhealthy writes
first request an IDR; only sustained failures restart the RTSP service. This is
decoder resynchronization, not RTP retransmission.

For RTSP-over-TCP, a burst of `EAGAIN` send failures is treated as client
backpressure. After eight failures by default, the stalled client socket is
shut down so live555 cleans up the session and the client can reconnect. The
threshold is configurable with `tcpSendFailuresBeforeDisconnect`.

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

## Microphone, speaker, and fill light

The audio API uses the onboard microphone and speaker output. Its defaults
match the documented device format: 16 kHz, mono, S16_LE.

```cpp
#include <recamera/audio.hpp>

recamera::AudioConfig config{16000, 1, 1024};
std::vector<std::int16_t> pcm(config.sampleRate * config.channels);

recamera::Microphone microphone;
microphone.open(config);
microphone.read(pcm.data(), config.sampleRate);
microphone.close();

recamera::Speaker speaker;
speaker.open(config);
speaker.write(pcm.data(), config.sampleRate);
speaker.drain();
```

Fill-light brightness ranges from 0 to 255, with `on()` and `off()` helpers:

```cpp
#include <recamera/light.hpp>

recamera::FillLight light;
light.setBrightness(64);
light.off();
```

`examples/audio_loopback` records three seconds, prints peak/RMS levels, and
plays the capture through the speaker. `examples/fill_light` turns the light on
for two seconds. The device endpoints and formats follow the
[Seeed reCamera 2002 hardware Wiki](https://wiki.seeedstudio.com/recamera_2002_series_hardware_and_specs/).
