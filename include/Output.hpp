#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

#include <vector>

struct Workspace;
class MuWM;

struct Output
{
    MuWM *wm                      = nullptr;
    wlr_output *output            = nullptr;
    wlr_scene_output *sceneOutput = nullptr;
    wlr_box usableArea            = {}; // output area left after layer surfaces
    Listener<Output> frame;
    Listener<Output> destroy;
    Listener<Output> requestState;
    std::vector<Workspace *> workspaces;
    Workspace *focusedWorkspace = nullptr;

    void onFrame(void *data);
    void onDestroy(void *data);
    void onRequestState(void *data);
};
