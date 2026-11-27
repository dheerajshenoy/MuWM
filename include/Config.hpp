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

enum class Easing
{
    Linear = 0,
    OutCubic,
    InOutCubic,
};

enum class AnimStyle
{
    None = 0,
    Slide,
    Fade,
    Zoom,
};

struct AnimSpec
{
    bool enabled    = true;
    int duration_ms = -1; // < 0: use animation.duration_ms
    Easing curve    = Easing::OutCubic;
    AnimStyle style = AnimStyle::Slide;
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
        float sticky_color[4]    = {0.75f, 0.45f, 1.0f, 1.0f};
        bool smart               = true;
        bool fullscreen          = false;
    } border;

    struct KeyboardConfig
    {
        std::string file, rules, model, layout, variant, options;
        int repeat_rate  = 25;
        int repeat_delay = 600;
    } keyboard;

    struct PointerConfig
    {
        std::string accel_profile = "adaptive";
        float accel_speed         = 0.0f;
        bool left_handed          = false;
        bool natural_scroll       = false;
        bool middle_emulation     = false;
    };

    struct MouseConfig : public PointerConfig
    {
        unsigned int rotation_angle = 0; // 0, 90, 180, 270
    } mouse;

    struct TouchpadConfig : public PointerConfig
    {
        bool tap_to_click          = true;
        bool disable_while_typing  = true;
        bool clickfinger_behavior  = false;
        int drag_3fg               = 0;
        int drag_lock              = 0;
        bool flipx                 = false;
        bool flipy                 = false;
        std::string tap_button_map = "lrm";
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

    struct Gestures
    {
        float swipe_threshold
            = 100.0f; // how far fingers move before a swipe counts
    } gestures;

    struct Animation
    {
        bool enabled    = true; // master switch
        int duration_ms = 200;  // default for every kind
        AnimSpec move;
        AnimSpec open{.duration_ms = 150, .style = AnimStyle::Slide};
        AnimSpec close{.duration_ms = 150, .style = AnimStyle::Zoom};
        AnimSpec workspace{.duration_ms = 250};
    } animation;

    // Only drawn in scenefx builds; elsewhere the settings are accepted and
    // ignored, so one init.lua works with both.
    struct Shadow
    {
        bool enabled       = true;
        float opacity      = 0.5f;
        int sigma          = 10;
        int offset_x       = 0;
        int offset_y       = 0;
        float color[4]     = {0.0f, 0.0f, 0.0f, 1.0f};
        bool floating_only = false;
    } shadow;
};
