#pragma once

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
    float masterRatio = -1.0f; // < 0: use the configured default
};
