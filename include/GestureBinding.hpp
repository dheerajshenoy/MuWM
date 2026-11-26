#pragma once

#include "lua.hpp"

#include <functional>

struct GestureBinding
{
    int fingers       = 0;
    wlr_direction dir = WLR_DIRECTION_LEFT;
    int ref           = LUA_NOREF; // Lua callback, if any
    std::function<void()> native;  // built-in action
};
