#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

#include <unordered_set>

class MuWM;

struct Keyboard
{
    MuWM *wm         = nullptr;
    wlr_keyboard *kb = nullptr;
    Listener<Keyboard> key;
    Listener<Keyboard> modifiers;
    Listener<Keyboard> destroy;

    void onKey(void *data);
    void onModifiers(void *data);
    void onDestroy(void *data);


    std::unordered_set<xkb_keycode_t>
        consumed; // keycodes of pressed keys we handled
};
