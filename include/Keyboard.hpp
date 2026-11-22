#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

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
};
