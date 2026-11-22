#pragma once

#include <cstdint>
#include <string>
#include <variant>

// Event payloads are plain copies of the state at emit time, so handlers never
// see a pointer into compositor objects that may already be gone.
struct WindowInfo
{
    uint64_t id = 0;
    std::string title;
    std::string appId;
    std::string workspace;
    std::string output;
    int width    = 0;
    int height   = 0;
    bool focused = false;
};

// monostate: events without a payload.
using EventData = std::variant<std::monostate, WindowInfo>;
