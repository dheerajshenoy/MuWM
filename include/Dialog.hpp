#pragma once

#include "Listener.hpp"
#include "wlroots.hpp"

class MuWM;

struct Dialog
{
    MuWM *wm = nullptr;
    Listener<Dialog> setModal;
    Listener<Dialog> destroy;

    void onSetModal(void *);
    void onDestroy(void *);
};
