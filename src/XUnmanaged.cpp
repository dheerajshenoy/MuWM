#include "XUnmanaged.hpp"

#include "MuWM.hpp"

void
XUnmanaged::place()
{
    if (tree)
        wlr_scene_node_set_position(&tree->node, xsurface->x, xsurface->y);
}

void
XUnmanaged::onAssociate(void *)
{
    tree = wlr_scene_subsurface_tree_create(wm->windowTree(), xsurface->surface);
    map.bind<&XUnmanaged::onMap>(&xsurface->surface->events.map, this);
    unmap.bind<&XUnmanaged::onUnmap>(&xsurface->surface->events.unmap, this);
    place();
}

void
XUnmanaged::onDissociate(void *)
{
    wm->releaseUnmanagedFocus(this); // its surface is going away
    map.unbind();
    unmap.unbind();
    tree = nullptr; // the scene tree goes away with the surface
}

void
XUnmanaged::onMap(void *)
{
    place();
    wlr_scene_node_raise_to_top(&tree->node); // above every window

    // Menus with keyboard navigation ask for the keyboard; tooltips don't.
    if (wlr_xwayland_surface_override_redirect_wants_focus(xsurface))
        wm->focusUnmanaged(this);
}

void
XUnmanaged::onUnmap(void *)
{
    wm->releaseUnmanagedFocus(this);
}

void
XUnmanaged::onRequestConfigure(void *data)
{
    // It positions itself, so grant whatever it asks for.
    auto *ev = static_cast<wlr_xwayland_surface_configure_event *>(data);
    wlr_xwayland_surface_configure(xsurface, ev->x, ev->y, ev->width,
                                   ev->height);
}

void
XUnmanaged::onSetGeometry(void *)
{
    place();
}

void
XUnmanaged::onDestroy(void *)
{
    wm->releaseUnmanagedFocus(this);
    associate.unbind();
    dissociate.unbind();
    map.unbind();
    unmap.unbind();
    destroy.unbind();
    requestConfigure.unbind();
    setGeometry.unbind();
    delete this;
}
