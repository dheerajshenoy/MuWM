#include "Popup.hpp"

#include "MuWM.hpp"

void
Popup::onCommit(void *)
{
    if (!xdg->base->initial_commit)
        return;

    // Work in the parent's coordinate space, so a menu can't leave the screen.
    int lx, ly;
    wlr_scene_node_coords(&parentTree->node, &lx, &ly);
    if (auto *out = wlr_output_layout_output_at(wm->layout(), lx, ly))
    {
        wlr_box box;
        wlr_output_layout_get_box(wm->layout(), out, &box);
        box.x -= lx;
        box.y -= ly;
        wlr_xdg_popup_unconstrain_from_box(xdg, &box);
    }
    wlr_xdg_surface_schedule_configure(xdg->base); // required for popups
}

void
Popup::onDestroy(void *)
{
    commit.unbind();
    destroy.unbind();
    delete this;
}
