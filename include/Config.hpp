#pragma once

#include "lua.hpp"

#include <string>
#include <vector>

enum class NewClientPosition
{
    AfterCurrent = 0,
    BeforeCurrent,
    Top,
    Bottom,
};

struct Config
{
    NewClientPosition new_client_position = NewClientPosition::AfterCurrent;

    float scale = 1.0f;

    std::vector<std::string> workspaces = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};

    struct Gaps
    {
        int value  = 10;
        bool smart = true;
    } gaps;

    struct Border
    {
        int width                = 2;
        float focused_color[4]   = {0.0f, 0.5f, 1.0f, 1.0f};
        float unfocused_color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        bool smart               = true;
        bool fullscreen          = false;
    } border;

    struct KeyboardConfig
    {
        std::string rules, model, layout, variant, xkb_options;
        int repeat_rate  = 25;
        int repeat_delay = 600;
    } keyboard;

    struct TouchpadConfig
    {
        bool natural_scroll = true;
        bool tap_to_click   = true;
    } touchpad;

    struct Layout
    {
        float master_ratio = 0.55f;
    } layout;
};
