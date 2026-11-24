#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

struct Workspace;
class MuWM;

struct Toplevel
{
    MuWM *wm                       = nullptr;
    wlr_xdg_toplevel *xdg          = nullptr;
    wlr_xwayland_surface *xsurface = nullptr; // for XWayland windows
    wlr_scene_tree *tree           = nullptr; // container: surface + borders
    wlr_scene_tree *surfaceTree    = nullptr; // just the client's surfaces
    wlr_scene_rect *border[4]      = {};      // top, bottom, left, right
    wlr_box floatBox               = {};
    Listener<Toplevel> map;
    Listener<Toplevel> unmap;
    Listener<Toplevel> commit;
    Listener<Toplevel> destroy;
    Listener<Toplevel> requestFullscreen;
    Listener<Toplevel> setTitle;
    Listener<Toplevel> setAppId;
    Listener<Toplevel> associate;        // X11: the window got a wl_surface
    Listener<Toplevel> dissociate;       // X11: the wl_surface is going away
    Listener<Toplevel> requestConfigure; // X11: the client wants a geometry
    wlr_ext_foreign_toplevel_handle_v1 *foreignHandle = nullptr; // while mapped
    Workspace *workspace                              = nullptr;
    uint64_t id                                       = 0;
    bool fullscreen                                   = false;
    bool floating                                     = false;
    wlr_box slot = {}; // size the layout asked for; the borders never exceed it

    // The size limits seen at the last commit, to notice when a client changes
    // them after mapping.
    struct SizeLimits
    {
        int minW = 0, minH = 0, maxW = 0, maxH = 0;
        bool operator==(const SizeLimits &) const = default;
    } limits;

    void onMap(void *data);
    void onUnmap(void *data);
    void onCommit(void *data);
    void onDestroy(void *data);
    void onRequestFullscreen(void *data);
    void onSetTitle(void *data);
    void onSetAppId(void *data);
    void onAssociate(void *data);
    void onDissociate(void *data);
    void onRequestConfigure(void *data);
    void updateForeignHandle();
    void destroyForeignHandle();
    void clampSize(int &w, int &h) const;
    void setTiled(bool tiled);
    void fitBorders();

    void setFocused(bool focused);
    void resizeBorders(int w, int h);
    void setBordersVisible(bool visible);

    // Everything the compositor needs from the client side of a window. The
    // rest of the code calls these and never touches `xdg` directly.
    wlr_surface *surface() const;
    wlr_box geometry() const; // the window area, without shadows
    void setSize(int width, int height);
    void setActivated(bool activated);
    void setFullscreen(bool fullscreen); // tells the client; see MuWM::
    void scheduleConfigure();
    void close();
    const char *title() const; // never null
    const char *appId() const; // never null
    bool isDialog() const;     // has a parent window
    bool isFixedSize() const;  // min size equals max size
};
