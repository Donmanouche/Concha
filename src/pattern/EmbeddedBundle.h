#pragma once

#include <string_view>

namespace concha {

// js/src/concha.mjs bundled by esbuild, compiled into the binary (cmake/StrudelBundle.cmake).
std::string_view embeddedBundle();

} // namespace concha
