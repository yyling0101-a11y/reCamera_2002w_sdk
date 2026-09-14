#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>

#include <recamera/camera.hpp>
#include <recamera/onvif.hpp>
#include <recamera/rtsp.hpp>

namespace
{
std::atomic<bool> running{true};
void stop(int)
{
    running = false;
}

bool containsNal(const recamera::EncodedFrame &frame, std::uint8_t wanted)
{
    for (const auto &block : frame.blocks())
    {
        for (std::size_t i = 0; i + 4 < block.size(); ++i)
        {
            std::size_t header = 0;
            if (block[i] == 0 && block[i + 1] == 0 && block[i + 2] == 1)
                header = i + 3;
            else if (i + 4 < block.size() && block[i] == 0 && block[i + 1] == 0 && block[i + 2] == 0 &&
                     block[i + 3] == 1)
                header = i + 4;
            if (header != 0 && (block[header] & 0x1fU) == wanted)
                return true;
        }
    }
    return false;
}
} // namespace

int main()
{
    using namespace std::chrono_literals;
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);

    constexpr const char *username = "admin";
    constexpr const char *password = "recamera.1";

    recamera::Camera camera;
    recamera::RtspStream rtsp;
    recamera::OnvifService onvif;

    recamera::RtspConfig rtspConfig;
    rtspConfig.port = 8554;
    rtspConfig.path = "/onvif";
    rtspConfig.gop = 30;
    rtspConfig.authenticationEnabled = true;
    rtspConfig.username = username;
    rtspConfig.password = password;
    rtspConfig.realm = "reCamera";

    recamera::OnvifConfig onvifConfig;
    // Empty selects the device's active non-loopback address (eth0 preferred).
    onvifConfig.address.clear();
    onvifConfig.port = 8000;
    onvifConfig.username = username;
    onvifConfig.password = password;
    onvifConfig.rtspPort = rtspConfig.port;
    onvifConfig.rtspPath = rtspConfig.path;
    onvifConfig.gop = rtspConfig.gop;
    onvifConfig.requestKeyframe = [&camera] { return camera.requestIdr(); };

    if (!camera.open({1920, 1080, 30, 2}))
        return 1;
    if (!camera.setGop(rtspConfig.gop))
    {
        std::fprintf(stderr, "Camera GOP configuration failed: %s\n", camera.lastError().message.c_str());
        return 1;
    }
    // Start hardware capture/encoding before publishing either service. This
    // guarantees that an NVR connecting immediately can request a valid IDR.
    if (!camera.start())
    {
        std::fprintf(stderr, "Camera start failed: %s\n", camera.lastError().message.c_str());
        return 1;
    }
    if (!rtsp.start(rtspConfig))
    {
        std::fprintf(stderr, "RTSP start failed: %s\n", rtsp.lastError().message.c_str());
        return 1;
    }

    // Seed the RTSP session with codec parameters before ONVIF discovery lets
    // an NVR connect. Otherwise an eager NVR can cache SDP without SPS/PPS.
    bool sawSps = false;
    bool sawPps = false;
    for (int i = 0; i < 30 && !(sawSps && sawPps); ++i)
    {
        auto encoded = camera.readEncoded(500ms);
        if (!encoded)
            continue;
        sawSps = sawSps || containsNal(*encoded, 7);
        sawPps = sawPps || containsNal(*encoded, 8);
        if (!rtsp.write(*encoded))
        {
            std::fprintf(stderr, "RTSP priming failed: %s\n", rtsp.lastError().message.c_str());
            return 1;
        }
    }
    if (!sawSps || !sawPps)
    {
        std::fprintf(stderr, "Camera did not produce H.264 SPS/PPS while priming RTSP\n");
        return 1;
    }
    if (!onvif.start(onvifConfig))
    {
        std::fprintf(stderr, "ONVIF start failed: %s\n", onvif.lastError().message.c_str());
        return 1;
    }
    std::fprintf(stderr, "ONVIF: http://%s:%u/onvif/device_service\n", onvifConfig.address.c_str(), onvifConfig.port);
    std::fprintf(stderr, "RTSP: rtsp://%s:%u%s (Digest user=%s)\n", onvifConfig.address.c_str(), rtspConfig.port,
                 rtspConfig.path.c_str(), username);

    while (running)
    {
        auto encoded = camera.readEncoded(1s);
        if (encoded && !rtsp.write(*encoded))
        {
            std::fprintf(stderr, "RTSP write failed: %s\n", rtsp.lastError().message.c_str());
        }
    }
    return 0;
}
