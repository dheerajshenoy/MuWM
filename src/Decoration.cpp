#include "Decoration.hpp"

void
Decoration::onRequestMode(void *)
{
    // Scheduling a configure before the surface is initialized asserts in
    // wlroots; the initial commit (onCommit) applies the mode in that case.
    if (!deco->toplevel->base->initialized)
        return;
    wlr_xdg_toplevel_decoration_v1_set_mode(
        deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
Decoration::onCommit(void *)
{
    if (deco->toplevel->base->initial_commit)
        wlr_xdg_toplevel_decoration_v1_set_mode(
            deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
Decoration::onDestroy(void *)
{
    requestMode.unbind();
    commit.unbind();
    destroy.unbind();
    delete this;
}
