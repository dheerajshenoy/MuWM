#pragma once

#include "lua.hpp"

#include <cstdint>

extern "C"
{
#include <xkbcommon/xkbcommon.h>
}

struct Binding
{
    uint32_t mods    = 0;
    xkb_keysym_t sym = XKB_KEY_NoSymbol;
    int ref          = LUA_NOREF;
};
