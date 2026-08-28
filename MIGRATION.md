# Migrating from video_demo

## Old flow

```cpp
initVideo();

video_ch_param_t param{};
param.format = VIDEO_FORMAT_H264;
param.width = 1920;
param.height = 1080;
param.fps = 30;

setupVideo(VIDEO_CH2, &param);
registerVideoFrameHandler(VIDEO_CH2, 0, fpStreamingSendToRtsp, nullptr);
initRtsp(1 << VIDEO_CH2);
startVideo();
```

The application also had to order `deinitVideo()` and `deinitRtsp()`, manage
signals, retain vendor callback context, and diagnose raw CVI errors.

## New flow

```cpp
#include <recamera/camera.hpp>

recamera::Camera camera;

if (!camera.open({1920, 1080, 30})) return 1;
if (!camera.startRtsp({8554, "/live0", 60})) return 1;
if (!camera.start()) return 1;
```

Destruction is automatic. For an explicit shutdown, call `camera.stop()` or
`camera.close()`.

For applications that need AI or custom work before publishing, migrate to the
explicit path:

```cpp
Camera camera;
RtspStream rtsp;

camera.open(...);
rtsp.start(...);
camera.start();

while (running) {
    auto raw = camera.read(timeout);
    if (!raw) continue;
    runInference(*raw);

    auto h264 = camera.readEncoded(timeout);
    if (h264) rtsp.write(*h264);
}
```

The returned raw frame leases a VPSS buffer. Process it immediately and release
it before calling `Camera::stop()`; use `plane()` when the planes are not
contiguous.

This lets the application inspect, infer on, save, or gate each cycle before
publishing. The current `readEncoded()` data is produced from the camera's
parallel hardware VENC path; modifications made through the leased NV21 view are
not yet fed back into VENC.

## Incremental migration

1. Replace direct video initialization with `Camera::open` and `Camera::start`.
2. Replace `rtsp_demo.c` and its VENC callback with `Camera::startRtsp`.
3. Read `camera.lastError()` at application boundaries and
   `camera.streamStatus()` for operational diagnostics.
4. If encoded data is also needed, register `setFrameHandler`. Do not retain
   the borrowed pointer after the callback returns.
5. Remove application calls to `deinitVideo`, `deinitRtsp`, and raw
   `CVI_RTSP_*` functions after all streams have migrated.

Phase 1 intentionally does not migrate AI, Audio, ONVIF, or Web code.
