#include <recamera/ai.hpp>
#include <recamera/camera.hpp>
#include <recamera/osd.hpp>
#include <recamera/webrtc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {
using recamera::DetectionBox;
std::atomic<bool> running{true};
void stop(int) { running = false; }

std::size_t featureCount(int w, int h) {
    std::size_t n = 0;
    for (int stride : {8, 16, 32}) n += static_cast<std::size_t>(w / stride) * (h / stride);
    return n;
}

float valueAt(const recamera::Tensor& tensor, int channel, std::size_t position,
              int channels, std::size_t positions) {
    const bool positionMajor = tensor.info.shape.size() >= 3 && tensor.info.shape.back() == channels;
    const std::size_t index = positionMajor ? position * channels + channel
                                            : static_cast<std::size_t>(channel) * positions + position;
    float value = 0.0F;
    std::memcpy(&value, tensor.data.data() + index * sizeof(float), sizeof(value));
    return value;
}

float iou(const DetectionBox& a, const DetectionBox& b) {
    const float ax1 = a.x - a.width * .5F, ay1 = a.y - a.height * .5F;
    const float ax2 = a.x + a.width * .5F, ay2 = a.y + a.height * .5F;
    const float bx1 = b.x - b.width * .5F, by1 = b.y - b.height * .5F;
    const float bx2 = b.x + b.width * .5F, by2 = b.y + b.height * .5F;
    const float intersection = std::max(0.0F, std::min(ax2, bx2) - std::max(ax1, bx1)) *
                               std::max(0.0F, std::min(ay2, by2) - std::max(ay1, by1));
    const float area = a.width * a.height + b.width * b.height - intersection;
    return area > 0.0F ? intersection / area : 0.0F;
}

void nms(std::vector<DetectionBox>& boxes, float threshold) {
    std::sort(boxes.begin(), boxes.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    std::vector<DetectionBox> kept;
    for (const auto& candidate : boxes) {
        bool suppressed = false;
        for (const auto& selected : kept) {
            if (candidate.classId == selected.classId && iou(candidate, selected) > threshold) {
                suppressed = true;
                break;
            }
        }
        if (!suppressed) kept.push_back(candidate);
    }
    boxes.swap(kept);
}

bool prepareRgb(const recamera::VideoFrame& frame, int width, int height,
                std::vector<std::uint8_t>& input) {
    if (frame.format() != recamera::PixelFormat::RGB888 || frame.width() != width ||
        frame.height() != height || frame.planeCount() == 0) return false;
    const auto plane = frame.plane(0);
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 3;
    const std::size_t stride = plane.stride >= rowBytes ? plane.stride : rowBytes;
    if (!plane.data || plane.size < stride * height || input.size() != rowBytes * height) return false;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    auto* q = reinterpret_cast<std::int8_t*>(input.data());
    for (int y = 0; y < height; ++y) {
        const auto* row = plane.data + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            const std::size_t p = static_cast<std::size_t>(y) * width + x;
            q[p] = static_cast<std::int8_t>(static_cast<int>(row[x * 3]) - 128);
            q[pixels + p] = static_cast<std::int8_t>(static_cast<int>(row[x * 3 + 1]) - 128);
            q[pixels * 2 + p] = static_cast<std::int8_t>(static_cast<int>(row[x * 3 + 2]) - 128);
        }
    }
    return true;
}

bool decodeYolo(const recamera::Tensor& boxes, const recamera::Tensor& scores, int width, int height,
                float scoreThreshold, std::vector<DetectionBox>& detections) {
    const std::size_t positions = featureCount(width, height);
    if (!positions || boxes.info.dataType != recamera::TensorDataType::Float32 ||
        scores.info.dataType != recamera::TensorDataType::Float32 ||
        boxes.info.elementCount != positions * 4 || scores.info.elementCount % positions) return false;
    const int classes = static_cast<int>(scores.info.elementCount / positions);
    detections.clear();
    std::size_t position = 0;
    for (int stride : {8, 16, 32}) {
        for (int gy = 0; gy < height / stride; ++gy) {
            for (int gx = 0; gx < width / stride; ++gx, ++position) {
                int target = -1;
                float best = scoreThreshold;
                for (int c = 0; c < classes; ++c) {
                    const float score = valueAt(scores, c, position, classes, positions);
                    if (score > best) { best = score; target = c; }
                }
                if (target < 0) continue;
                const float left = valueAt(boxes, 0, position, 4, positions);
                const float top = valueAt(boxes, 1, position, 4, positions);
                const float right = valueAt(boxes, 2, position, 4, positions);
                const float bottom = valueAt(boxes, 3, position, 4, positions);
                const float x1 = (gx + .5F - left) * stride, y1 = (gy + .5F - top) * stride;
                const float x2 = (gx + .5F + right) * stride, y2 = (gy + .5F + bottom) * stride;
                DetectionBox box;
                box.x = std::clamp((x1 + x2) * .5F / width, 0.0F, 1.0F);
                box.y = std::clamp((y1 + y2) * .5F / height, 0.0F, 1.0F);
                box.width = std::clamp((x2 - x1) / width, 0.0F, 1.0F);
                box.height = std::clamp((y2 - y1) / height, 0.0F, 1.0F);
                box.score = best;
                box.classId = target;
                if (box.width > 0.0F && box.height > 0.0F) detections.push_back(box);
            }
        }
    }
    nms(detections, .45F);
    return true;
}
} // namespace

int main(int argc, char** argv) {
    using namespace std::chrono_literals;
    if (argc < 2 || argc > 4) {
        std::fprintf(stderr, "usage: %s model.cvimodel [score_threshold=0.5] [webrtc_port=8080]\n", argv[0]);
        return 2;
    }
    const float threshold = argc >= 3 ? std::strtof(argv[2], nullptr) : .5F;
    const int port = argc >= 4 ? std::atoi(argv[3]) : 8080;
    if (threshold <= 0.0F || threshold >= 1.0F || port <= 0 || port > 65535) return 2;
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);

    recamera::Model model;
    if (!model.load(argv[1])) {
        std::fprintf(stderr, "model: %s\n", model.lastError().message.c_str());
        return 3;
    }
    if (model.inputs().size() != 1 || model.outputs().size() != 2 ||
        model.inputs()[0].shape.size() != 4 || model.inputs()[0].dataType != recamera::TensorDataType::Int8) {
        std::fprintf(stderr, "this example requires one INT8 input and two YOLOv8 FP32 outputs\n");
        return 3;
    }
    const auto& shape = model.inputs()[0].shape;
    const bool nhwc = shape[3] == 3;
    const int inputHeight = nhwc ? shape[1] : shape[2];
    const int inputWidth = nhwc ? shape[2] : shape[3];
    const std::size_t positions = featureCount(inputWidth, inputHeight);
    int boxOutput = -1, scoreOutput = -1;
    for (int i = 0; i < 2; ++i) {
        if (model.outputs()[i].elementCount == positions * 4) boxOutput = i;
        else if (positions && model.outputs()[i].elementCount % positions == 0) scoreOutput = i;
    }
    if (inputWidth <= 0 || inputHeight <= 0 || boxOutput < 0 || scoreOutput < 0) {
        std::fprintf(stderr, "unsupported YOLOv8 tensor layout\n");
        return 3;
    }

    constexpr int streamWidth = 1920, streamHeight = 1080, streamFps = 30, inferFps = 5;
    recamera::CameraConfig config;
    config.width = streamWidth; config.height = streamHeight; config.fps = streamFps; config.queueDepth = 3;
    config.rawWidth = inputWidth; config.rawHeight = inputHeight; config.rawFps = inferFps;
    config.rawFormat = recamera::PixelFormat::RGB888;
    recamera::Camera camera;
    recamera::WebRtcStream webrtc;
    recamera::Overlay overlay;
    if (!camera.open(config)) { std::fprintf(stderr, "camera: %s\n", camera.lastError().message.c_str()); return 4; }
    if (!webrtc.start({static_cast<std::uint16_t>(port), "/live0", "0.0.0.0", "", streamFps, 2})) {
        std::fprintf(stderr, "webrtc: %s\n", webrtc.lastError().message.c_str()); return 5;
    }
    if (!camera.start()) { std::fprintf(stderr, "camera: %s\n", camera.lastError().message.c_str()); return 4; }
    std::this_thread::sleep_for(300ms);
    if (!overlay.start({streamWidth, streamHeight, 2, 12, 7, 4}))
        std::fprintf(stderr, "overlay disabled: %s\n", overlay.lastError().message.c_str());

    std::thread streaming([&] {
        while (running) {
            auto encoded = camera.readEncoded(500ms);
            if (encoded && !webrtc.write(*encoded))
                std::fprintf(stderr, "webrtc write: %s\n", webrtc.lastError().message.c_str());
        }
    });
    std::vector<std::uint8_t> input(model.inputs()[0].byteSize);
    std::vector<DetectionBox> detections;
    std::uint64_t frames = 0;
    std::fprintf(stderr, "ready: http://192.168.42.1:%d/live0 model=%dx%d stream=%dx%d\n",
                 port, inputWidth, inputHeight, streamWidth, streamHeight);
    while (running) {
        auto frame = camera.read(1s);
        if (!frame) continue;
        if (!prepareRgb(*frame, inputWidth, inputHeight, input)) {
            std::fprintf(stderr, "unexpected camera raw frame contract\n"); running = false; break;
        }
        frame.reset();
        if (!model.setInput(0, input) || !model.run()) {
            std::fprintf(stderr, "inference: %s\n", model.lastError().message.c_str()); running = false; break;
        }
        const auto boxes = model.output(boxOutput), scores = model.output(scoreOutput);
        if (!decodeYolo(boxes, scores, inputWidth, inputHeight, threshold, detections)) {
            std::fprintf(stderr, "unsupported YOLOv8 output contract\n"); running = false; break;
        }
        if (overlay.running() && !overlay.update(detections))
            std::fprintf(stderr, "overlay: %s\n", overlay.lastError().message.c_str());
        if (++frames % 10 == 0)
            std::fprintf(stderr, "inference frames=%llu detections=%zu\n",
                         static_cast<unsigned long long>(frames), detections.size());
    }
    running = false;
    streaming.join();
    overlay.stop();
    camera.stop();
    webrtc.stop();
    return 0;
}
