#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

struct Workspace;
class MuWM;

struct Toplevel
{
    MuWM *wm                  = nullptr;
    wlr_xdg_toplevel *xdg     = nullptr;
    wlr_scene_tree *tree      = nullptr; // container: surface + borders
    wlr_scene_tree *surfaceTree = nullptr; // just the client's surfaces
    wlr_scene_rect *border[4] = {}; // top, bottom, left, right
    wlr_box floatBox          = {};
    Listener<Toplevel> map;
    Listener<Toplevel> unmap;
    Listener<Toplevel> commit;
    Listener<Toplevel> destroy;
    Listener<Toplevel> requestFullscreen;
    Listener<Toplevel> setTitle;
    Listener<Toplevel> setAppId;
    wlr_ext_foreign_toplevel_handle_v1 *foreignHandle = nullptr; // while mapped
    Workspace *workspace = nullptr;
    uint64_t id          = 0;
    bool fullscreen      = false;
    bool floating        = false;

    void onMap(void *data);
    void onUnmap(void *data);
    void onCommit(void *data);
    void onDestroy(void *data);
    void onRequestFullscreen(void *data);
    void onSetTitle(void *data);
    void onSetAppId(void *data);
    void updateForeignHandle();
    void destroyForeignHandle();

    void setFocused(bool focused);
    void resizeBorders(int w, int h);
    void setBordersVisible(bool visible);
};
