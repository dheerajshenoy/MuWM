#pragma once

#include "wlroots.hpp"
#include "Listener.hpp"

class MuWM;

struct Output
{
    MuWM *wm                      = nullptr;
    wlr_output *output            = nullptr;
    wlr_scene_output *sceneOutput = nullptr;
    Listener<Output> frame;
    Listener<Output> destroy;

    void onFrame(void *data);
    void onDestroy(void *data);
};

