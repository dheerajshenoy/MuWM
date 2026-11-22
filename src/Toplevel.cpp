#include "Toplevel.hpp"

#include "MuWM.hpp"

void
Toplevel::onMap(void *)
{
    wlr_log(WLR_INFO, "Window mapped: %s",
            xdg->title ? xdg->title : "(untitled)");
    wm->focusToplevel(this);
}

void
Toplevel::onCommit(void *)
{
    if (xdg->base->initial_commit)
        wlr_xdg_toplevel_set_size(xdg, 0, 0); // 0,0 = "client picks its size"
}

void
Toplevel::onDestroy(void *)
{
    wm->forgetToplevel(this);
    wl_list_remove(&map.listener.link);
    wl_list_remove(&commit.listener.link);
    wl_list_remove(&destroy.listener.link);
    delete this;
}
