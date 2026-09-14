# reCamera C++ SDK (SG2002W)

该 SDK 提供 C++17 摄像头、认证 RTSP、WebRTC、ONVIF、音频和补光灯接口，
适用于 reCamera 2002W。相机采集和传输保持分离，因此
应用程序可以检查帧、执行推理或其他处理，再选择要发布的编码帧。
SG2002 TPU 已提供通用 CVIMODEL tensor 接口。

所需 SSCMA Sophgo 媒体源码已经内置在 `third_party/sscma`，来源提交为
`c705d180571e11ff41cd56098c0d95734daff521`，不需要在同级目录再 clone
`sscma-example-sg200x`。`third_party/cvi_rtsp` 也包含可复现构建的 RTSP
源码，以及 Digest 鉴权和海康 NVR 所需的 H.264 SPS/PPS SDP 修复。

## 建造

```bash
export SG200X_SDK_PATH=/path/to/sg2002_recamera_emmc
export PATH=/path/to/host-tools/gcc/riscv64-linux-musl-x86_64/bin:$PATH

mkdir -p build install
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
make install
```

仓库内置的 SG2002W toolchain 会被自动选中。默认安装前缀就是仓库根目录的
`install/`。仍需设置 `SG200X_SDK_PATH`，因为它提供 Sophgo 的闭源板级头文件
和共享库；这与 SSCMA 源码仓库是两个不同的依赖。

安装结果包含：

```text
install/
├── include/recamera/*.hpp
├── include/cvi_rtsp/*.h
├── lib/librecamera_sdk.so*
├── lib/libcvi_rtsp.so
├── lib/cmake/recamera-sdk/*.cmake
└── share/recamera-sdk/{examples,toolchain,licenses}/
```

## 在 reCamera 上部署 ONVIF/海康 NVR 服务

`examples/camera_onvif_rtsp` 是部署用的 Camera → H.264 RTSP → ONVIF
程序。它默认监听 ONVIF HTTP `8000`、WS-Discovery UDP `3702` 和 RTSP
`8554`，RTSP 路径为 `/onvif`。在将其接入海康 NVR 前，应先修改示例中的
RTSP/ONVIF 用户名和密码，并重新构建；不要把真实凭据提交进仓库。

### 旧固件的兼容构建

部分生产固件的 `libcviruntime.so` 不包含较新 AI 接口需要的
`CVI_NN_*` 符号。只部署 ONVIF/RTSP 时，使用下面的精简构建，避免 AI、音频、
OSD 和补光灯模块造成动态加载失败：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DRECAMERA_ENABLE_EXTENDED_APIS=OFF
cmake --build build -j
```

部署需要的三个产物是：

```text
build/examples/camera_onvif_rtsp/recamera_camera_onvif_rtsp
build/librecamera_sdk.so.0.1.0
build/third_party/cvi_rtsp/libcvi_rtsp.so
```

其中 `libcvi_rtsp.so` 含 H.264 Annex-B NAL 拆分修复：一个 VENC 包里的
SPS、PPS 和 IDR 会分别送给 Live555。缺失该库时，海康常显示预览空白，且
日志或客户端会出现 `non-existing PPS`、未知分辨率或抓图失败。

### 上传、备份和启动

以下命令以 `root@设备IP` 为例。先上传到暂存目录；确认文件齐全后再停服务。
不要覆盖 `/mnt/system/usr/lib/libcvi_rtsp.so`，该文件属于固件；将修复库放在
`/root/libcvi_rtsp.so`，并通过 `LD_LIBRARY_PATH` 优先加载它。

```bash
ssh root@设备IP 'mkdir -p /root/onvif-stage'
scp build/examples/camera_onvif_rtsp/recamera_camera_onvif_rtsp \
    build/librecamera_sdk.so.0.1.0 \
    build/third_party/cvi_rtsp/libcvi_rtsp.so \
    root@设备IP:/root/onvif-stage/

ssh root@设备IP
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

`LD_LIBRARY_PATH` 不能省略：设备的 `libini.so` 位于
`/mnt/system/usr/lib/3rd`，`libcviruntime.so` 位于 `/mnt/system/lib`。程序启动后
确认加载的是 `/root/libcvi_rtsp.so`，而不是固件原版库：

```bash
pid=$(pidof recamera_camera_onvif_rtsp)
ss -ltnup | grep -E ':(8000|8554|3702)'
grep -E 'librecamera_sdk|libcvi_rtsp' /proc/$pid/maps
tail -n 50 /userdata/recamera_sdk/onvif-sdk.log
```

海康 ONVIF 通道应配置管理端口 `8000`、用户名/密码与示例一致。验收时，NVR
代理 RTSP 应能报告 H.264 分辨率和帧率，抓图接口应返回 JPEG：

```bash
curl --digest -u 'NVR用户:NVR密码' \
  http://NVR_IP/ISAPI/ContentMgmt/InputProxy/channels/通道号/status
curl --digest -u 'NVR用户:NVR密码' \
  -o snapshot.jpg http://NVR_IP/ISAPI/Streaming/channels/代理通道号/picture
file snapshot.jpg
```

独立用户工程只需要查找安装后的 SDK：

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_app LANGUAGES CXX)

find_package(recamera-sdk CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE recamera::sdk)
```

配置用户工程时传入安装目录和已安装的 toolchain：

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/recamera_sdk/install/share/recamera-sdk/toolchain/toolchain-sg2002w.cmake \
  -DCMAKE_PREFIX_PATH=/path/to/recamera_sdk/install
cmake --build build -j
```

SDK target 会携带 SG2002W 链接依赖，公共 reCamera 头文件不暴露 CVI 或
Sophgo 类型。安装目录中的每个 example 也都是可以单独配置的 CMake 工程。

## 可组合相机 → 处理 → RTSP 流

核心API将捕获和传输分开：

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

原始路径允许飞行中有一帧租用帧，并且自然地适用
反压。编码队列受以下因素限制： `CameraConfig::queueDepth` 和
当消费者网络延迟时，丢弃最旧的帧。无DMA或CVI类型。
跨越公共 API。

此阶段会同时显示原始硬件编码的摄像头视频流和
原始帧。编辑 `VideoFrame` 像素不会改变配对的 H.264
对任意修改后的 CPU 帧进行编码需要单独的硬件。
接下来计划开发编码器后端；SDK 并不假定支持直通模式。
重新编码。

看 `examples/camera_process_rtsp` 完整显式流程。

## 已认证的 RTSP + ONVIF

RTSP 身份验证是可选的。启用后，底层 RTSP 服务器将使用
摘要式身份验证。ONVIF 使用 WS-Security 保护设备/媒体操作。
UsernameToken 仅允许匿名进行时钟同步，如
常见NVR发现流程所必需。

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

`OnvifConfig::address` 可以留空以通告非环回 IPv4 地址。
地址。在具有多个活动地址的设备上，请显式设置此地址。
服务在 UDP 3702 端口上公开 WS-Discovery，并在 ONVIF 设备上公开媒体端点。
默认端口为 8000。 `requestKeyframe` 制作 `SetSynchronizationPoint` 要求
来自硬件编码器的IDR。参见 `examples/camera_onvif_rtsp` 为
完整的显式 Camera → RTSP + ONVIF 示例。

## WebRTC 流

`WebRtcStream` 接受与自有硬件相同的 H.264 帧 `RtspStream` 和
增加了进程内 ICE、DTLS、SRTP、RTP 数据包化、HTTP 信令以及少量其他功能。
浏览器播放器。未使用任何软件视频编码器。

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

打开 `http://192.168.42.1:8080/live0` 在 USB/LAN 网络上的浏览器中。
默认为空 `stunServer` 仅使用主机 ICE 候选地址。设置 STUN URL
对于路由网络；TURN 不包含在第一阶段中。 `WebRtcStatus` 报告
客户端、连接、帧、字节和写入错误。

## 便利流

```cpp
#include <recamera/camera.hpp>

recamera::Camera camera;
if (!camera.open({1920, 1080, 30})) return 1;
if (!camera.startRtsp({8554, "/live0", 60})) return 1;
if (!camera.start()) return 1;
```

`Camera::startRtsp()` 仍然是应用程序的便捷管道
无需决定何时发布编码帧。

销毁器会停止 VENC 工作进程，销毁 RTSP，并释放视频。
管道。 `lastError()` 返回稳定的 SDK 错误，而不是原始 CVI 代码。

## 诊断和恢复

`streamStatus()` 报告发送帧、写入错误、写入速度慢、延迟等问题
客户端、连接总数、IDR 请求、重启和恢复级别。
默认情况下，写入速度慢于 100 毫秒的操作会发出警告。连续出现不正常的写入操作
首先请求 IDR；只有在持续故障的情况下才会重启 RTSP 服务。
解码器重新同步，而非RTP重传。

新客户端连接默认请求 IDR。恢复阈值和 GOP
可在以下方面进行配置： `RtspConfig`。

`Camera::read()` 返回对 VPSS 帧的租约。没有全帧 CPU 复制。
已执行。 `data()` 地址平面零；使用 `planeCount()` 和 `plane()` 为了
具有多个不连续平面的格式。拍摄结束后立即释放帧。
处理完成后，在停止或销毁相机之前，以便继续拍摄。

可选的帧回调函数接收供应商提供的借用的 H.264 包内存。
VENC 工作线程。它必须快速返回，并在之后复制所需的任何字节。

看 [ARCHITECTURE.md](ARCHITECTURE.md) 对于依赖关系和生命周期模型
[MIGRATION.md](MIGRATION.md) 用于从 `video_demo`。

## SG2002 TPU 推理

SDK 直接包装设备固件已有的 `libcviruntime.so`，应用只需要包含本 SDK 的头文件：

```cpp
#include <recamera/ai.hpp>

recamera::Model model;
if (!model.load("/path/to/model.cvimodel")) {
    // model.lastError().message
}

const auto &input = model.inputs().at(0);
std::vector<std::uint8_t> data(input.byteSize);
// 按模型的输入契约完成 resize、颜色转换、归一化和量化后填入 data。
model.setInput(0, data);
model.run();
recamera::Tensor output = model.output(0);
```

`Model` 是通用 tensor 推理接口，不猜测模型的预处理和后处理规则。输入字节数必须与
`TensorInfo::byteSize` 完全一致；量化参数可从 `quantizationScale` 和
`quantizationZeroPoint` 获取。

`examples/model_inference` 是完整的 YOLOv8 管线：VPSS CH1 按模型尺寸输出 RGB888，
TPU 执行推理和 YOLOv8 后处理，硬件 Region OSD 将检测框挂到 1080p VENC CH2，编码帧
由 WebRTC 发布。设备端运行：

```bash
./recamera_model_inference ./yolov8n.cvimodel 0.5 8080
```

浏览器打开 `http://设备IP:8080/live0`。

## 麦克风、喇叭与补光灯

音频接口使用板载麦克风和喇叭输出，默认采用设备官方配置：16 kHz、单声道、
S16_LE。调用方只需包含本 SDK 头文件：

```cpp
#include <recamera/audio.hpp>

recamera::AudioConfig config{16000, 1, 1024};
std::vector<std::int16_t> pcm(config.sampleRate * config.channels);

recamera::Microphone microphone;
microphone.open(config);
microphone.read(pcm.data(), config.sampleRate); // 阻塞读取一秒
microphone.close();

recamera::Speaker speaker;
speaker.open(config);
speaker.write(pcm.data(), config.sampleRate);
speaker.drain();
```

补光灯支持 0–255 亮度以及快捷开关：

```cpp
#include <recamera/light.hpp>

recamera::FillLight light;
light.setBrightness(64);
light.off();
```

`examples/audio_loopback` 会录音三秒并从喇叭回放，同时打印峰值与 RMS；
`examples/fill_light` 会点亮补光灯两秒后关闭。接口参数和硬件节点依据
[Seeed reCamera 2002 硬件 Wiki](https://wiki.seeedstudio.com/recamera_2002_series_hardware_and_specs/)。
