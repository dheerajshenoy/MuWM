#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

class MuWM;

// One surface of the session locker, covering one output.
struct LockSurface
{
    MuWM *wm                           = nullptr;
    wlr_session_lock_surface_v1 *surface = nullptr;
    wlr_scene_tree *tree               = nullptr; // freed with the surface
    int configuredW = 0; // last size sent, so a configure isn't repeated
    int configuredH = 0;
    Listener<LockSurface> map;
    Listener<LockSurface> destroy;

    void onMap(void *data);
    void onDestroy(void *data);
};
