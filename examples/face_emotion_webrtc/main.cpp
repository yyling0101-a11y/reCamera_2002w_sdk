#include <recamera/ai.hpp>
#include <recamera/camera.hpp>
#include <recamera/osd.hpp>
#include <recamera/webrtc.hpp>

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace
{
using recamera::DetectionBox;
using namespace std::chrono_literals;

constexpr int kInputSize = 640;
constexpr int kAttributeSize = 224;
constexpr int kEmotionSize = 64;
constexpr std::array<const char *, 7> kEmotionNames = {"angry", "disgust", "fear", "happy",
                                                       "sad", "surprise", "neutral"};
constexpr std::array<const char *, 9> kAgeNames = {"0-2", "3-9", "10-19", "20-29", "30-39",
                                                   "40-49", "50-59", "60-69", "70+"};
constexpr std::array<const char *, 7> kRaceNames = {"White", "Black", "Latino_Hispanic", "East_Asian",
                                                    "Southeast_Asian", "Indian", "Middle_Eastern"};

struct FaceAnalysis
{
    DetectionBox box;
    int emotion = -1, age = -1, gender = -1, race = -1;
    float emotionConfidence = 0, ageConfidence = 0, genderConfidence = 0, raceConfidence = 0;
};
std::atomic<bool> running{true};
void stop(int)
{
    running = false;
}

std::uint16_t bf16(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<std::uint16_t>(bits >> 16);
}

void storeValue(std::vector<std::uint8_t> &buffer, std::size_t index, recamera::TensorDataType type, float value)
{
    if (type == recamera::TensorDataType::UInt8)
        buffer[index] = static_cast<std::uint8_t>(std::clamp(value, 0.0F, 255.0F));
    else if (type == recamera::TensorDataType::Float32)
        std::memcpy(buffer.data() + index * sizeof(float), &value, sizeof(value));
    else
    {
        const auto valueBf16 = bf16(value);
        std::memcpy(buffer.data() + index * sizeof(valueBf16), &valueBf16, sizeof(valueBf16));
    }
}

float tensorFloat(const recamera::Tensor &tensor, std::size_t index)
{
    if (tensor.info.dataType == recamera::TensorDataType::Float32)
    {
        float value;
        std::memcpy(&value, tensor.data.data() + index * sizeof(value), sizeof(value));
        return value;
    }
    if (tensor.info.dataType == recamera::TensorDataType::BFloat16)
    {
        std::uint16_t upper;
        std::memcpy(&upper, tensor.data.data() + index * sizeof(upper), sizeof(upper));
        std::uint32_t bits = static_cast<std::uint32_t>(upper) << 16;
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    return 0.0F;
}

bool prepareDetector(const recamera::VideoFrame &frame, const recamera::TensorInfo &info,
                     std::vector<std::uint8_t> &input)
{
    if ((frame.format() != recamera::PixelFormat::RGB888 && frame.format() != recamera::PixelFormat::NV21) ||
        frame.width() != kInputSize || frame.height() != kInputSize || frame.planeCount() == 0)
        return false;
    const auto p = frame.plane(0);
    const std::size_t stride =
        std::max<std::size_t>(p.stride, frame.format() == recamera::PixelFormat::RGB888 ? kInputSize * 3 : kInputSize);
    const auto uv = frame.format() == recamera::PixelFormat::NV21 && frame.planeCount() > 1
                        ? frame.plane(1)
                        : recamera::VideoFrame::Plane{};
    if (!p.data || p.size < stride * kInputSize ||
        info.dataType != recamera::TensorDataType::UInt8)
        return false;
    constexpr std::size_t pixels = kInputSize * kInputSize;
    for (int y = 0; y < kInputSize; ++y)
    {
        const auto *row = p.data + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < kInputSize; ++x)
        {
            const std::size_t i = static_cast<std::size_t>(y) * kInputSize + x;
            float r, g, b;
            if (frame.format() == recamera::PixelFormat::RGB888)
            {
                r = row[x * 3];
                g = row[x * 3 + 1];
                b = row[x * 3 + 2];
            }
            else
            {
                const std::size_t uvStride = std::max<std::size_t>(uv.stride, kInputSize);
                if (!uv.data || uv.size < uvStride * (kInputSize / 2))
                    return false;
                const int yy = row[x];
                const auto *pair = uv.data + static_cast<std::size_t>(y / 2) * uvStride + (x & ~1);
                const int v = pair[0] - 128, u = pair[1] - 128;
                r = std::clamp(yy + 1.402F * v, 0.0F, 255.0F);
                g = std::clamp(yy - .344136F * u - .714136F * v, 0.0F, 255.0F);
                b = std::clamp(yy + 1.772F * u, 0.0F, 255.0F);
            }
            input[i * 3] = static_cast<std::uint8_t>(r);
            input[i * 3 + 1] = static_cast<std::uint8_t>(g);
            input[i * 3 + 2] = static_cast<std::uint8_t>(b);
        }
    }
    return true;
}

float iou(const DetectionBox &a, const DetectionBox &b)
{
    const float ax1 = a.x - a.width * .5F, ay1 = a.y - a.height * .5F;
    const float ax2 = a.x + a.width * .5F, ay2 = a.y + a.height * .5F;
    const float bx1 = b.x - b.width * .5F, by1 = b.y - b.height * .5F;
    const float bx2 = b.x + b.width * .5F, by2 = b.y + b.height * .5F;
    const float overlap = std::max(0.0F, std::min(ax2, bx2) - std::max(ax1, bx1)) *
                          std::max(0.0F, std::min(ay2, by2) - std::max(ay1, by1));
    const float area = a.width * a.height + b.width * b.height - overlap;
    return area > 0 ? overlap / area : 0;
}

std::vector<DetectionBox> decodeYoloFace(recamera::Model &model, float threshold)
{
    std::vector<DetectionBox> boxes;
    if (model.outputs().size() != 1)
        return boxes;
    const auto output = model.output(0);
    if (output.info.elementCount != 5U * 8400U)
        return boxes;
    for (std::size_t i = 0; i < 8400; ++i)
    {
        float score = tensorFloat(output, 4U * 8400U + i);
        if (score < threshold)
            continue;
        const float cx = tensorFloat(output, i) / kInputSize;
        const float cy = tensorFloat(output, 8400U + i) / kInputSize;
        const float width = tensorFloat(output, 2U * 8400U + i) / kInputSize;
        const float height = tensorFloat(output, 3U * 8400U + i) / kInputSize;
        if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(width) || !std::isfinite(height) ||
            width < .04F || height < .04F || width > .9F || height > .9F)
            continue;
        boxes.push_back({std::clamp(cx, 0.0F, 1.0F), std::clamp(cy, 0.0F, 1.0F),
                         std::clamp(width, 0.0F, 1.0F), std::clamp(height, 0.0F, 1.0F), score, 0});
    }
    std::sort(boxes.begin(), boxes.end(), [](const auto &a, const auto &b) { return a.score > b.score; });
    std::vector<DetectionBox> kept;
    for (const auto &box : boxes)
    {
        bool suppress = false;
        for (const auto &selected : kept)
            if (iou(box, selected) > .3F)
            {
                suppress = true;
                break;
            }
        if (!suppress && kept.size() < 2)
            kept.push_back(box);
    }
    return kept;
}

bool cropFace(const std::vector<std::uint8_t> &rgb, const DetectionBox &box, int size, float cropScale,
              std::vector<std::uint8_t> &crop)
{
    crop.resize(static_cast<std::size_t>(size) * size * 3);
    const float side = std::max(box.width, box.height) * kInputSize * cropScale;
    if (side < 16)
        return false;
    const float left = box.x * kInputSize - side * .5F;
    const float top = box.y * kInputSize - side * .5F;
    for (int y = 0; y < size; ++y)
    {
        const int sy = std::clamp(static_cast<int>(top + (y + .5F) * side / size), 0, kInputSize - 1);
        for (int x = 0; x < size; ++x)
        {
            const int sx = std::clamp(static_cast<int>(left + (x + .5F) * side / size), 0, kInputSize - 1);
            const auto source = (static_cast<std::size_t>(sy) * kInputSize + sx) * 3;
            const auto dest = (static_cast<std::size_t>(y) * size + x) * 3;
            std::memcpy(crop.data() + dest, rgb.data() + source, 3);
        }
    }
    return true;
}

std::pair<int, float> classify(recamera::Model &model, std::size_t outputIndex, std::size_t classes)
{
    if (outputIndex >= model.outputs().size())
        return {-1, 0};
    const auto output = model.output(outputIndex);
    if (output.info.elementCount != classes)
        return {-1, 0};
    std::vector<float> logits(classes);
    float maximum = -1e30F;
    for (std::size_t i = 0; i < classes; ++i)
        maximum = std::max(maximum, logits[i] = tensorFloat(output, i));
    float sum = 0;
    for (float &v : logits)
        sum += (v = std::exp(v - maximum));
    const int best = static_cast<int>(std::max_element(logits.begin(), logits.end()) - logits.begin());
    return {best, sum > 0 ? logits[best] / sum : 0};
}

bool analyzeFace(recamera::Model &attributes, recamera::Model &emotion, const std::vector<std::uint8_t> &rgb,
                 FaceAnalysis &face)
{
    std::vector<std::uint8_t> crop;
    if (!cropFace(rgb, face.box, kAttributeSize, 1.3F, crop) || !attributes.setInput(0, crop) || !attributes.run())
        return false;
    int raceOutput = -1, genderOutput = -1, ageOutput = -1;
    for (std::size_t i = 0; i < attributes.outputs().size(); ++i)
    {
        const auto count = attributes.outputs()[i].elementCount;
        if (count == 7) raceOutput = static_cast<int>(i);
        else if (count == 2) genderOutput = static_cast<int>(i);
        else if (count == 9) ageOutput = static_cast<int>(i);
    }
    auto race = classify(attributes, raceOutput, 7), gender = classify(attributes, genderOutput, 2);
    auto age = classify(attributes, ageOutput, 9);
    face.race = race.first; face.raceConfidence = race.second;
    face.gender = gender.first == 0 ? 1 : 0; face.genderConfidence = gender.second;
    face.age = age.first; face.ageConfidence = age.second;

    if (!cropFace(rgb, face.box, kEmotionSize, 1.3F, crop))
        return true;
    std::vector<std::uint8_t> gray(emotion.inputs()[0].byteSize);
    for (int y = 0; y < kEmotionSize; ++y)
        for (int x = 0; x < kEmotionSize; ++x)
        {
            const auto i = (static_cast<std::size_t>(y) * kEmotionSize + x);
            const auto *p = crop.data() + i * 3;
            storeValue(gray, i, emotion.inputs()[0].dataType,
                       (.299F * p[0] + .587F * p[1] + .114F * p[2]) / 255.0F);
        }
    if (emotion.setInput(0, gray) && emotion.run())
    {
        auto result = classify(emotion, 0, 7);
        face.emotion = result.first; face.emotionConfidence = result.second;
        face.box.classId = std::max(0, result.first);
    }
    return true;
}

std::string base64(const unsigned char *data, std::size_t length)
{
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < length; i += 3)
    {
        std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < length)
            v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        if (i + 2 < length)
            v |= data[i + 2];
        out.push_back(table[(v >> 18) & 63]);
        out.push_back(table[(v >> 12) & 63]);
        out.push_back(i + 1 < length ? table[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < length ? table[v & 63] : '=');
    }
    return out;
}

class WebSocketServer
{
  public:
    bool start(std::uint16_t port)
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);
        if (listener_ < 0 || bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
            listen(listener_, 8) < 0)
            return false;
        fcntl(listener_, F_SETFL, fcntl(listener_, F_GETFL) | O_NONBLOCK);
        thread_ = std::thread([this] { acceptLoop(); });
        return true;
    }
    void broadcast(const std::string &message)
    {
        std::vector<std::uint8_t> frame{0x81};
        if (message.size() < 126)
            frame.push_back(static_cast<std::uint8_t>(message.size()));
        else
        {
            frame.push_back(126);
            frame.push_back(message.size() >> 8);
            frame.push_back(message.size());
        }
        frame.insert(frame.end(), message.begin(), message.end());
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                      [&](int fd) {
                                          if (send(fd, frame.data(), frame.size(), MSG_NOSIGNAL) ==
                                              static_cast<ssize_t>(frame.size()))
                                              return false;
                                          close(fd);
                                          return true;
                                      }),
                       clients_.end());
    }
    ~WebSocketServer()
    {
        active_ = false;
        if (thread_.joinable())
            thread_.join();
        if (listener_ >= 0)
            close(listener_);
        for (int fd : clients_)
            close(fd);
    }

  private:
    void acceptLoop()
    {
        while (active_ && running)
        {
            pollfd pfd{listener_, POLLIN, 0};
            if (poll(&pfd, 1, 200) <= 0)
                continue;
            int fd = accept(listener_, nullptr, nullptr);
            if (fd < 0)
                continue;
            timeval timeout{2, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            std::array<char, 4096> buffer{};
            const ssize_t n = recv(fd, buffer.data(), buffer.size() - 1, 0);
            std::string request(buffer.data(), n > 0 ? static_cast<std::size_t>(n) : 0);
            const std::string marker = "Sec-WebSocket-Key:";
            const auto begin = request.find(marker);
            if (begin == std::string::npos)
            {
                close(fd);
                continue;
            }
            auto keyStart = request.find_first_not_of(" \t", begin + marker.size());
            auto keyEnd = request.find("\r\n", keyStart);
            std::string key = request.substr(keyStart, keyEnd - keyStart) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            unsigned char digest[SHA_DIGEST_LENGTH];
            SHA1(reinterpret_cast<const unsigned char *>(key.data()), key.size(), digest);
            const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                         "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                                         base64(digest, sizeof(digest)) + "\r\n\r\n";
            if (send(fd, response.data(), response.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(response.size()))
                close(fd);
            else
            {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                std::lock_guard<std::mutex> lock(mutex_);
                clients_.push_back(fd);
            }
        }
    }
    int listener_ = -1;
    std::atomic<bool> active_{true};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<int> clients_;
};

std::string json(const std::vector<FaceAnalysis> &faces, std::uint64_t sequence)
{
    std::string out = "{\"sequence\":" + std::to_string(sequence) + ",\"faces\":[";
    for (std::size_t i = 0; i < faces.size(); ++i)
    {
        if (i)
            out += ',';
        const auto &face = faces[i];
        char item[768];
        std::snprintf(item, sizeof(item),
                      "{\"id\":%zu,\"confidence\":%.4f,"
                      "\"emotion\":\"%s\",\"emotion_id\":%d,\"emotion_confidence\":%.4f,"
                      "\"age\":\"%s\",\"age_id\":%d,\"age_confidence\":%.4f,"
                      "\"gender\":\"%s\",\"gender_id\":%d,\"gender_confidence\":%.4f,"
                      "\"race\":\"%s\",\"race_id\":%d,\"race_confidence\":%.4f,"
                      "\"box\":{\"x\":%.4f,\"y\":%.4f,\"width\":%.4f,\"height\":%.4f}}",
                      i, face.box.score,
                      face.emotion >= 0 ? kEmotionNames[face.emotion] : "unknown", face.emotion,
                      face.emotionConfidence, face.age >= 0 ? kAgeNames[face.age] : "unknown", face.age,
                      face.ageConfidence, face.gender == 1 ? "male" : (face.gender == 0 ? "female" : "unknown"),
                      face.gender, face.genderConfidence, face.race >= 0 ? kRaceNames[face.race] : "unknown",
                      face.race, face.raceConfidence, face.box.x, face.box.y, face.box.width, face.box.height);
        out += item;
    }
    return out + "]}";
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 4 || argc > 7)
    {
        std::fprintf(
            stderr,
            "usage: %s face.cvimodel age_gender_race.cvimodel emotion.cvimodel "
            "[face_threshold=.70] [webrtc_port=8081] [websocket_port=8765]\n",
            argv[0]);
        return 2;
    }
    const float threshold = argc > 4 ? std::strtof(argv[4], nullptr) : .70F;
    const int webrtcPort = argc > 5 ? std::atoi(argv[5]) : 8081;
    const int websocketPort = argc > 6 ? std::atoi(argv[6]) : 8765;
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);

    recamera::Model detector, attributes, emotion;
    if (!detector.load(argv[1]) || !attributes.load(argv[2]) || !emotion.load(argv[3]))
    {
        std::fprintf(stderr, "model load failed: detector=%s attributes=%s emotion=%s\n",
                     detector.lastError().message.c_str(), attributes.lastError().message.c_str(),
                     emotion.lastError().message.c_str());
        return 3;
    }
    std::fprintf(stderr, "detector: input=%s bytes=%zu outputs=%zu; emotion: inputs=%zu output=%s count=%zu\n",
                 recamera::toString(detector.inputs()[0].dataType), detector.inputs()[0].byteSize,
                 detector.outputs().size(), emotion.inputs().size(),
                 emotion.outputs().empty() ? "none" : recamera::toString(emotion.outputs()[0].dataType),
                 emotion.outputs().empty() ? 0U : emotion.outputs()[0].elementCount);
    if (detector.inputs().size() != 1 || detector.outputs().size() != 1 ||
        detector.outputs()[0].elementCount != 5U * 8400U ||
        detector.inputs()[0].dataType != recamera::TensorDataType::UInt8 ||
        attributes.inputs().size() != 1 || attributes.inputs()[0].dataType != recamera::TensorDataType::UInt8 ||
        attributes.inputs()[0].elementCount != 224U * 224U * 3U || attributes.outputs().size() != 3 ||
        emotion.inputs().size() != 1 ||
        emotion.outputs().size() != 1 ||
        emotion.inputs()[0].dataType != recamera::TensorDataType::BFloat16 ||
        emotion.outputs()[0].elementCount != 7)
    {
        std::fprintf(stderr, "unexpected model tensor contract\n");
        return 3;
    }

    recamera::CameraConfig config;
    // The three-model pipeline uses about 50 MB of the SG2002W ION pool.
    // Keep the encoded channel at 360p so video VB plus the ARGB1555 overlay
    // allocation still fits after the models have been loaded.
    config.width = 640;
    config.height = 360;
    // Keep the source pipeline at the OV5647 sensor's native 30 fps. rawFps
    // independently throttles TPU inference; lowering this source rate causes
    // the firmware VI/VPSS pipeline to stop after its initial buffers drain.
    config.fps = 30;
    config.queueDepth = 4;
    config.rawWidth = kInputSize;
    config.rawHeight = kInputSize;
    config.rawFps = 3;
    // NV21 is the firmware-native VPSS path and avoids an RGB channel stall on
    // current production firmware. CPU conversion is limited to 640x640 at 3 fps.
    config.rawFormat = recamera::PixelFormat::NV21;
    recamera::Camera camera;
    recamera::WebRtcStream webrtc;
    recamera::Overlay overlay;
    WebSocketServer websocket;
    if (!camera.open(config) ||
        !webrtc.start({static_cast<std::uint16_t>(webrtcPort), "/live0", "0.0.0.0", "", 30, 2}) ||
        !websocket.start(static_cast<std::uint16_t>(websocketPort)) || !camera.start())
    {
        std::fprintf(stderr, "startup failed: camera=%s webrtc=%s\n", camera.lastError().message.c_str(),
                     webrtc.lastError().message.c_str());
        return 4;
    }
    std::this_thread::sleep_for(300ms);
    if (!overlay.start({640, 360, 2, 13, 7, 3}))
        std::fprintf(stderr, "overlay disabled: %s\n", overlay.lastError().message.c_str());
    std::thread streaming([&] {
        while (running)
        {
            auto h264 = camera.readEncoded(500ms);
            if (h264)
                webrtc.write(*h264);
        }
    });

    std::vector<std::uint8_t> detectorInput(detector.inputs()[0].byteSize);
    std::uint64_t sequence = 0;
    std::fprintf(stderr, "ready: WebRTC http://192.168.42.1:%d/live0 WebSocket ws://192.168.42.1:%d/\n", webrtcPort,
                 websocketPort);
    while (running)
    {
        auto frame = camera.read(1s);
        if (!frame)
            continue;
        if (!prepareDetector(*frame, detector.inputs()[0], detectorInput) || !detector.setInput(0, detectorInput) ||
            !detector.run())
        {
            std::fprintf(stderr, "detector failed: %s\n", detector.lastError().message.c_str());
            break;
        }
        const auto detected = decodeYoloFace(detector, threshold);
        std::vector<FaceAnalysis> faces;
        faces.reserve(detected.size());
        for (const auto &box : detected)
        {
            FaceAnalysis face;
            face.box = box;
            analyzeFace(attributes, emotion, detectorInput, face);
            faces.push_back(face);
        }
        frame.reset();
        if (overlay.running())
        {
            std::vector<DetectionBox> overlayBoxes;
            for (const auto &face : faces) overlayBoxes.push_back(face.box);
            overlay.update(overlayBoxes);
        }
        websocket.broadcast(json(faces, ++sequence));
    }
    running = false;
    streaming.join();
    overlay.stop();
    camera.stop();
    webrtc.stop();
    return 0;
}
