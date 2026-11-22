#pragma once

#include "lua.hpp"

#include <string>

struct Config
{
    int gap = 10;

    struct Border
    {
        int width                = 2;
        float focused_color[4]   = {0.0f, 0.5f, 1.0f, 1.0f};
        float unfocused_color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    } border;

    struct KeyboardConfig
    {
        std::string rules, model, layout, variant, xkb_options;
        int repeat_rate  = 25;
        int repeat_delay = 600;
    } keyboard;
};
