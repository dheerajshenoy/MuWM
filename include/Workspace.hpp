#pragma once

#include "Output.hpp"
#include "Toplevel.hpp"

#include <vector>

struct Workspace
{
    Output *output = nullptr;
    std::vector<Toplevel *> toplevels;
    Toplevel *focusedToplevel = nullptr;
};
