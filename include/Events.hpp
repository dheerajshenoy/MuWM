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
    uint64_t workspaceId = 0; // 0: not on a workspace
    std::string output;
    std::string tag;
    std::string tagDescription;
    int x           = 0;
    int y           = 0;
    int width       = 0;
    int height      = 0;
    bool focused    = false;
    bool floating   = false;
    bool fullscreen = false;
    bool sticky     = false;
};

struct WorkspaceInfo
{
    uint64_t id = 0;
    std::string name;
    std::string output;    // name of the output it belongs to
    int index         = 0; // 1-based position in the configured list
    int windowCount   = 0;
    float masterRatio = 0.0f;
    bool visible      = false; // shown on its output
    bool urgent       = false;
};

struct OutputInfo
{
    std::string name;
    std::string description;
    uint64_t workspaceId = 0; // the workspace it shows
    int x                = 0;
    int y                = 0;
    int width            = 0;
    int height           = 0;
    float scale          = 1.0f;
    bool focused         = false;
};

// monostate: events without a payload.
using EventData = std::variant<std::monostate, WindowInfo, WorkspaceInfo>;
