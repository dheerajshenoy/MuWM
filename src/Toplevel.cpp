#include "Toplevel.hpp"

#include "MuWM.hpp"

void
Toplevel::onMap(void *)
{
    wlr_log(WLR_INFO, "Window mapped: %s",
            xdg->title ? xdg->title : "(untitled)");
    wm->addToplevel(this);
}

void
Toplevel::onCommit(void *)
{
    if (xdg->base->initial_commit)
        wlr_xdg_toplevel_set_size(xdg, 0, 0); // 0,0 = "client picks its size"
}

void
Toplevel::onUnmap(void *)
{
    wm->removeToplevel(this);
}

void
Toplevel::onDestroy(void *)
{
    wm->removeToplevel(this);
    map.unbind();
    unmap.unbind();
    commit.unbind();
    destroy.unbind();
    delete this;
}

void
Toplevel::resizeBorders(int w, int h)
{
    int b = wm->config().border.width;
    b     = 10;
    wlr_scene_rect_set_size(border[0], w + 2 * b, b); // top
    wlr_scene_node_set_position(&border[0]->node, -b, -b);
    wlr_scene_rect_set_size(border[1], w + 2 * b, b); // bottom
    wlr_scene_node_set_position(&border[1]->node, -b, h);
    wlr_scene_rect_set_size(border[2], b, h); // left
    wlr_scene_node_set_position(&border[2]->node, -b, 0);
    wlr_scene_rect_set_size(border[3], b, h); // right
    wlr_scene_node_set_position(&border[3]->node, w, 0);
}

void
Toplevel::setFocused(bool focused)
{
    for (auto *r : border)
        wlr_scene_rect_set_color(r, focused
                                        ? wm->config().border.focused_color
                                        : wm->config().border.unfocused_color);
}
