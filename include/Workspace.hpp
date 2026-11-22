#pragma once

#include "Output.hpp"
#include "Toplevel.hpp"

#include <string>
#include <vector>

struct Workspace
{
    Output *output            = nullptr;
    Toplevel *focusedToplevel = nullptr;
    wlr_scene_tree *sceneTree = nullptr; // parent of this workspace

    std::vector<Toplevel *> toplevels;
    std::string name;
};
