#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

class MuWM;

struct Pointer
{
    MuWM *wm                 = nullptr;
    wlr_input_device *device = nullptr;
    Listener<Pointer> destroy;

    void onDestroy(void *data);
};
