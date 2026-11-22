#pragma once

#include "Listener.hpp"

struct Decoration
{
    wlr_xdg_toplevel_decoration_v1 *deco = nullptr;
    Listener<Decoration> requestMode;
    Listener<Decoration> commit;
    Listener<Decoration> destroy;

    void onRequestMode(void *data);
    void onCommit(void *data);
    void onDestroy(void *data);
};
