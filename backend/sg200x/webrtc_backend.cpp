#include "backend/sg200x/webrtc_backend.hpp"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace recamera::sg200x {
namespace {

constexpr const char* kPlayer = R"HTML(<!doctype html><meta charset="utf-8">
<title>reCamera WebRTC</title><style>html,body{margin:0;background:#111;color:#eee;font:16px sans-serif}video{width:100vw;height:100vh;object-fit:contain}</style>
<video id="v" autoplay muted playsinline controls></video><script>
(async()=>{const pc=new RTCPeerConnection();pc.ontrack=e=>v.srcObject=e.streams[0];
pc.addTransceiver('video',{direction:'recvonly'});await pc.setLocalDescription(await pc.createOffer());
await new Promise(r=>pc.iceGatheringState==='complete'?r():pc.onicegatheringstatechange=()=>pc.iceGatheringState==='complete'&&r());
const p=location.pathname.replace(/\/$/,'')+'/offer';const a=await fetch(p,{method:'POST',headers:{'Content-Type':'application/sdp'},body:pc.localDescription.sdp});
if(!a.ok)throw new Error(await a.text());await pc.setRemoteDescription({type:'answer',sdp:await a.text()});})().catch(e=>document.body.innerText=e);
</script>)HTML";

bool sendAll(int fd, const std::string& value) {
    std::size_t sent = 0;
    while (sent < value.size()) {
        const auto n = ::send(fd, value.data() + sent, value.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

void reply(int fd, int code, const char* type, const std::string& body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << code << (code == 200 ? " OK" : " Error") << "\r\n"
        << "Content-Type: " << type << "\r\nContent-Length: " << body.size()
        << "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n" << body;
    sendAll(fd, out.str());
}

}  // namespace

struct WebRtcBackend::Client {
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtp;
    std::atomic<bool> connected{false};
    std::atomic<bool> closed{false};
};

WebRtcBackend::~WebRtcBackend() { stop(); }

bool WebRtcBackend::start(const WebRtcConfig& config, Error& error) {
    if (running_) {
        error = {ErrorCode::AlreadyRunning, "WebRTC is already running"};
        return false;
    }
    if (config.port == 0 || config.path.size() < 2 || config.path.front() != '/' ||
        config.fps <= 0 || config.maxClients == 0) {
        error = {ErrorCode::InvalidArgument, "invalid WebRTC port, path, fps, or maxClients"};
        return false;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = {ErrorCode::BackendError, "WebRTC signaling socket failed"};
        return false;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.bindAddress.c_str(), &address.sin_addr) != 1 ||
        ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(fd, 4) != 0) {
        ::close(fd);
        error = {ErrorCode::BackendError, "WebRTC signaling bind/listen failed"};
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        status_ = {};
        status_.running = true;
        listenFd_ = fd;
    }
    running_ = true;
    serverThread_ = std::thread(&WebRtcBackend::serve, this);
    std::fprintf(stderr, "[recamera][INFO] WebRTC player at http://0.0.0.0:%u%s\n",
                 config.port, config.path.c_str());
    error = {};
    return true;
}

void WebRtcBackend::stop() noexcept {
    if (!running_.exchange(false)) return;
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fd = listenFd_;
        listenFd_ = -1;
        status_.running = false;
    }
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    if (serverThread_.joinable()) serverThread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& client : clients_) {
        client->peer->resetCallbacks();
        client->peer->close();
    }
    clients_.clear();
    status_.clients = 0;
}

bool WebRtcBackend::running() const noexcept { return running_; }
WebRtcStatus WebRtcBackend::status() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void WebRtcBackend::serve() noexcept {
    while (running_) {
        const int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) continue;
        handleConnection(fd);
        ::close(fd);
    }
}

void WebRtcBackend::handleConnection(int fd) noexcept {
    std::string request;
    char buffer[4096];
    std::size_t contentLength = 0;
    while (request.size() < 128 * 1024) {
        const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) return;
        request.append(buffer, static_cast<std::size_t>(n));
        const auto split = request.find("\r\n\r\n");
        if (split == std::string::npos) continue;
        const auto marker = request.find("Content-Length:");
        if (marker != std::string::npos) contentLength = std::strtoul(request.c_str() + marker + 15, nullptr, 10);
        if (request.size() >= split + 4 + contentLength) break;
    }
    const auto firstEnd = request.find("\r\n");
    if (firstEnd == std::string::npos) return;
    const std::string first = request.substr(0, firstEnd);
    if (first.rfind("GET " + config_.path + " ", 0) == 0 || first.rfind("GET / ", 0) == 0) {
        reply(fd, 200, "text/html; charset=utf-8", kPlayer);
        return;
    }
    if (first.rfind("POST " + config_.path + "/offer ", 0) != 0) {
        reply(fd, 404, "text/plain", "not found");
        return;
    }
    const auto split = request.find("\r\n\r\n");
    Error error;
    const auto answer = createAnswer(request.substr(split + 4, contentLength), error);
    if (error) reply(fd, 500, "text/plain", error.message);
    else reply(fd, 200, "application/sdp", answer);
}

std::string WebRtcBackend::createAnswer(const std::string& offer, Error& error) {
    std::unique_lock<std::mutex> lock(mutex_);
    removeClosedClients();
    if (clients_.size() >= config_.maxClients) {
        error = {ErrorCode::BackendError, "WebRTC client limit reached"};
        return {};
    }
    auto client = std::make_shared<Client>();
    rtc::Configuration configuration;
    if (!config_.stunServer.empty()) configuration.iceServers.emplace_back(config_.stunServer);
    // Signaling is request/response HTTP, so negotiation must be driven exactly
    // once below. Otherwise setRemoteDescription() auto-answers and the explicit
    // answer attempt runs again in Stable state.
    configuration.disableAutoNegotiation = true;
    client->peer = std::make_shared<rtc::PeerConnection>(configuration);

    // The answer must preserve the offer's m-line order and mids exactly. Use
    // the browser-created video section instead of inventing a local "video"
    // mid before applying the remote description.
    rtc::Description remote(offer, "offer");
    std::string videoMid;
    int h264Payload = -1;
    std::optional<std::string> h264Profile;
    for (int i = 0; i < remote.mediaCount() && h264Payload < 0; ++i) {
        const auto entry = remote.media(i);
        if (!std::holds_alternative<rtc::Description::Media*>(entry)) continue;
        const auto* media = std::get<rtc::Description::Media*>(entry);
        if (media->type() != "video") continue;
        for (const int payload : media->payloadTypes()) {
            const auto* mapping = media->rtpMap(payload);
            if (mapping != nullptr && mapping->format == "H264") {
                videoMid = media->mid();
                h264Payload = payload;
                if (!mapping->fmtps.empty()) h264Profile = mapping->fmtps.front();
                break;
            }
        }
    }
    if (h264Payload < 0) {
        error = {ErrorCode::Unsupported, "WebRTC offer does not contain H.264 video"};
        return {};
    }
    const rtc::SSRC ssrc = static_cast<rtc::SSRC>(0x52430000u + status_.totalConnections + 1);
    rtc::Description::Video media(videoMid, rtc::Description::Direction::SendOnly);
    media.addH264Codec(h264Payload, h264Profile);
    media.addSSRC(ssrc, "recamera", "recamera-stream", "video");
    client->track = client->peer->addTrack(media);
    client->rtp = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "recamera",
                                                                static_cast<std::uint8_t>(h264Payload),
                                                                rtc::H264RtpPacketizer::ClockRate);
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence,
                                                               client->rtp);
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(client->rtp));
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    client->track->setMediaHandler(packetizer);
    auto weak = std::weak_ptr<Client>(client);
    client->peer->onStateChange([this, weak](rtc::PeerConnection::State state) {
        auto value = weak.lock();
        if (!value) return;
        std::lock_guard<std::mutex> guard(mutex_);
        if (state == rtc::PeerConnection::State::Connected && !value->connected.exchange(true)) {
            ++status_.clients;
            ++status_.totalConnections;
        } else if ((state == rtc::PeerConnection::State::Disconnected ||
                    state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed) &&
                   !value->closed.exchange(true)) {
            if (value->connected && status_.clients > 0) --status_.clients;
            ++status_.totalDisconnections;
        }
    });
    std::condition_variable gathered;
    bool complete = false;
    client->peer->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            std::lock_guard<std::mutex> guard(mutex_);
            complete = true;
            gathered.notify_one();
        }
    });
    clients_.push_back(client);
    lock.unlock();
    try {
        client->peer->setRemoteDescription(std::move(remote));
        client->peer->setLocalDescription(rtc::Description::Type::Answer);
    } catch (const std::exception& ex) {
        client->peer->onGatheringStateChange({});
        client->closed = true;
        error = {ErrorCode::BackendError, ex.what()};
        return {};
    }
    lock.lock();
    if (!gathered.wait_for(lock, std::chrono::seconds(10), [&] { return complete; })) {
        client->peer->onGatheringStateChange({});
        error = {ErrorCode::Timeout, "WebRTC ICE gathering timed out"};
        client->closed = true;
        return {};
    }
    client->peer->onGatheringStateChange({});
    auto description = client->peer->localDescription();
    if (!description) {
        error = {ErrorCode::BackendError, "WebRTC answer is unavailable"};
        return {};
    }
    error = {};
    return std::string(*description);
}

void WebRtcBackend::removeClosedClients() {
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                  [](const auto& value) { return value->closed.load(); }), clients_.end());
}

bool WebRtcBackend::write(const EncodedFrame& frame, Error& error) {
    if (frame.format() != PixelFormat::H264 || frame.blocks().empty()) {
        error = {ErrorCode::InvalidArgument, "WebRTC accepts H.264 encoded frames"};
        return false;
    }
    rtc::binary accessUnit;
    accessUnit.reserve(frame.size());
    for (const auto& block : frame.blocks()) {
        const auto* begin = reinterpret_cast<const std::byte*>(block.data());
        accessUnit.insert(accessUnit.end(), begin, begin + block.size());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    removeClosedClients();
    bool failed = false;
    int recipients = 0;
    for (const auto& client : clients_) {
        if (!client->connected || !client->track->isOpen()) continue;
        client->rtp->timestamp += rtc::H264RtpPacketizer::ClockRate / static_cast<std::uint32_t>(config_.fps);
        if (!client->track->send(accessUnit)) failed = true;
        else ++recipients;
    }
    if (failed) ++status_.writeErrors;
    if (recipients > 0) {
        ++status_.framesSent;
        status_.bytesSent += frame.size() * static_cast<std::uint64_t>(recipients);
    }
    error = failed ? Error{ErrorCode::BackendError, "WebRTC send queue is full"} : Error{};
    return !failed;
}

}  // namespace recamera::sg200x
