#pragma once

#include "lua.hpp"

#include <string>


struct Config
{
    int gap = 10;

    struct Border
    {
        int width                = 10;
        float focused_color[4]   = {0.0f, 0.5f, 1.0f, 1.0f};
        float unfocused_color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    } border;
};
