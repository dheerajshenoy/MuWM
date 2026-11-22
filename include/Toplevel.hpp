#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

class MuWM;

struct Toplevel
{
    MuWM *wm              = nullptr;
    wlr_xdg_toplevel *xdg = nullptr;
    wlr_scene_tree *tree  = nullptr;
    Listener<Toplevel> map;
    Listener<Toplevel> commit;
    Listener<Toplevel> destroy;

    void onMap(void *data);
    void onCommit(void *data);
    void onDestroy(void *data);
};
