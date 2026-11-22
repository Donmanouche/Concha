#include "StkRawwaves.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

const std::vector<std::pair<std::string_view, std::string_view>>& concha_stk_rawwaves(); // generated

namespace {
std::string cacheFolder()
{
#ifdef _WIN32
    const char* base = std::getenv("LOCALAPPDATA");
    std::string dir = base && *base ? base : ".";
    return dir + "\\Concha\\stk-rawwaves\\";
#else
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    std::string dir;
    if (xdg && *xdg) dir = xdg;
    else {
        const char* home = std::getenv("HOME");
        dir = std::string(home && *home ? home : ".") +
#ifdef __APPLE__
              "/Library/Caches";
#else
              "/.cache";
#endif
    }
    return dir + "/Concha/stk-rawwaves/";
#endif
}
} // namespace

const char* concha_stk_rawwave_path()
{
    static const std::string path = cacheFolder();
    return path.c_str();
}

void concha_stk_extract_rawwaves()
{
    static std::once_flag once;
    std::call_once(once, [] {
        namespace fs = std::filesystem;
        const fs::path dir = fs::u8path(concha_stk_rawwave_path());
        std::error_code ec;
        fs::create_directories(dir, ec);
        for (const auto& [name, data] : concha_stk_rawwaves()) {
            const fs::path file = dir / fs::u8path(std::string(name));
            if (fs::file_size(file, ec) == data.size() && !ec) continue;
            const fs::path part = file.string() + ".part";
            {
                std::ofstream out(part, std::ios::binary);
                out.write(data.data(), std::streamsize(data.size()));
                if (!out) continue;
            }
            fs::rename(part, file, ec);
        }
    });
}
