# Face analysis WebRTC example (SG2002W)

This example uses the current reCamera C++ SDK for camera capture, SG2002 TPU
inference, hardware overlay, WebRTC video and a result WebSocket. Its model
pipeline follows the SSCMA `face-analysis` reference:

1. `yolo-face_mixfp16.cvimodel`: face detection, 640x640 RGB.
2. `age_gender_race_bf16.cvimodel`: FairFace, 9 age bins, 2 genders and 7 race labels.
3. `emotion_bf16.cvimodel`: 7-class facial emotion recognition.

The tested model artifacts come from the
[`RobotXTeam/sscma-example-sg200x` v1.0.1 release](https://github.com/RobotXTeam/sscma-example-sg200x/releases/tag/v1.0.1).
They need about 50 MB of ION memory in total. The application limits analysis
to the two highest-scoring faces and runs the raw inference channel at 3 fps so
false detections cannot starve the WebRTC thread. The encoded stream is
640x360 with a four-frame queue so the video buffers, color overlay and all three
models fit in the SG2002W ION pool. The sensor pipeline must remain configured
at its native 30 fps; the current firmware delivers about 10 encoded fps and
about 1 full three-model analysis result per second.

Endpoints after deployment:

- WebRTC player: `http://192.168.42.1:8081/live0`
- analysis WebSocket: `ws://192.168.42.1:8765/`

The overlay box color represents the detected emotion. Emotion IDs are:
`angry`, `disgust`, `fear`, `happy`, `sad`, `surprise`, `neutral`.

Age labels are `0-2`, `3-9`, `10-19`, `20-29`, `30-39`, `40-49`, `50-59`,
`60-69`, and `70+`. Race labels follow FairFace: `White`, `Black`,
`Latino_Hispanic`, `East_Asian`, `Southeast_Asian`, `Indian`, and
`Middle_Eastern`.

Example WebSocket payload:

```json
{"sequence":42,"faces":[{"id":0,"confidence":0.94,"emotion":"happy","emotion_id":3,"emotion_confidence":0.82,"age":"20-29","age_id":3,"age_confidence":0.67,"gender":"female","gender_id":0,"gender_confidence":0.91,"race":"East_Asian","race_id":3,"race_confidence":0.74,"box":{"x":0.51,"y":0.42,"width":0.2,"height":0.3}}]}
```

Run `./convert_models.sh` to fetch the already converted and reference-tested
CV181x models. The script records SHA-256 checksums under `models/reference`.

## Deploy

Copy the executable, the SDK libraries built together with it, and all three
models into the same target directory. Keep the SONAME symlink for the SDK
library because the executable requests `librecamera_sdk.so.0`:

```sh
scp \
  build/examples/face_emotion_webrtc/recamera_face_emotion_webrtc \
  build/librecamera_sdk.so.0.1.0 \
  build/third_party/cvi_rtsp/libcvi_rtsp.so \
  examples/face_emotion_webrtc/S92face-emotion \
  examples/face_emotion_webrtc/models/reference/*.cvimodel \
  root@192.168.42.1:/userdata/face_emotion/

ssh root@192.168.42.1
cd /userdata/face_emotion
ln -sf librecamera_sdk.so.0.1.0 librecamera_sdk.so.0
chmod +x recamera_face_emotion_webrtc
```

## Run manually

The application directory must be the **first** entry in `LD_LIBRARY_PATH`.
This ensures that the executable loads the newly deployed reCamera SDK and
`libcvi_rtsp.so` instead of an older system copy:

```sh
cd /userdata/face_emotion

export LD_LIBRARY_PATH="/userdata/face_emotion:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/mnt/system/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

./recamera_face_emotion_webrtc \
  yolo-face_mixfp16.cvimodel \
  age_gender_race_bf16.cvimodel \
  emotion_bf16.cvimodel 0.70 8081 8765
```

To verify the selected SDK before starting:

```sh
LD_LIBRARY_PATH="/userdata/face_emotion:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/mnt/system/lib" \
  ldd ./recamera_face_emotion_webrtc | grep -E 'recamera_sdk|cvi_rtsp'
```

Both paths should resolve under `/userdata/face_emotion`.

## Run as a service

The supplied init script already applies the same library priority. Install and
start it with:

```sh
cp /userdata/face_emotion/S92face-emotion /etc/init.d/S92face-emotion
chmod +x /etc/init.d/S92face-emotion
/etc/init.d/S92face-emotion restart
/etc/init.d/S92face-emotion status
tail -f /userdata/face_emotion/face-emotion.log
```
