#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

class MuWM;

struct Popup
{
    MuWM *wm           = nullptr;
    wlr_xdg_popup *xdg = nullptr;
    wlr_scene_tree *parentTree = nullptr; // tree of the surface it opens from
    Listener<Popup> commit, destroy;

    void onCommit(void *);
    void onDestroy(void *);
};
