#include "Toplevel.hpp"

#include "MuWM.hpp"

void
Toplevel::onMap(void *)
{
    wlr_log(WLR_INFO, "Window mapped: %s",
            xdg->title ? xdg->title : "(untitled)");
    // Dialogs (they have a parent) and fixed-size windows don't belong in the
    // tiled layout.
    const auto &c    = xdg->current;
    const bool fixed = c.min_width > 0 && c.min_width == c.max_width
                       && c.min_height > 0 && c.min_height == c.max_height;
    if (xdg->parent || fixed)
        wm->setFloating(this, true);

    // Advertise the window to ext-foreign-toplevel-list; pickers and the
    // window capture source are keyed on this handle.
    wlr_ext_foreign_toplevel_handle_v1_state st{
        xdg->title ? xdg->title : "", xdg->app_id ? xdg->app_id : ""};
    foreignHandle = wlr_ext_foreign_toplevel_handle_v1_create(
        wm->foreignToplevelList(), &st);
    foreignHandle->data = this;

    wm->addToplevel(this);
}

void
Toplevel::updateForeignHandle()
{
    if (!foreignHandle)
        return;
    wlr_ext_foreign_toplevel_handle_v1_state st{
        xdg->title ? xdg->title : "", xdg->app_id ? xdg->app_id : ""};
    wlr_ext_foreign_toplevel_handle_v1_update_state(foreignHandle, &st);
}

void
Toplevel::destroyForeignHandle()
{
    if (!foreignHandle)
        return;
    wlr_ext_foreign_toplevel_handle_v1_destroy(foreignHandle);
    foreignHandle = nullptr;
}

void
Toplevel::onSetTitle(void *)
{
    updateForeignHandle();
}

void
Toplevel::onSetAppId(void *)
{
    updateForeignHandle();
}

void
Toplevel::onCommit(void *)
{
    if (xdg->base->initial_commit)
    {
        wlr_xdg_toplevel_set_size(xdg, 0, 0); // 0,0 = "client picks its size"
        // A client can ask for fullscreen before its first commit.
        wm->setFullscreen(this, xdg->requested.fullscreen);
    }
}

void
Toplevel::onRequestFullscreen(void *)
{
    wm->setFullscreen(this, xdg->requested.fullscreen);
    // The client waits for a configure answering its request, even if we
    // changed nothing.
    if (xdg->base->initialized)
        wlr_xdg_surface_schedule_configure(xdg->base);
}

void
Toplevel::onUnmap(void *)
{
    destroyForeignHandle();
    wm->removeToplevel(this);
}

void
Toplevel::onDestroy(void *)
{
    destroyForeignHandle();
    wm->removeToplevel(this);
    map.unbind();
    unmap.unbind();
    commit.unbind();
    destroy.unbind();
    requestFullscreen.unbind();
    setTitle.unbind();
    setAppId.unbind();
    delete this;
}

void
Toplevel::resizeBorders(int w, int h)
{
    const int b = wm->config().border.width;
    wlr_scene_rect_set_size(border[0], w + 2 * b, b);
    wlr_scene_node_set_position(&border[0]->node, -b, -b);
    wlr_scene_rect_set_size(border[1], w + 2 * b, b);
    wlr_scene_node_set_position(&border[1]->node, -b, h);
    wlr_scene_rect_set_size(border[2], b, h);
    wlr_scene_node_set_position(&border[2]->node, -b, 0);
    wlr_scene_rect_set_size(border[3], b, h);
    wlr_scene_node_set_position(&border[3]->node, w, 0);
}

void
Toplevel::setBordersVisible(bool visible)
{
    for (auto *r : border)
        wlr_scene_node_set_enabled(&r->node, visible);
}

void
Toplevel::setFocused(bool focused)
{
    for (auto *r : border)
        wlr_scene_rect_set_color(r, focused
                                        ? wm->config().border.focused_color
                                        : wm->config().border.unfocused_color);
}
