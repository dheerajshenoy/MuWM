#pragma once

#include "wlroots.hpp"

template <typename T> struct Listener
{
    wl_listener listener{}; // must stay first
    T *owner = nullptr;

    template <auto Handler> void bind(wl_signal *signal, T *obj)
    {
        owner           = obj;
        listener.notify = [](wl_listener *l, void *data)
        {
            auto *self = reinterpret_cast<Listener *>(l);
            (self->owner->*Handler)(data);
        };
        wl_signal_add(signal, &listener);
    }
};
