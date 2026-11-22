#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

class MuWM;

struct LayerShellSurface
{
    MuWM *wm                            = nullptr;
    wlr_layer_surface_v1 *layerSurface  = nullptr;
    wlr_scene_layer_surface_v1 *scene   = nullptr;
    Listener<LayerShellSurface> map;
    Listener<LayerShellSurface> unmap;
    Listener<LayerShellSurface> commit;
    Listener<LayerShellSurface> destroy;

    void onMap(void *data);
    void onUnmap(void *data);
    void onCommit(void *data);
    void onDestroy(void *data);

    // Launchers and lock screens ask for the keyboard; wallpapers and most
    // bars don't. Only the top and overlay layers may take it.
    bool wantsKeyboard() const;
};
