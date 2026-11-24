#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

class MuWM;

struct IdleInhibitor
{
    MuWM *wm                         = nullptr;
    wlr_idle_inhibitor_v1 *inhibitor = nullptr;
    Listener<IdleInhibitor> destroy;
    void onDestroy(void *data);
};
