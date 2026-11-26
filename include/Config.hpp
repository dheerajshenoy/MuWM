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

    float scale              = 1.0f;
    bool focus_follows_mouse = true;
    bool mouse_follows_focus = false;
    bool raise_on_focus      = false;
    std::string bell_sound   = "default";
    bool apply_tearing       = false;
    bool dynamic_workspaces  = true;
    bool focus_moved_window  = false; // follow a window moved to a workspace

    std::vector<std::string> workspaces
        = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};

    struct Cursor
    {
        int size          = 24;
        std::string theme = "default";
    } cursor;

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
        bool natural_scroll       = false;
        bool tap_to_click         = true;
        bool middle_emulation     = true;
        bool disable_while_typing = true;
    } touchpad;

    struct Layout
    {
        float master_ratio = 0.55f;
    } layout;

    struct Dim
    {
        float unfocused = 0.0f; // black overlay alpha; 0 = off
        float modal     = 0.0f; // for a window with a modal dialog open
    } dim;

    struct Opacity
    {
        float focused   = 1.0f; // 1 = opaque (off)
        float unfocused = 1.0f;
        float modal     = 1.0f; // for a window with a modal dialog open
    } opacity;
};
