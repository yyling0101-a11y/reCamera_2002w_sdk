#include <recamera/onvif.hpp>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <ifaddrs.h>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace recamera {
namespace {

constexpr const char* kDiscoveryGroup = "239.255.255.250";
constexpr std::uint16_t kDiscoveryPort = 3702;

std::string xmlEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '\"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string localAddress() {
    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) return {};
    std::string fallback;
    for (ifaddrs* it = addresses; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET ||
            (it->ifa_flags & IFF_LOOPBACK) != 0) continue;
        char text[INET_ADDRSTRLEN]{};
        const auto* address = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) == nullptr) continue;
        if (fallback.empty()) fallback = text;
        if (std::strcmp(it->ifa_name, "eth0") == 0 || std::strcmp(it->ifa_name, "wlan0") == 0) {
            fallback = text;
            break;
        }
    }
    freeifaddrs(addresses);
    return fallback;
}

bool extractElement(const std::string& xml, const std::string& localName,
                    std::string& value, std::string* opening = nullptr) {
    std::size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        if (pos + 1 >= xml.size() || xml[pos + 1] == '/' || xml[pos + 1] == '?' || xml[pos + 1] == '!') {
            ++pos;
            continue;
        }
        const auto tagEnd = xml.find('>', pos + 1);
        const auto nameEnd = xml.find_first_of(" \t\r\n/>", pos + 1);
        if (tagEnd == std::string::npos || nameEnd == std::string::npos || nameEnd > tagEnd) return false;
        const auto qualified = xml.substr(pos + 1, nameEnd - pos - 1);
        const auto colon = qualified.rfind(':');
        const auto local = colon == std::string::npos ? qualified : qualified.substr(colon + 1);
        if (local != localName) {
            pos = tagEnd + 1;
            continue;
        }
        const auto closeTag = "</" + qualified + ">";
        const auto close = xml.find(closeTag, tagEnd + 1);
        if (close == std::string::npos) return false;
        if (opening) *opening = xml.substr(pos, tagEnd - pos + 1);
        value = xml.substr(tagEnd + 1, close - tagEnd - 1);
        const auto begin = value.find_first_not_of(" \t\r\n");
        const auto end = value.find_last_not_of(" \t\r\n");
        value = begin == std::string::npos ? std::string() : value.substr(begin, end - begin + 1);
        return true;
    }
    return false;
}

bool constantTimeEqual(const std::string& a, const std::string& b) {
    std::size_t n = a.size() > b.size() ? a.size() : b.size();
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    for (std::size_t i = 0; i < n; ++i)
        diff |= static_cast<unsigned>(i < a.size() ? static_cast<unsigned char>(a[i]) : 0) ^
                static_cast<unsigned>(i < b.size() ? static_cast<unsigned char>(b[i]) : 0);
    return diff == 0;
}

bool decodeBase64(const std::string& text, std::vector<unsigned char>& bytes) {
    std::string compact;
    for (char c : text) if (c != ' ' && c != '\t' && c != '\r' && c != '\n') compact += c;
    if (compact.empty() || compact.size() % 4 != 0) return false;
    bytes.resize((compact.size() / 4) * 3);
    const int decoded = EVP_DecodeBlock(bytes.data(),
        reinterpret_cast<const unsigned char*>(compact.data()), static_cast<int>(compact.size()));
    if (decoded < 0) return false;
    std::size_t size = static_cast<std::size_t>(decoded);
    if (!compact.empty() && compact.back() == '=') --size;
    if (compact.size() > 1 && compact[compact.size() - 2] == '=') --size;
    bytes.resize(size);
    return true;
}

std::string encodeBase64(const unsigned char* data, std::size_t size) {
    std::string out(4 * ((size + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data,
                                        static_cast<int>(size));
    out.resize(static_cast<std::size_t>(written));
    return out;
}

bool verifyUsernameToken(const std::string& xml, const std::string& expectedUser,
                         const std::string& expectedPassword) {
    std::string user, password, passwordTag, nonceText, created;
    if (!extractElement(xml, "Username", user) || !extractElement(xml, "Password", password, &passwordTag))
        return false;
    if (!constantTimeEqual(user, expectedUser)) return false;
    if (passwordTag.find("PasswordText") != std::string::npos)
        return constantTimeEqual(password, expectedPassword);
    if (passwordTag.find("PasswordDigest") == std::string::npos ||
        !extractElement(xml, "Nonce", nonceText) || !extractElement(xml, "Created", created)) return false;
    std::vector<unsigned char> nonce;
    if (!decodeBase64(nonceText, nonce)) return false;
    std::vector<unsigned char> material;
    material.reserve(nonce.size() + created.size() + expectedPassword.size());
    material.insert(material.end(), nonce.begin(), nonce.end());
    material.insert(material.end(), created.begin(), created.end());
    material.insert(material.end(), expectedPassword.begin(), expectedPassword.end());
    unsigned char digest[SHA_DIGEST_LENGTH]{};
    SHA1(material.data(), material.size(), digest);
    return constantTimeEqual(password, encodeBase64(digest, sizeof(digest)));
}

std::string envelope(const std::string& body) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
           "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
           "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
           "xmlns:tt=\"http://www.onvif.org/ver10/schema\"><s:Body>" + body +
           "</s:Body></s:Envelope>";
}

std::string authFault() {
    return envelope("<s:Fault><s:Code><s:Value>s:Sender</s:Value><s:Subcode>"
                    "<s:Value xmlns:ter=\"http://www.onvif.org/ver10/error\">ter:NotAuthorized</s:Value>"
                    "</s:Subcode></s:Code><s:Reason><s:Text xml:lang=\"en\">Sender not authorized"
                    "</s:Text></s:Reason></s:Fault>");
}

bool sendAll(int fd, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto n = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

std::size_t contentLength(const std::string& request) {
    auto pos = request.find("Content-Length:");
    if (pos == std::string::npos) pos = request.find("content-length:");
    if (pos == std::string::npos) return 0;
    pos = request.find(':', pos);
    return pos == std::string::npos ? 0 : std::strtoul(request.c_str() + pos + 1, nullptr, 10);
}

std::string requestPath(const std::string& request) {
    const auto first = request.find(' ');
    const auto second = first == std::string::npos ? std::string::npos : request.find(' ', first + 1);
    return second == std::string::npos ? "/" : request.substr(first + 1, second - first - 1);
}

std::string timeResponse() {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char date[256]{};
    std::snprintf(date, sizeof(date),
        "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:DateTimeType>NTP</tt:DateTimeType>"
        "<tt:DaylightSavings>false</tt:DaylightSavings><tt:TimeZone><tt:TZ>UTC</tt:TZ></tt:TimeZone>"
        "<tt:UTCDateTime><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second>"
        "</tt:Time><tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
        "</tt:UTCDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>",
        utc.tm_hour, utc.tm_min, utc.tm_sec, utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    return envelope(date);
}

}  // namespace

class OnvifService::Impl {
public:
    ~Impl() { stop(); }

    bool start(OnvifConfig value, Error& nextError) {
        if (running) {
            nextError = {ErrorCode::AlreadyRunning, "ONVIF is already running"};
            setError(nextError);
            return false;
        }
        if (value.address.empty()) value.address = localAddress();
        in_addr parsed{};
        if (value.address.empty() || inet_pton(AF_INET, value.address.c_str(), &parsed) != 1 ||
            value.port == 0 || value.rtspPort == 0 || value.rtspPath.size() < 2 ||
            value.rtspPath.front() != '/' || value.username.empty() || value.password.empty() ||
            value.width <= 0 || value.height <= 0 || value.fps <= 0 || value.gop <= 0) {
            nextError = {ErrorCode::InvalidArgument, "invalid ONVIF address, stream, or credentials"};
            setError(nextError);
            return false;
        }
        config = std::move(value);
        httpFd = socket(AF_INET, SOCK_STREAM, 0);
        discoveryFd = socket(AF_INET, SOCK_DGRAM, 0);
        if (httpFd < 0 || discoveryFd < 0 || !bindHttp() || !bindDiscovery()) {
            closeSockets();
            nextError = {ErrorCode::BackendError, "ONVIF HTTP or WS-Discovery bind failed"};
            setError(nextError);
            return false;
        }
        running = true;
        {
            std::lock_guard<std::mutex> lock(mutex);
            current = {};
            current.running = true;
        }
        httpThread = std::thread(&Impl::httpLoop, this);
        discoveryThread = std::thread(&Impl::discoveryLoop, this);
        nextError = {};
        setError(nextError);
        std::fprintf(stderr, "[recamera][INFO] ONVIF at http://%s:%u/onvif/device_service\n",
                     config.address.c_str(), config.port);
        return true;
    }

    void stop() noexcept {
        if (!running.exchange(false)) return;
        if (httpFd >= 0) shutdown(httpFd, SHUT_RDWR);
        if (discoveryFd >= 0) shutdown(discoveryFd, SHUT_RDWR);
        if (httpThread.joinable()) httpThread.join();
        if (discoveryThread.joinable()) discoveryThread.join();
        closeSockets();
        std::lock_guard<std::mutex> lock(mutex);
        current.running = false;
    }

    bool bindHttp() {
        int reuse = 1;
        setsockopt(httpFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(config.port);
        address.sin_addr.s_addr = INADDR_ANY;
        return bind(httpFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
               listen(httpFd, 8) == 0;
    }

    bool bindDiscovery() {
        int reuse = 1;
        setsockopt(discoveryFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        timeval timeout{1, 0};
        setsockopt(discoveryFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kDiscoveryPort);
        address.sin_addr.s_addr = INADDR_ANY;
        if (bind(discoveryFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return false;
        ip_mreq membership{};
        membership.imr_multiaddr.s_addr = inet_addr(kDiscoveryGroup);
        membership.imr_interface.s_addr = inet_addr(config.address.c_str());
        return setsockopt(discoveryFd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) == 0;
    }

    void closeSockets() {
        if (httpFd >= 0) close(httpFd);
        if (discoveryFd >= 0) close(discoveryFd);
        httpFd = discoveryFd = -1;
    }

    void httpLoop() {
        while (running) {
            const int client = accept(httpFd, nullptr, nullptr);
            if (client < 0) continue;
            handleHttp(client);
            close(client);
        }
    }

    void handleHttp(int fd) {
        timeval timeout{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::string request;
        char buffer[4096];
        while (request.size() < 256 * 1024) {
            const auto n = recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            request.append(buffer, static_cast<std::size_t>(n));
            const auto end = request.find("\r\n\r\n");
            if (end != std::string::npos && request.size() >= end + 4 + contentLength(request)) break;
        }
        const auto end = request.find("\r\n\r\n");
        const auto path = requestPath(request);
        const auto body = end == std::string::npos ? std::string() : request.substr(end + 4);
        std::string responseBody;
        const bool isTime = body.find("GetSystemDateAndTime") != std::string::npos;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++current.soapRequests;
        }
        if (!isTime && !verifyUsernameToken(body, config.username, config.password)) {
            std::lock_guard<std::mutex> lock(mutex);
            ++current.authenticationFailures;
            responseBody = authFault();
        } else if (path == "/onvif/device_service") {
            responseBody = deviceResponse(body);
        } else if (path == "/onvif/media_service") {
            responseBody = mediaResponse(body);
        } else {
            responseBody = envelope("<s:Fault><s:Reason><s:Text>Unknown ONVIF endpoint</s:Text></s:Reason></s:Fault>");
        }
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/soap+xml; charset=utf-8\r\n"
            "Connection: close\r\nContent-Length: " + std::to_string(responseBody.size()) + "\r\n\r\n" + responseBody;
        sendAll(fd, response);
    }

    std::string deviceResponse(const std::string& body) const {
        const auto base = "http://" + config.address + ":" + std::to_string(config.port) + "/onvif/";
        if (body.find("GetSystemDateAndTime") != std::string::npos) return timeResponse();
        if (body.find("GetDeviceInformation") != std::string::npos)
            return envelope("<tds:GetDeviceInformationResponse><tds:Manufacturer>" + xmlEscape(config.manufacturer) +
                "</tds:Manufacturer><tds:Model>" + xmlEscape(config.model) + "</tds:Model><tds:FirmwareVersion>" +
                xmlEscape(config.firmwareVersion) + "</tds:FirmwareVersion><tds:SerialNumber>" +
                xmlEscape(config.serialNumber) + "</tds:SerialNumber><tds:HardwareId>" +
                xmlEscape(config.hardwareId) + "</tds:HardwareId></tds:GetDeviceInformationResponse>");
        if (body.find("GetCapabilities") != std::string::npos)
            return envelope("<tds:GetCapabilitiesResponse><tds:Capabilities><tt:Device><tt:XAddr>" + base +
                "device_service</tt:XAddr></tt:Device><tt:Media><tt:XAddr>" + base +
                "media_service</tt:XAddr><tt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" "
                "RTP_RTSP_TCP=\"true\"/></tt:Media></tds:Capabilities></tds:GetCapabilitiesResponse>");
        if (body.find("GetServices") != std::string::npos)
            return envelope("<tds:GetServicesResponse><tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl"
                "</tds:Namespace><tds:XAddr>" + base + "device_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major>"
                "<tt:Minor>6</tt:Minor></tds:Version></tds:Service><tds:Service><tds:Namespace>"
                "http://www.onvif.org/ver10/media/wsdl</tds:Namespace><tds:XAddr>" + base +
                "media_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor>"
                "</tds:Version></tds:Service></tds:GetServicesResponse>");
        if (body.find("GetScopes") != std::string::npos)
            return envelope("<tds:GetScopesResponse><tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>"
                "onvif://www.onvif.org/type/video_encoder</tt:ScopeItem></tds:Scopes><tds:Scopes><tt:ScopeDef>"
                "Configurable</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/name/" + xmlEscape(config.deviceName) +
                "</tt:ScopeItem></tds:Scopes></tds:GetScopesResponse>");
        if (body.find("GetHostname") != std::string::npos)
            return envelope("<tds:GetHostnameResponse><tds:HostnameInformation><tt:FromDHCP>false</tt:FromDHCP>"
                "<tt:Name>" + xmlEscape(config.deviceName) + "</tt:Name></tds:HostnameInformation></tds:GetHostnameResponse>");
        if (body.find("GetNetworkInterfaces") != std::string::npos)
            return envelope("<tds:GetNetworkInterfacesResponse><tds:NetworkInterfaces token=\"eth0\" enabled=\"true\">"
                "<tt:Info><tt:Name>eth0</tt:Name><tt:HwAddress>00:00:00:00:00:00</tt:HwAddress><tt:MTU>1500</tt:MTU>"
                "</tt:Info><tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config><tt:Manual><tt:Address>" + config.address +
                "</tt:Address><tt:PrefixLength>24</tt:PrefixLength></tt:Manual><tt:DHCP>false</tt:DHCP></tt:Config>"
                "</tt:IPv4></tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>");
        if (body.find("GetServiceCapabilities") != std::string::npos)
            return envelope("<tds:GetServiceCapabilitiesResponse><tds:Capabilities/></tds:GetServiceCapabilitiesResponse>");
        return envelope("<s:Fault><s:Reason><s:Text>Unsupported device request</s:Text></s:Reason></s:Fault>");
    }

    std::string profile() const {
        return "<trt:Profiles token=\"profile_1\" fixed=\"true\"><tt:Name>MainStream</tt:Name>"
            "<tt:VideoSourceConfiguration token=\"video_source_1\"><tt:Name>VideoSource</tt:Name><tt:UseCount>1"
            "</tt:UseCount><tt:SourceToken>source_1</tt:SourceToken><tt:Bounds x=\"0\" y=\"0\" width=\"" +
            std::to_string(config.width) + "\" height=\"" + std::to_string(config.height) +
            "\"/></tt:VideoSourceConfiguration>" + encoderConfig() + "</trt:Profiles>";
    }

    std::string encoderConfig() const {
        return "<tt:VideoEncoderConfiguration token=\"video_encoder_1\"><tt:Name>H264 Main Stream</tt:Name>"
            "<tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding><tt:Resolution><tt:Width>" +
            std::to_string(config.width) + "</tt:Width><tt:Height>" + std::to_string(config.height) +
            "</tt:Height></tt:Resolution><tt:Quality>5</tt:Quality><tt:RateControl><tt:FrameRateLimit>" +
            std::to_string(config.fps) + "</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval>"
            "<tt:BitrateLimit>" + std::to_string(config.bitrateKbps) + "</tt:BitrateLimit></tt:RateControl>"
            "<tt:H264><tt:GovLength>" + std::to_string(config.gop) + "</tt:GovLength><tt:H264Profile>Baseline"
            "</tt:H264Profile></tt:H264><tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0"
            "</tt:IPv4Address></tt:Address><tt:Port>0</tt:Port><tt:TTL>1</tt:TTL><tt:AutoStart>false</tt:AutoStart>"
            "</tt:Multicast><tt:SessionTimeout>PT60S</tt:SessionTimeout></tt:VideoEncoderConfiguration>";
    }

    std::string mediaResponse(const std::string& body) const {
        if (body.find("GetProfiles") != std::string::npos)
            return envelope("<trt:GetProfilesResponse>" + profile() + "</trt:GetProfilesResponse>");
        if (body.find("GetProfile") != std::string::npos)
            return envelope("<trt:GetProfileResponse>" + profile() + "</trt:GetProfileResponse>");
        if (body.find("GetVideoSources") != std::string::npos)
            return envelope("<trt:GetVideoSourcesResponse><trt:VideoSources token=\"source_1\"><tt:Framerate>" +
                std::to_string(config.fps) + "</tt:Framerate><tt:Resolution><tt:Width>" +
                std::to_string(config.width) + "</tt:Width><tt:Height>" + std::to_string(config.height) +
                "</tt:Height></tt:Resolution></trt:VideoSources></trt:GetVideoSourcesResponse>");
        if (body.find("GetVideoEncoderConfigurations") != std::string::npos)
            return envelope("<trt:GetVideoEncoderConfigurationsResponse>" + encoderConfig() +
                "</trt:GetVideoEncoderConfigurationsResponse>");
        if (body.find("GetVideoEncoderConfigurationOptions") != std::string::npos)
            return envelope("<trt:GetVideoEncoderConfigurationOptionsResponse><trt:Options><tt:QualityRange>"
                "<tt:Min>1</tt:Min><tt:Max>10</tt:Max></tt:QualityRange><tt:H264><tt:ResolutionsAvailable>"
                "<tt:Width>" + std::to_string(config.width) + "</tt:Width><tt:Height>" +
                std::to_string(config.height) + "</tt:Height></tt:ResolutionsAvailable><tt:GovLengthRange>"
                "<tt:Min>1</tt:Min><tt:Max>300</tt:Max></tt:GovLengthRange><tt:FrameRateRange><tt:Min>1</tt:Min>"
                "<tt:Max>" + std::to_string(config.fps) + "</tt:Max></tt:FrameRateRange><tt:H264ProfilesSupported>"
                "Baseline</tt:H264ProfilesSupported></tt:H264></trt:Options></trt:GetVideoEncoderConfigurationOptionsResponse>");
        if (body.find("GetVideoEncoderConfiguration") != std::string::npos)
            return envelope("<trt:GetVideoEncoderConfigurationResponse>" + encoderConfig() +
                "</trt:GetVideoEncoderConfigurationResponse>");
        if (body.find("GetStreamUri") != std::string::npos) {
            const auto uri = "rtsp://" + config.address + ":" + std::to_string(config.rtspPort) + config.rtspPath;
            return envelope("<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>" + xmlEscape(uri) +
                "</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false"
                "</tt:InvalidAfterReboot><tt:Timeout>PT60S</tt:Timeout></trt:MediaUri></trt:GetStreamUriResponse>");
        }
        if (body.find("SetSynchronizationPoint") != std::string::npos) {
            if (config.requestKeyframe && !config.requestKeyframe())
                return envelope("<s:Fault><s:Reason><s:Text>Unable to request keyframe</s:Text></s:Reason></s:Fault>");
            return envelope("<trt:SetSynchronizationPointResponse/>");
        }
        return envelope("<s:Fault><s:Reason><s:Text>Unsupported media request</s:Text></s:Reason></s:Fault>");
    }

    std::string probeMatch(const std::string& relatesTo) const {
        const auto uuid = "urn:uuid:" + config.deviceUuid;
        const auto xaddr = "http://" + config.address + ":" + std::to_string(config.port) + "/onvif/device_service";
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:a=\"http://www.w3.org/2005/08/addressing\" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
            "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"><s:Header><a:MessageID>urn:uuid:" + config.deviceUuid +
            "-probe</a:MessageID><a:RelatesTo>" + xmlEscape(relatesTo) + "</a:RelatesTo><a:To>"
            "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:To><a:Action>"
            "http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</a:Action></s:Header><s:Body><d:ProbeMatches>"
            "<d:ProbeMatch><a:EndpointReference><a:Address>" + uuid + "</a:Address></a:EndpointReference>"
            "<d:Types>dn:NetworkVideoTransmitter</d:Types><d:Scopes>onvif://www.onvif.org/type/video_encoder "
            "onvif://www.onvif.org/name/" + xmlEscape(config.deviceName) + " onvif://www.onvif.org/hardware/" +
            xmlEscape(config.hardwareId) + "</d:Scopes><d:XAddrs>" + xaddr + "</d:XAddrs><d:MetadataVersion>1"
            "</d:MetadataVersion></d:ProbeMatch></d:ProbeMatches></s:Body></s:Envelope>";
    }

    void discoveryLoop() {
        char buffer[16384];
        while (running) {
            sockaddr_in peer{};
            socklen_t length = sizeof(peer);
            const auto n = recvfrom(discoveryFd, buffer, sizeof(buffer) - 1, 0,
                                    reinterpret_cast<sockaddr*>(&peer), &length);
            if (n <= 0) continue;
            std::string request(buffer, static_cast<std::size_t>(n));
            if (request.find("Probe") == std::string::npos) continue;
            std::string messageId;
            extractElement(request, "MessageID", messageId);
            const auto response = probeMatch(messageId.empty() ? "urn:uuid:unknown" : messageId);
            sendto(discoveryFd, response.data(), response.size(), 0,
                   reinterpret_cast<sockaddr*>(&peer), length);
            std::lock_guard<std::mutex> lock(mutex);
            ++current.discoveryRequests;
        }
    }

    OnvifStatus status() const {
        std::lock_guard<std::mutex> lock(mutex);
        return current;
    }

    void setError(const Error& value) {
        std::lock_guard<std::mutex> lock(mutex);
        error = value;
    }

    Error lastError() const {
        std::lock_guard<std::mutex> lock(mutex);
        return error;
    }

    OnvifConfig config;
    mutable std::mutex mutex;
    OnvifStatus current;
    Error error;
    std::atomic<bool> running{false};
    int httpFd = -1;
    int discoveryFd = -1;
    std::thread httpThread;
    std::thread discoveryThread;
};

OnvifService::OnvifService() : impl_(std::make_unique<Impl>()) {}
OnvifService::~OnvifService() = default;
OnvifService::OnvifService(OnvifService&&) noexcept = default;
OnvifService& OnvifService::operator=(OnvifService&&) noexcept = default;

bool OnvifService::start(const OnvifConfig& config) {
    if (!impl_) return false;
    Error error;
    const bool ok = impl_->start(config, error);
    return ok;
}
void OnvifService::stop() noexcept { if (impl_) impl_->stop(); }
bool OnvifService::running() const noexcept { return impl_ && impl_->running.load(); }
OnvifStatus OnvifService::status() const noexcept { return impl_ ? impl_->status() : OnvifStatus{}; }
Error OnvifService::lastError() const {
    return impl_ ? impl_->lastError() : Error{ErrorCode::NotInitialized, "moved-from OnvifService"};
}

}  // namespace recamera
