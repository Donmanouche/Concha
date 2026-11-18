#include "io/ExternalOut.h"

#include <curl/curl.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <variant>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace concha::io {

// ---- packets ----

namespace {
void putLength(std::vector<uint8_t>& out, size_t n) // MQTT "remaining length"
{
    do {
        uint8_t b = uint8_t(n % 128);
        n /= 128;
        if (n) b |= 0x80;
        out.push_back(b);
    } while (n);
}
void putString(std::vector<uint8_t>& out, const std::string& s)
{
    out.push_back(uint8_t(s.size() >> 8));
    out.push_back(uint8_t(s.size() & 0xff));
    out.insert(out.end(), s.begin(), s.end());
}
std::vector<uint8_t> packet(uint8_t header, const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> out{header};
    putLength(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
} // namespace

std::vector<uint8_t> mqttConnectPacket(const std::string& client, const std::string& user, const std::string& password)
{
    std::vector<uint8_t> body;
    putString(body, "MQTT");
    body.push_back(4); // 3.1.1
    uint8_t flags = 0x02; // clean session
    if (!user.empty()) flags |= 0x80;
    if (!user.empty() && !password.empty()) flags |= 0x40;
    body.push_back(flags);
    body.push_back(0);
    body.push_back(60); // keep alive (s)
    putString(body, client);
    if (!user.empty()) putString(body, user);
    if (!user.empty() && !password.empty()) putString(body, password);
    return packet(0x10, body);
}

std::vector<uint8_t> mqttPublishPacket(const std::string& topic, const std::string& payload)
{
    std::vector<uint8_t> body;
    putString(body, topic);
    body.insert(body.end(), payload.begin(), payload.end());
    return packet(0x30, body); // QoS 0
}

std::vector<uint8_t> webSocketFrame(const std::vector<uint8_t>& payload, const uint8_t mask[4])
{
    std::vector<uint8_t> out{0x82}; // FIN, binary
    const size_t n = payload.size();
    if (n < 126) out.push_back(uint8_t(0x80 | n));
    else if (n < 65536) {
        out.push_back(0x80 | 126);
        out.push_back(uint8_t(n >> 8));
        out.push_back(uint8_t(n & 0xff));
    } else {
        out.push_back(0x80 | 127);
        for (int k = 7; k >= 0; --k) out.push_back(uint8_t((uint64_t(n) >> (8 * k)) & 0xff));
    }
    out.insert(out.end(), mask, mask + 4);
    for (size_t i = 0; i < n; ++i) out.push_back(payload[i] ^ mask[i % 4]);
    return out;
}

namespace {

// ---- connections ----

struct Url {
    std::string scheme, host, path;
    int port = 0;
};
bool parseUrl(const std::string& s, Url& u)
{
    const size_t p = s.find("://");
    if (p == std::string::npos) return false;
    u.scheme = s.substr(0, p);
    std::string rest = s.substr(p + 3);
    const size_t slash = rest.find('/');
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    std::string hostPort = rest.substr(0, slash);
    const size_t colon = hostPort.rfind(':'), bracket = hostPort.rfind(']'); // [::1]:8883
    if (colon != std::string::npos && (bracket == std::string::npos || colon > bracket)) {
        u.port = std::atoi(hostPort.c_str() + colon + 1);
        hostPort = hostPort.substr(0, colon);
    }
    u.host = hostPort;
    if (!u.port) u.port = u.scheme == "wss" || u.scheme == "mqtts" ? (u.scheme == "wss" ? 443 : 8883) : (u.scheme == "ws" ? 80 : 1883);
    return u.scheme == "ws" || u.scheme == "wss" || u.scheme == "mqtt" || u.scheme == "mqtts" || u.scheme == "tcp";
}

class MqttConnection {
public:
    explicit MqttConnection(MqttMessage params) : params_(std::move(params)) {}
    ~MqttConnection() { close(); }

    bool connected() const { return curl_ != nullptr; }
    Clock::time_point nextAttempt{};

    bool connect(std::string& error)
    {
        close();
        Url u;
        if (!parseUrl(params_.host, u)) {
            error = "unsupported address " + params_.host + " (ws://, wss://, mqtt://, mqtts://)";
            return false;
        }
        ws_ = u.scheme == "ws" || u.scheme == "wss";
        const bool tls = u.scheme == "wss" || u.scheme == "mqtts";
        curl_ = curl_easy_init();
        const std::string url = std::string(tls ? "https://" : "http://") + u.host + ":" + std::to_string(u.port) + "/";
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_CONNECT_ONLY, 1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        const CURLcode rc = curl_easy_perform(curl_);
        if (rc != CURLE_OK) {
            error = std::string("cannot connect to ") + params_.host + ": " + curl_easy_strerror(rc);
            close();
            return false;
        }
        if (ws_) { // WebSocket upgrade, subprotocol mqtt (as Paho)
            uint8_t key[16];
            for (auto& k : key) k = uint8_t(rng_());
            static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string keyText;
            for (int i = 0; i < 16; i += 3) {
                const uint32_t v = (uint32_t(key[i]) << 16) | (i + 1 < 16 ? uint32_t(key[i + 1]) << 8 : 0) | (i + 2 < 16 ? key[i + 2] : 0);
                keyText += b64[(v >> 18) & 63];
                keyText += b64[(v >> 12) & 63];
                keyText += i + 1 < 16 ? b64[(v >> 6) & 63] : '=';
                keyText += i + 2 < 16 ? b64[v & 63] : '=';
            }
            const std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                                    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + keyText +
                                    "\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: mqtt\r\n\r\n";
            if (!sendRaw(reinterpret_cast<const uint8_t*>(req.data()), req.size())) {
                error = "WebSocket handshake failed (send)";
                close();
                return false;
            }
            std::string response;
            uint8_t c;
            while (response.find("\r\n\r\n") == std::string::npos && response.size() < 8192) {
                if (!recvRaw(&c, 1, 5000)) break;
                response += char(c);
            }
            if (response.find(" 101") == std::string::npos) {
                error = "WebSocket handshake refused by " + params_.host + ": " + response.substr(0, response.find('\r'));
                close();
                return false;
            }
        }
        if (!sendPacket(mqttConnectPacket(params_.client, params_.user, params_.password))) {
            error = "MQTT CONNECT failed";
            close();
            return false;
        }
        std::vector<uint8_t> ack;
        if (!readPacket(ack, 5000) || ack.size() < 4 || (ack[0] & 0xf0) != 0x20) {
            error = "no CONNACK from " + params_.host;
            close();
            return false;
        }
        if (ack[3] != 0) {
            static const char* reasons[] = {"", "unacceptable protocol version", "client id rejected", "server unavailable",
                                            "bad user name or password", "not authorized"};
            error = std::string("MQTT broker refused the connection: ") + (ack[3] < 6 ? reasons[ack[3]] : "error");
            close();
            return false;
        }
        lastSent_ = Clock::now();
        return true;
    }

    bool publish(const std::string& topic, const std::string& payload) { return sendPacket(mqttPublishPacket(topic, payload)); }

    // keep alive: PINGREQ after 30 s of silence; drain what the broker sends
    bool idle()
    {
        if (!curl_) return true;
        std::vector<uint8_t> in;
        while (readAvailable(in)) {
        }
        if (Clock::now() - lastSent_ > std::chrono::seconds(30)) return sendPacket({0xc0, 0x00});
        return true;
    }

    void close()
    {
        if (curl_) curl_easy_cleanup(curl_);
        curl_ = nullptr;
        inBuf_.clear();
    }

private:
    bool sendRaw(const uint8_t* data, size_t n)
    {
        size_t done = 0;
        for (int tries = 0; done < n && tries < 5000; ++tries) {
            size_t sent = 0;
            const CURLcode rc = curl_easy_send(curl_, data + done, n - done, &sent);
            if (rc == CURLE_AGAIN) {
                waitSocket(false, 1);
                continue;
            }
            if (rc != CURLE_OK) return false;
            done += sent;
        }
        return done == n;
    }
    bool recvRaw(uint8_t* data, size_t n, int timeoutMs)
    {
        size_t got = 0;
        const auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (got < n) {
            size_t r = 0;
            const CURLcode rc = curl_easy_recv(curl_, data + got, n - got, &r);
            if (rc == CURLE_AGAIN) {
                if (Clock::now() > until) return false;
                waitSocket(true, 10);
                continue;
            }
            if (rc != CURLE_OK || r == 0) return false;
            got += r;
        }
        return true;
    }
    void waitSocket(bool forRead, int ms)
    {
#if defined(__unix__) || defined(__APPLE__)
        curl_socket_t s = CURL_SOCKET_BAD;
        curl_easy_getinfo(curl_, CURLINFO_ACTIVESOCKET, &s);
        if (s == CURL_SOCKET_BAD) return;
        pollfd p{int(s), short(forRead ? POLLIN : POLLOUT), 0};
        poll(&p, 1, ms);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
    }
    bool sendPacket(const std::vector<uint8_t>& p)
    {
        lastSent_ = Clock::now();
        if (!ws_) return sendRaw(p.data(), p.size());
        uint8_t mask[4];
        for (auto& m : mask) m = uint8_t(rng_());
        const auto f = webSocketFrame(p, mask);
        return sendRaw(f.data(), f.size());
    }
    // one MQTT packet (through the WebSocket frames when ws)
    bool readPacket(std::vector<uint8_t>& out, int timeoutMs)
    {
        const auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (Clock::now() < until) {
            if (takePacket(out)) return true;
            if (!fill(50)) return false;
        }
        return false;
    }
    bool readAvailable(std::vector<uint8_t>& out)
    {
        if (takePacket(out)) return true;
        size_t r = 0;
        uint8_t buf[4096];
        const CURLcode rc = curl_easy_recv(curl_, buf, sizeof buf, &r);
        if (rc == CURLE_AGAIN) return false;
        if (rc != CURLE_OK || r == 0) {
            close();
            return false;
        }
        appendIncoming(buf, r);
        return takePacket(out);
    }
    bool fill(int ms)
    {
        uint8_t buf[4096];
        size_t r = 0;
        const CURLcode rc = curl_easy_recv(curl_, buf, sizeof buf, &r);
        if (rc == CURLE_AGAIN) {
            waitSocket(true, ms);
            return true;
        }
        if (rc != CURLE_OK || r == 0) return false;
        appendIncoming(buf, r);
        return true;
    }
    // raw bytes in; WebSocket frames unwrapped into mqttBuf_
    void appendIncoming(const uint8_t* b, size_t n)
    {
        if (!ws_) {
            mqttBuf_.insert(mqttBuf_.end(), b, b + n);
            return;
        }
        inBuf_.insert(inBuf_.end(), b, b + n);
        for (;;) {
            if (inBuf_.size() < 2) return;
            const bool masked = inBuf_[1] & 0x80;
            uint64_t len = inBuf_[1] & 0x7f;
            size_t h = 2;
            if (len == 126) {
                if (inBuf_.size() < 4) return;
                len = (uint64_t(inBuf_[2]) << 8) | inBuf_[3];
                h = 4;
            } else if (len == 127) {
                if (inBuf_.size() < 10) return;
                len = 0;
                for (int k = 0; k < 8; ++k) len = (len << 8) | inBuf_[size_t(2 + k)];
                h = 10;
            }
            if (masked) h += 4;
            if (inBuf_.size() < h + len) return;
            const uint8_t op = inBuf_[0] & 0x0f;
            if (op == 0x2 || op == 0x0)
                for (uint64_t i = 0; i < len; ++i)
                    mqttBuf_.push_back(masked ? uint8_t(inBuf_[h + i] ^ inBuf_[h - 4 + (i % 4)]) : inBuf_[h + i]);
            inBuf_.erase(inBuf_.begin(), inBuf_.begin() + long(h + len));
        }
    }
    bool takePacket(std::vector<uint8_t>& out)
    {
        if (mqttBuf_.size() < 2) return false;
        size_t len = 0, mul = 1, i = 1;
        for (; i < mqttBuf_.size() && i < 5; ++i) {
            len += (mqttBuf_[i] & 0x7f) * mul;
            mul *= 128;
            if (!(mqttBuf_[i] & 0x80)) break;
        }
        if (i >= mqttBuf_.size() || mqttBuf_.size() < i + 1 + len) return false;
        out.assign(mqttBuf_.begin(), mqttBuf_.begin() + long(i + 1 + len));
        mqttBuf_.erase(mqttBuf_.begin(), mqttBuf_.begin() + long(i + 1 + len));
        return true;
    }

    MqttMessage params_;
    CURL* curl_ = nullptr;
    bool ws_ = false;
    std::vector<uint8_t> inBuf_, mqttBuf_;
    Clock::time_point lastSent_{};
    std::mt19937 rng_{std::random_device{}()};
};

#if defined(__unix__) || defined(__APPLE__)
speed_t baudConstant(int baud)
{
    switch (baud) {
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: return 0;
    }
}
#endif

struct SerialPort {
    int fd = -1;
    std::string path;
    int baud = 0;
    Clock::time_point nextAttempt{};
};

// ---- the sender thread ----

struct Job {
    Clock::time_point due;
    uint64_t seq = 0;
    bool mqtt = true;
    MqttMessage m;
    std::string port;
    int baud = 0;
    std::vector<uint8_t> bytes;
};
struct Later {
    bool operator()(const std::shared_ptr<Job>& a, const std::shared_ptr<Job>& b) const
    {
        return a->due != b->due ? a->due > b->due : a->seq > b->seq;
    }
};

class Sender {
public:
    Sender()
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        thread_ = std::thread([this] { run(); });
    }
    ~Sender()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
#if defined(__unix__) || defined(__APPLE__)
        for (auto& [k, p] : serial_)
            if (p.fd >= 0) ::close(p.fd);
#endif
    }

    void add(std::shared_ptr<Job> job)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() > 10000) return; // nobody listening fast enough: drop
            job->seq = seq_++;
            queue_.push(std::move(job));
        }
        cv_.notify_all();
    }
    void log(std::string m)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (messages_.size() < 200) messages_.push_back(std::move(m));
    }
    std::vector<std::string> take()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        out.swap(messages_);
        return out;
    }

private:
    void run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            const auto wake = queue_.empty() ? Clock::now() + std::chrono::seconds(1) : queue_.top()->due;
            cv_.wait_until(lock, wake, [this] { return stop_ || (!queue_.empty() && queue_.top()->due <= Clock::now()); });
            if (stop_) break;
            std::vector<std::shared_ptr<Job>> due;
            while (!queue_.empty() && queue_.top()->due <= Clock::now()) {
                due.push_back(queue_.top());
                queue_.pop();
            }
            lock.unlock();
            for (auto& j : due) j->mqtt ? sendMqtt(*j) : sendSerial(*j);
            for (auto& [k, c] : mqtt_)
                if (c->connected() && !c->idle()) log("[mqtt] connection to " + k.substr(0, k.find('\n')) + " lost");
            lock.lock();
        }
    }

    void sendMqtt(const Job& j)
    {
        if (j.m.topic.empty()) {
            log("[mqtt] no topic: give one to .mqtt() or set `topic` in the pattern");
            return;
        }
        const std::string key = j.m.host + "\n" + j.m.client;
        auto& c = mqtt_[key];
        if (!c) c = std::make_unique<MqttConnection>(j.m);
        if (!c->connected()) {
            if (Clock::now() < c->nextAttempt) return; // dropped, as while Paho connects
            std::string error;
            if (!c->connect(error)) {
                c->nextAttempt = Clock::now() + std::chrono::seconds(5);
                log("[mqtt] " + error);
                return;
            }
            log("[mqtt] connected to " + j.m.host);
        }
        if (!c->publish(j.m.topic, j.m.payload)) {
            c->close();
            log("[mqtt] connection to " + j.m.host + " lost");
        }
    }

    void sendSerial(const Job& j)
    {
#if defined(__unix__) || defined(__APPLE__)
        auto& p = serial_[j.port];
        if (p.fd < 0) {
            if (Clock::now() < p.nextAttempt) return;
            std::string path = j.port;
            if (path == "default" || path.empty()) {
                path.clear();
                for (const char* pattern : {"/dev/ttyACM*", "/dev/ttyUSB*", "/dev/cu.usbmodem*", "/dev/cu.usbserial*"}) {
                    glob_t g{};
                    if (glob(pattern, 0, nullptr, &g) == 0 && g.gl_pathc > 0) path = g.gl_pathv[0];
                    globfree(&g);
                    if (!path.empty()) break;
                }
            }
            const speed_t speed = baudConstant(j.baud);
            if (path.empty() || !speed) {
                p.nextAttempt = Clock::now() + std::chrono::seconds(5);
                log(path.empty() ? "[serial] no serial port found (/dev/ttyACM*, /dev/ttyUSB*)"
                                 : "[serial] unsupported baud rate " + std::to_string(j.baud));
                return;
            }
            const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                p.nextAttempt = Clock::now() + std::chrono::seconds(5);
                log("[serial] cannot open " + path + ": " + std::strerror(errno));
                return;
            }
            termios t{};
            tcgetattr(fd, &t);
            cfmakeraw(&t);
            t.c_cflag |= CLOCAL | CREAD;
            cfsetispeed(&t, speed);
            cfsetospeed(&t, speed);
            tcsetattr(fd, TCSANOW, &t);
            p.fd = fd;
            p.path = path;
            p.baud = j.baud;
            log("[serial] opened " + path + " at " + std::to_string(j.baud) + " baud");
        }
        size_t done = 0;
        for (int tries = 0; done < j.bytes.size() && tries < 100; ++tries) {
            const ssize_t w = ::write(p.fd, j.bytes.data() + done, j.bytes.size() - done);
            if (w > 0) done += size_t(w);
            else if (w < 0 && errno != EAGAIN) {
                log("[serial] " + p.path + ": " + std::strerror(errno));
                ::close(p.fd);
                p.fd = -1;
                p.nextAttempt = Clock::now() + std::chrono::seconds(2);
                return;
            } else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#else
        (void)j;
        log("[serial] not available on this system yet");
#endif
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<std::shared_ptr<Job>, std::vector<std::shared_ptr<Job>>, Later> queue_;
    uint64_t seq_ = 0;
    bool stop_ = false;
    std::vector<std::string> messages_;
    std::map<std::string, std::unique_ptr<MqttConnection>> mqtt_; // sender thread only
    std::map<std::string, SerialPort> serial_;                    // sender thread only
    std::thread thread_;
};

Sender& sender()
{
    static Sender s;
    return s;
}
} // namespace

void sendMqtt(MqttMessage message, Clock::time_point due)
{
    auto j = std::make_shared<Job>();
    j->due = due;
    j->mqtt = true;
    j->m = std::move(message);
    sender().add(std::move(j));
}

void sendSerial(std::string port, int baud, std::vector<uint8_t> bytes, Clock::time_point due)
{
    auto j = std::make_shared<Job>();
    j->due = due;
    j->mqtt = false;
    j->port = std::move(port);
    j->baud = baud;
    j->bytes = std::move(bytes);
    sender().add(std::move(j));
}

std::vector<std::string> takeMessages() { return sender().take(); }

} // namespace concha::io
