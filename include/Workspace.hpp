#pragma once

#include "Animation.hpp"
#include "Output.hpp"
#include "Toplevel.hpp"

#include <string>
#include <vector>

struct Workspace
{
    Output *output                      = nullptr;
    Toplevel *focusedToplevel           = nullptr;
    wlr_scene_tree *sceneTree           = nullptr; // parent of this workspace
    wlr_ext_workspace_handle_v1 *handle = nullptr;
    std::vector<Toplevel *> focusHistory; // most recent last
    std::vector<Toplevel *> toplevels;
    std::string name;
    uint64_t id = 0; // unique and stable; 0 means none
    bool urgent = false;
    size_t slot;
    float masterRatio = -1.0f; // < 0: use the configured default
    Animation slideX, slideY;
};
