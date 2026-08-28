# reCamera 2002W device validation

Validation date: 2026-08-25

## Device

- Address: `192.168.42.1` over USB networking
- Architecture: RISC-V 64-bit
- Kernel: Linux 5.10.4
- Root filesystem: Buildroot 2021.05
- Sensor reported by the media stack: OV5647, 1920x1080, 30 fps mode

## Resource release

The factory services were stopped using the official Seeed procedure:

```sh
/etc/init.d/S03node-red stop
/etc/init.d/S91sscma-node stop
/etc/init.d/S93sscma-supervisor stop
```

After shutdown, the Node-RED and SSCMA processes were absent and ports 1880
and 8090 were no longer listening.

On 2026-08-27, boot startup was disabled reversibly by renaming the three init
scripts to names that do not match Buildroot's `S*` boot scan:

```text
/etc/init.d/disabled-S03node-red
/etc/init.d/disabled-S91sscma-node
/etc/init.d/disabled-S93sscma-supervisor
```

The changes reside in the persistent `/userdata/.overlay_fs/etc` overlay. To
restore factory startup, rename each file back to its original `S*` name.

## Deployment

Artifacts were installed under `/userdata/recamera_sdk`:

- `librecamera_sdk.so.0.1.0`
- `librecamera_sdk.so.0` symlink
- `recamera_camera_rtsp`

Runtime command:

```sh
cd /userdata/recamera_sdk
LD_LIBRARY_PATH=/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/usr/lib:. \
  ./recamera_camera_rtsp
```

## Results

- Camera SYS/VI/VPSS/VENC initialization: pass
- RTSP listener on TCP 8554: pass
- RTSP URL: `rtsp://192.168.42.1:8554/live0`
- Codec: H.264 Constrained Baseline
- Resolution: 1920x1080
- Eight-second TCP RTSP decode with FFmpeg: pass, no decode errors
- Connect-triggered IDR: pass (`idrRequests` increased from 0 to 1)
- Graceful SIGTERM teardown: pass; ISP memory released and port 8554 closed
- Restart after teardown: pass
- RTSP write errors: 0
- Slow writes above 100 ms: 0
- Maximum observed write latency during a client session: 30.608 ms
- Automatic RTSP restarts: 0

### Composable pipeline validation (2026-08-27)

- `Camera::read()` zero-copy leased NV21 path: pass
- `Camera::readEncoded()` owned H.264 path: pass
- Application-controlled `RtspStream::write()`: pass
- More than 1,300 raw frames processed and encoded frames explicitly written
- RTSP write errors: 0
- Eight-second FFmpeg TCP decode: pass
- One transient missing-PPS warning was observed immediately on client attach;
  the connect-triggered IDR restored decoding and the stream continued normally
- Four writes slightly exceeded the default 100 ms warning threshold during
  the two client sessions; no recovery restart was triggered

## Open observation

The encoded-frame callback advanced by approximately 50 frames every five
seconds (about 10 fps). FFmpeg likewise inferred a 10 fps transport cadence,
although the sensor startup log and H.264 metadata report a 30 fps mode and the
public configuration requested 30 fps.

This does not prevent stable streaming, but the 30 fps configuration contract
is not yet proven on this firmware. Determining whether the cadence originates
in the existing SG200x VENC configuration, firmware RTSP implementation, or
sensor exposure policy requires a focused vendor-layer diagnostic. No Sophgo,
CVI, or vendor source was modified during this validation.
