#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

class MuWM;

struct Toplevel
{
    MuWM *wm                  = nullptr;
    wlr_xdg_toplevel *xdg     = nullptr;
    wlr_scene_tree *tree      = nullptr;
    wlr_scene_rect *border[4] = {}; // top, bottom, left, right
    Listener<Toplevel> map;
    Listener<Toplevel> unmap;
    Listener<Toplevel> commit;
    Listener<Toplevel> destroy;

    void onMap(void *data);
    void onUnmap(void *data);
    void onCommit(void *data);
    void onDestroy(void *data);

    void setFocused(bool focused);
    void resizeBorders(int w, int h);
};
