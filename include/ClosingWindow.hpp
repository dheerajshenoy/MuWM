#pragma once

#include "Animation.hpp"
#include "Config.hpp"

struct ClosingWindow
{
    struct Item
    {
        wlr_scene_node *node = nullptr;
        wlr_box box          = {};    // relative to the window's top-left
        std::array<float, 4> color{}; // rects only
        float opacity = 1.0f;         // buffers only
        bool isRect   = false;
    };

    wlr_scene_tree *tree = nullptr;
    std::vector<Item> items;
    int w = 0, h = 0; // window size, for the scale centre
    Animation anim;   // 1 → 0
    AnimStyle style = AnimStyle::Fade;
};
