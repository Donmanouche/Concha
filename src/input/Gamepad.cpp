#include "input/Gamepad.h"

#include <atomic>
#include <mutex>
#include <thread>

#ifdef __linux__
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>
#endif

namespace concha::input {

namespace {
constexpr int kMaxPads = 4;

struct Reader {
    std::mutex mutex;
    std::array<GamepadState, kMaxPads> pads{};
    std::thread thread;
    std::atomic<bool> stop{false};

    Reader() { thread = std::thread([this] { run(); }); }
    ~Reader()
    {
        stop = true;
        if (thread.joinable()) thread.join();
    }

#ifdef __linux__
    struct Device {
        int fd = -1;
        std::string path;
        struct input_absinfo abs[ABS_CNT];
        bool hasAbs[ABS_CNT];
    };

    static bool testBit(const unsigned long* bits, int bit)
    {
        return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1;
    }

    // a gamepad: the gamepad buttons (BTN_SOUTH...) and a stick
    static bool open(const std::string& path, Device& d)
    {
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return false;
        unsigned long keys[(KEY_CNT + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {};
        unsigned long absBits[(ABS_CNT + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absBits), absBits);
        if (!testBit(keys, BTN_SOUTH) || !testBit(absBits, ABS_X)) {
            ::close(fd);
            return false;
        }
        d.fd = fd;
        d.path = path;
        for (int a = 0; a < ABS_CNT; ++a) {
            d.hasAbs[a] = testBit(absBits, a) && ioctl(fd, EVIOCGABS(a), &d.abs[a]) == 0;
        }
        return true;
    }

    static float norm(const input_absinfo& i, int value, bool unipolar)
    {
        const double span = double(i.maximum) - double(i.minimum);
        if (span <= 0) return 0.f;
        const double x = (double(value) - i.minimum) / span; // 0..1
        return float(unipolar ? x : x * 2.0 - 1.0);
    }

    static int buttonIndex(int code)
    {
        switch (code) {
        case BTN_SOUTH: return 0;
        case BTN_EAST: return 1;
        case BTN_WEST: return 2;
        case BTN_NORTH: return 3;
        case BTN_TL: return 4;
        case BTN_TR: return 5;
        case BTN_TL2: return 6;
        case BTN_TR2: return 7;
        case BTN_SELECT: return 8;
        case BTN_START: return 9;
        case BTN_THUMBL: return 10;
        case BTN_THUMBR: return 11;
        case BTN_DPAD_UP: return 12;
        case BTN_DPAD_DOWN: return 13;
        case BTN_DPAD_LEFT: return 14;
        case BTN_DPAD_RIGHT: return 15;
        case BTN_MODE: return 16;
        default: return -1;
        }
    }

    void apply(int pad, const Device& d, const input_event& e)
    {
        std::lock_guard<std::mutex> lock(mutex);
        GamepadState& s = pads[size_t(pad)];
        if (e.type == EV_KEY) {
            const int b = buttonIndex(e.code);
            if (b >= 0) s.buttons[size_t(b)] = e.value ? 1.f : 0.f;
        } else if (e.type == EV_ABS && e.code < ABS_CNT && d.hasAbs[e.code]) {
            const auto& i = d.abs[e.code];
            switch (e.code) {
            case ABS_X: s.axes[0] = norm(i, e.value, false); break;
            case ABS_Y: s.axes[1] = norm(i, e.value, false); break;
            case ABS_RX: s.axes[2] = norm(i, e.value, false); break;
            case ABS_RY: s.axes[3] = norm(i, e.value, false); break;
            case ABS_Z: s.buttons[6] = norm(i, e.value, true); break;  // analog triggers
            case ABS_RZ: s.buttons[7] = norm(i, e.value, true); break;
            case ABS_HAT0Y:
                s.buttons[12] = e.value < 0 ? 1.f : 0.f;
                s.buttons[13] = e.value > 0 ? 1.f : 0.f;
                break;
            case ABS_HAT0X:
                s.buttons[14] = e.value < 0 ? 1.f : 0.f;
                s.buttons[15] = e.value > 0 ? 1.f : 0.f;
                break;
            default: break;
            }
        }
    }

    std::vector<std::string> scan()
    {
        std::vector<std::string> paths;
        if (DIR* dir = opendir("/dev/input")) {
            while (dirent* e = readdir(dir))
                if (std::strncmp(e->d_name, "event", 5) == 0) paths.push_back(std::string("/dev/input/") + e->d_name);
            closedir(dir);
        }
        std::sort(paths.begin(), paths.end(), [](const std::string& a, const std::string& b) {
            return std::atoi(a.c_str() + 16) < std::atoi(b.c_str() + 16);
        });
        return paths;
    }

    void run()
    {
        std::vector<Device> devices;
        auto lastScan = std::chrono::steady_clock::time_point{};
        while (!stop) {
            // (re)scan every 2 s for new pads while there is room
            const auto now = std::chrono::steady_clock::now();
            if (int(devices.size()) < kMaxPads && now - lastScan > std::chrono::seconds(2)) {
                lastScan = now;
                for (const auto& path : scan()) {
                    if (int(devices.size()) >= kMaxPads) break;
                    if (std::any_of(devices.begin(), devices.end(), [&](const Device& d) { return d.path == path; })) continue;
                    Device d;
                    if (open(path, d)) {
                        devices.push_back(d);
                        std::lock_guard<std::mutex> lock(mutex);
                        pads[devices.size() - 1] = GamepadState{};
                        pads[devices.size() - 1].connected = true;
                    }
                }
            }
            if (devices.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            std::vector<pollfd> fds;
            for (const auto& d : devices) fds.push_back({d.fd, POLLIN, 0});
            if (poll(fds.data(), fds.size(), 200) <= 0) continue;
            for (size_t k = 0; k < devices.size(); ++k) {
                if (fds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) { // unplugged: the later pads move up
                    ::close(devices[k].fd);
                    devices.erase(devices.begin() + long(k));
                    std::lock_guard<std::mutex> lock(mutex);
                    for (size_t j = k; j + 1 < pads.size(); ++j) pads[j] = pads[j + 1];
                    pads.back() = GamepadState{};
                    break;
                }
                if (!(fds[k].revents & POLLIN)) continue;
                input_event ev[64];
                ssize_t n;
                while ((n = read(devices[k].fd, ev, sizeof ev)) > 0)
                    for (size_t i = 0; i < size_t(n) / sizeof(input_event); ++i) apply(int(k), devices[k], ev[i]);
            }
        }
        for (auto& d : devices) ::close(d.fd);
    }
#else
    void run() {}
#endif
};

Reader& reader()
{
    static Reader r;
    return r;
}
} // namespace

GamepadState gamepadState(int index)
{
    if (index < 0 || index >= kMaxPads) return {};
    Reader& r = reader();
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.pads[size_t(index)];
}

} // namespace concha::input
