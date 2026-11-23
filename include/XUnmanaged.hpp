#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

class MuWM;

// An override-redirect X11 window: a menu, tooltip or drop-down. It places
// itself, so it is never tiled, bordered or focused by the compositor.
struct XUnmanaged
{
    MuWM *wm                       = nullptr;
    wlr_xwayland_surface *xsurface = nullptr;
    wlr_scene_tree *tree           = nullptr; // freed with the surface
    Listener<XUnmanaged> associate;
    Listener<XUnmanaged> dissociate;
    Listener<XUnmanaged> map;
    Listener<XUnmanaged> unmap;
    Listener<XUnmanaged> destroy;
    Listener<XUnmanaged> requestConfigure;
    Listener<XUnmanaged> setGeometry;

    void onAssociate(void *data);
    void onDissociate(void *data);
    void onMap(void *data);
    void onUnmap(void *data);
    void onDestroy(void *data);
    void onRequestConfigure(void *data);
    void onSetGeometry(void *data);

    void place(); // move the scene node to where the window asked to be
};
