#include "Keyboard.hpp"

#include "MuWM.hpp"

void
Keyboard::onKey(void *data)
{
    auto *ev = static_cast<wlr_keyboard_key_event *>(data);

    // libinput keycodes are offset by 8 relative to xkb keycodes
    const xkb_keysym_t *syms;
    int n = xkb_state_key_get_syms(kb->xkb_state, ev->keycode + 8, &syms);

    bool handled = false;
    if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED
        && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_ALT))
    {
        for (int i = 0; i < n; i++)
            handled = wm->handleKeybinding(syms[i]) || handled;
    }

    if (!handled)
    {
        wlr_seat_set_keyboard(wm->seat(), kb);
        wlr_seat_keyboard_notify_key(wm->seat(), ev->time_msec, ev->keycode,
                                     ev->state);
    }
}

void
Keyboard::onModifiers(void *)
{
    wlr_seat_set_keyboard(wm->seat(), kb);
    wlr_seat_keyboard_notify_modifiers(wm->seat(), &kb->modifiers);
}

void
Keyboard::onDestroy(void *)
{
    wl_list_remove(&key.listener.link);
    wl_list_remove(&modifiers.listener.link);
    wl_list_remove(&destroy.listener.link);
    delete this;
}
