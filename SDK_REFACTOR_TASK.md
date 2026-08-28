# 任务：为 reCamera SG2002W 设计并实现高层 C++ SDK

当前基础仓库：

`Seeed-Studio/sscma-example-sg200x`

当前仓库中已经包含：

```text
components/
├── sophgo
├── sscma-micro
├── mongoose
└── ...

solutions/
├── video_demo
├── audio
├── model_detector
├── yolov8_rtsp
├── onvif_demo
└── ...
```

目前 reCamera SG2002W 的开发流程过于底层。

开发者如果只是希望：

* 打开摄像头
* 获取视频帧
* 启动 RTSP
* 使用 TPU 做 AI 推理

却需要理解大量底层接口，例如：

```text
initVideo
setupVideo
startVideo
registerVideoFrameHandler

APP_DATA_CTX_S
VENC_STREAM_S
VIDEO_FRAME_INFO_S

CVI_VENC_*
CVI_VPSS_*
CVI_RTSP_*
CVI_SYS_*
```

这些接口对于普通 reCamera 应用开发者来说过于复杂。

本任务的目标是：

> 在现有 SSCMA / Sophgo 能力之上，设计一层现代、稳定、易用的 C++17 SDK。

---

# 一、总体目标

最终希望用户只需要编写类似下面的代码：

```cpp
#include <recamera/camera.hpp>

int main()
{
    recamera::Camera camera;

    if (!camera.open({
        .width = 1920,
        .height = 1080,
        .fps = 30,
    })) {
        return 1;
    }

    if (!camera.startRtsp({
        .port = 8554,
        .path = "/live0",
    })) {
        return 1;
    }

    camera.start();

    while (true) {
        sleep(1);
    }
}
```

而不是现在这种：

```cpp
initVideo();

video_ch_param_t param;

param.format = VIDEO_FORMAT_H264;
param.width = 1920;
param.height = 1080;
param.fps = 30;

setupVideo(VIDEO_CH2, &param);

registerVideoFrameHandler(
    VIDEO_CH2,
    0,
    fpStreamingSendToRtsp,
    nullptr
);

initRtsp(1 << VIDEO_CH2);

startVideo();
```

---

# 二、架构原则

新的 SDK 应采用以下分层：

```text
用户应用
   ↓
reCamera C++ SDK
   ↓
Service Layer
   ↓
SG200x Backend / C++ Wrapper
   ↓
现有 SSCMA Components
   ↓
Sophgo CVI_* API
   ↓
驱动 / 硬件
```

非常重要：

## 不要重写 Sophgo 底层

现有以下能力应该继续使用：

```text
CVI_VI_*
CVI_VPSS_*
CVI_VENC_*
CVI_SYS_*
CVI_RTSP_*
```

除非明确证明：

1. bug 位于 vendor 层；
2. 现有 API 无法解决；
3. wrapper/service layer 无法绕过；
4. 必须修改底层才能实现产品需求。

否则不要修改 Sophgo 底层实现。

---

# 三、第一阶段范围

第一阶段只实现：

```text
Camera
+
RTSP
+
CMake
+
示例
+
基本诊断能力
```

暂时不要实现：

```text
AI Model
Audio
ONVIF
WebSocket
完整推理 Pipeline
```

Camera + RTSP 完成并在真实 reCamera 2002W 上验证后，再继续扩展。

---

# 四、建议的新 SDK 目录

创建一个新的 SDK 仓库，例如：

```text
recamera-cpp-sdk/
├── CMakeLists.txt
│
├── include/
│   └── recamera/
│       ├── camera.hpp
│       ├── frame.hpp
│       ├── rtsp.hpp
│       ├── status.hpp
│       └── error.hpp
│
├── src/
│   ├── camera.cpp
│   ├── rtsp.cpp
│   └── status.cpp
│
├── backend/
│   └── sg200x/
│       ├── camera_backend.hpp
│       ├── camera_backend.cpp
│       ├── rtsp_backend.hpp
│       └── rtsp_backend.cpp
│
├── examples/
│   ├── camera_rtsp/
│   │   └── main.cpp
│   └── camera_frame/
│       └── main.cpp
│
├── tests/
│
└── third_party/
```

第一阶段允许通过 Git submodule 依赖：

```text
third_party/sscma-example-sg200x
```

但 SDK 应优先依赖：

```text
components/sophgo
components/sscma-micro
```

不要把：

```text
solutions/video_demo
solutions/yolov8_rtsp
```

作为 SDK 的正式依赖。

可以参考这些 solution 的实现，但不要直接复制它们作为 SDK 架构。

最终目标应该是：

```text
solutions
   ↓
reCamera SDK
   ↓
components
   ↓
Sophgo
```

而不是：

```text
reCamera SDK
   ↓
solutions
```

---

# 五、Camera API

设计 RAII 风格的 C++ API。

建议：

```cpp
namespace recamera {

struct CameraConfig {
    int width = 1920;
    int height = 1080;
    int fps = 30;
};

class Camera {
public:
    Camera();
    ~Camera();

    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    bool open(const CameraConfig& config);

    bool start();

    void stop();

    void close();

    bool isOpen() const;

    bool isRunning() const;

    bool startRtsp(const RtspConfig& config);

    void stopRtsp();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
```

要求：

```text
Camera 析构
↓
自动 stop
↓
自动释放 RTSP
↓
自动释放 Video
↓
自动释放相关资源
```

用户不能因为忘记：

```text
deinitRtsp
deinitVideo
```

而造成资源泄漏。

---

# 六、Frame 抽象

普通用户不应该直接看到：

```text
VIDEO_FRAME_INFO_S
VENC_STREAM_S
VENC_PACK_S
APP_DATA_CTX_S
```

创建一个高层抽象：

```cpp
namespace recamera {

enum class PixelFormat {
    RGB888,
    NV21,
    H264,
    JPEG
};

struct Frame {
    const uint8_t* data = nullptr;
    size_t size = 0;

    int width = 0;
    int height = 0;

    PixelFormat format;

    uint64_t timestamp = 0;
};

}
```

如果底层需要：

```text
DMA
physical address
zero-copy
CVI buffer
```

这些内容必须放在 backend 内部。

不要暴露在公开 API 中。

---

# 七、RTSP API

用户层设计类似：

```cpp
namespace recamera {

struct RtspConfig {
    uint16_t port = 8554;

    std::string path = "/live0";

    int gop = 60;

    bool requestIdrOnConnect = true;

    bool autoRecover = true;
};

class RtspStream {
public:
    bool start(const RtspConfig& config);

    void stop();

    bool running() const;

    StreamStatus status() const;
};

}
```

用户不能直接看到：

```text
CVI_RTSP_CTX*
CVI_RTSP_SESSION*
```

这些全部放在：

```text
backend/sg200x/rtsp_backend.cpp
```

内部管理。

---

# 八、RTSP 底层继续复用现有 API

优先继续使用：

```cpp
CVI_RTSP_Create
CVI_RTSP_Start
CVI_RTSP_CreateSession
CVI_RTSP_WriteFrame
CVI_RTSP_DestroySession
CVI_RTSP_Stop
CVI_RTSP_Destroy
```

不要重新实现 RTSP Server。

不要重新实现 live555。

---

# 九、RTSP 长时间运行稳定性

当前 `video_demo` 已经发现：

长时间运行 ffplay：

```bash
ffplay rtsp://device:8554/live0
```

经过数天后可能发生：

```text
旧 ffplay 卡住
但是新 ffplay 重新连接正常
```

此前日志中出现过：

```text
RTP: missed 1736 packets

RTP: missed 629 packets

RTP: missed 505 packets

CSeq expected != received
```

以及 H264：

```text
decode_slice_header error

non-existing PPS

corrupted macroblock
```

因此新的 RTSP Service 必须增加基本的产品级恢复能力。

---

# 十、RTSP 弱网恢复原则

注意：

UDP RTP 丢失的数据不能由 SDK “找回来”。

本 SDK 的目标不是实现 UDP 重传。

正确目标是：

```text
网络抖动
↓
RTP 丢包
↓
H264 reference chain 可能损坏
↓
主动产生新的 IDR
↓
Decoder 重新同步
↓
继续播放
```

所以不要错误设计成：

```text
UDP packet loss
↓
重新发送历史 packet
```

当前不需要实现 RTP 重传。

---

# 十一、客户端连接时主动 Request IDR

当新的 RTSP client 连接时：

调用现有 VENC API：

```cpp
CVI_VENC_RequestIDR(channel, CVI_TRUE);
```

使客户端尽快获取新的关键帧。

流程：

```text
RTSP Client Connect
↓
request IDR
↓
Encoder 输出新 IDR
↓
Client 建立正确 H264 decoder state
```

这应该作为默认行为。

---

# 十二、GOP

默认 GOP 建议：

```text
fps = 30

GOP = 30 或 60
```

即：

```text
1~2 秒一个 IDR
```

不要默认配置非常长的 GOP。

GOP 应可配置。

---

# 十三、必须修复现有 RTSP Demo 中的问题

参考当前：

```text
solutions/video_demo/main/rtsp_demo.c
```

但不要直接复制问题。

## 问题 1

Destroy Session 时当前逻辑存在：

```cpp
bStart[0]
```

放在循环中的问题。

正确应该使用：

```cpp
bStart[i]
```

同时 destroy 后：

```cpp
pstSession[i] = nullptr;
bStart[i] = false;
```

---

## 问题 2

必须检查：

```cpp
CVI_RTSP_CreateSession()
```

返回值。

只有创建成功：

```cpp
bStart[i] = true;
```

失败必须保持：

```cpp
bStart[i] = false;
```

---

## 问题 3

查找 VENC channel 对应 RTSP session 时：

不要：

```cpp
int idx = 0;
```

因为找不到时会错误使用 session 0。

应该：

```cpp
int idx = -1;
```

找不到后立即返回错误。

---

## 问题 4

`CVI_RTSP_DATA` block 数量有限。

在填写：

```cpp
data.dataPtr[i]
data.dataLen[i]
```

之前必须检查：

```cpp
pstStream->u32PackCount
```

不能超过：

```text
CVI_RTSP_DATA_MAX_BLOCK
```

否则存在数组越界风险。

---

## 问题 5

检查每个 pack：

```text
u32Len > u32Offset
```

否则不得发送。

---

# 十四、给 CVI_RTSP_WriteFrame 增加诊断

每次调用：

```cpp
CVI_RTSP_WriteFrame()
```

使用：

```cpp
std::chrono::steady_clock
```

统计耗时。

例如：

```cpp
auto begin = std::chrono::steady_clock::now();

int ret = CVI_RTSP_WriteFrame(...);

auto end = std::chrono::steady_clock::now();
```

记录：

```text
lastWriteLatencyMs

maxWriteLatencyMs

slowWriteCount

writeErrorCount
```

如果单次耗时超过：

```text
100 ms
```

记录 warning。

但是不要因为一次 slow write 立即重启 RTSP。

---

# 十五、StreamStatus

实现类似：

```cpp
struct StreamStatus {
    bool running = false;

    uint64_t framesSent = 0;

    uint64_t writeErrors = 0;

    uint64_t slowWrites = 0;

    uint64_t lastFrameTimestamp = 0;

    double lastWriteLatencyMs = 0;

    double maxWriteLatencyMs = 0;

    int clients = 0;
};
```

这些用于以后排查：

```text
网络问题

VENC 问题

RTSP 问题

client session 问题
```

---

# 十六、连接状态

当前：

```text
onConnect
onDisconnect
```

不能只打印日志。

需要维护：

```text
当前 client 数量

总连接次数

总断开次数
```

例如：

```cpp
std::atomic<int> clientCount;
```

---

# 十七、恢复机制

实现简单的分级恢复状态。

不要一检测错误就重启 RTSP。

推荐：

```text
正常 Streaming
      ↓
发现可能异常
      ↓
Level 1
request IDR
      ↓
继续观察
      ↓
如果恢复
      ↓
Streaming
```

如果持续出现：

```text
WriteFrame error

WriteFrame 长时间阻塞

stream health 明显异常
```

才进入：

```text
Level 2
restart RTSP service
```

流程：

```text
stop RTSP

destroy session

destroy server

重新 create server

重新 create session

request IDR
```

不要因为单次 UDP 丢包执行 restart。

---

# 十八、不要假设踢掉 Session 后 ffplay 会自动重连

普通 ffplay 被服务端主动断开后：

不一定会自动 reconnect。

因此弱网恢复第一选择应该是：

```text
保持 session
↓
request IDR
↓
让 decoder resync
```

而不是：

```text
立即 disconnect client
```

---

# 十九、线程安全

重点检查：

```text
VENC callback

RTSP write

RTSP connect/disconnect callback

Camera stop

RTSP stop

析构
```

避免：

```text
析构期间 callback 仍访问对象

RTSP 被销毁后 VENC callback 继续 WriteFrame

重复 start

重复 stop
```

允许使用：

```cpp
std::mutex

std::atomic

std::condition_variable
```

但是不要过度设计。

尽量避免额外后台线程。

---

# 二十、错误处理

不要把原始 CVI error code 直接作为公共 API。

设计：

```cpp
enum class ErrorCode {
    Ok,

    InvalidArgument,

    NotInitialized,

    AlreadyRunning,

    BackendError,

    Timeout,

    Unsupported
};
```

内部日志可以保留原始：

```text
CVI error code
```

方便调试。

---

# 二十一、日志

高层 SDK 使用统一日志接口：

```text
ERROR
WARN
INFO
DEBUG
```

避免公共 SDK 中大量：

```cpp
printf()
```

backend 中现有 Sophgo/CVI 日志可以继续保留。

---

# 二十二、CMake

SDK 必须能够编译为：

```text
librecamera_sdk.so
```

优先 shared library。

同时允许：

```text
librecamera_sdk.a
```

如果简单可实现。

提供：

```cmake
add_library(recamera_sdk ...)
```

应用端应该可以：

```cmake
target_link_libraries(
    example
    PRIVATE
    recamera_sdk
)
```

而不需要用户自己再写：

```text
一堆 Sophgo library

一堆 include path

一堆 CVI library
```

这些依赖应该由 SDK target 尽可能隐藏。

---

# 二十三、SSCMA 依赖方式

第一阶段允许：

```text
recamera-cpp-sdk
└── third_party
    └── sscma-example-sg200x
```

使用 Git submodule。

但是不要复制整个 SSCMA 仓库代码。

SDK 应固定依赖某一个 SSCMA commit。

README 中记录：

```text
Compatible SSCMA commit:
xxxxxxxx
```

---

# 二十四、第一阶段必须完成的 Example

创建：

```text
examples/camera_rtsp/main.cpp
```

最终应该尽量控制在非常简单的程度，例如：

```cpp
#include <recamera/camera.hpp>

#include <unistd.h>

int main()
{
    recamera::Camera camera;

    if (!camera.open({
        .width = 1920,
        .height = 1080,
        .fps = 30,
    })) {
        return 1;
    }

    if (!camera.startRtsp({
        .port = 8554,
        .path = "/live0",
    })) {
        return 1;
    }

    if (!camera.start()) {
        return 1;
    }

    while (true) {
        sleep(1);
    }

    return 0;
}
```

这就是新的 SDK 是否成功的核心判断标准：

> 原本几十行初始化 + callback + RTSP 代码，被压缩到非常清晰的 API。

---

# 二十五、暂时不要做的事情

第一阶段不要：

```text
重写 Sophgo driver

修改 ISP driver

修改 VENC driver

重写 cvi_rtsp

重写 live555

实现完整 AI SDK

实现完整 Audio SDK

实现 ONVIF

实现 Web UI

大规模修改原有 solutions
```

优先把：

```text
Camera + RTSP
```

彻底做稳定。

---

# 二十六、如果发现底层必须修改

如果在实现过程中发现：

```text
现有 CVI_* API 无法完成必要能力
```

不要直接修改 vendor code。

先停止该部分实现，并给出报告：

```text
1. 当前想实现什么能力

2. 为什么 wrapper/service layer 无法实现

3. 缺少哪个 CVI API

4. 问题位于哪个 vendor 文件

5. 最小修改方案是什么

6. 修改后是否会影响 ABI / compatibility
```

等人工确认后再修改。

---

# 二十七、实施顺序

严格按照以下顺序：

```text
1. 阅读整个 sscma-example-sg200x 的 CMake 架构

2. 阅读 components/sophgo

3. 阅读 solutions/video_demo

4. 理清：
   Camera
      ↓
   VPSS
      ↓
   VENC
      ↓
   callback
      ↓
   CVI_RTSP_WriteFrame

5. 写一份当前工作流说明

6. 设计公开 C++ header

7. 实现 Camera Backend

8. 实现 RTSP Backend

9. 实现 Camera 高层 API

10. 实现 RTSP diagnostics

11. 实现 connect -> request IDR

12. 实现最基础 auto recovery

13. 完成 camera_rtsp example

14. 编译

15. 修复所有编译错误

16. 确认现有 solutions 不被破坏

17. 编写 README
```

---

# 二十八、第一阶段交付物

最终必须提供：

```text
1. SDK 目录结构

2. Camera 公共 API

3. RTSP 公共 API

4. SG200x backend

5. CMake 构建

6. librecamera_sdk

7. camera_rtsp example

8. RTSP health/status

9. request-IDR-on-connect

10. README

11. ARCHITECTURE.md

12. MIGRATION.md
```

---

# 二十九、ARCHITECTURE.md

需要解释：

```text
Application
     ↓
reCamera C++ SDK
     ↓
Service Layer
     ↓
SG200x Backend
     ↓
SSCMA Components
     ↓
Sophgo CVI API
     ↓
Hardware
```

并明确：

```text
wrapper

service layer

backend

vendor API
```

分别负责什么。

---

# 三十、MIGRATION.md

对比旧代码：

```cpp
initVideo();

setupVideo(...);

registerVideoFrameHandler(...);

initRtsp(...);

startVideo();
```

和新的：

```cpp
Camera camera;

camera.open(...);

camera.startRtsp(...);

camera.start();
```

说明旧项目如何逐步迁移。

---

# 最重要的开发原则

请始终遵循：

> 能在 C++ wrapper / service layer 解决的问题，不修改 Sophgo vendor 层。

当前任务不是重新开发 SG2002W media stack。

当前任务是：

> 把已经存在并且可用的底层能力，封装成一个真正适合 reCamera 用户使用的现代 C++ SDK。

优先目标是：

```text
简单

安全

稳定

易懂

可诊断

可恢复

可维护
```

而不是增加更多功能。
