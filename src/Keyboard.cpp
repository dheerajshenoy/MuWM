#include "Keyboard.hpp"

#include "MuWM.hpp"

namespace
{
constexpr uint32_t kBindMask = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL
                               | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
} // namespace

void
Keyboard::onKey(void *data)
{
    auto *ev                    = static_cast<wlr_keyboard_key_event *>(data);
    const xkb_keycode_t keycode = ev->keycode + 8;

    // if we consumed the press, swallow the release too
    if (ev->state == WL_KEYBOARD_KEY_STATE_RELEASED
        && consumed.erase(ev->keycode))
        return;

    bool handled = false;
    if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED)
    {
        const uint32_t mods
            = wlr_keyboard_get_modifiers(kb) & kBindMask; // ignore Caps/NumLock

        const xkb_keysym_t *syms;
        int n = xkb_state_key_get_syms(kb->xkb_state, keycode, &syms);
        for (int i = 0; i < n && !handled; i++)
            handled = wm->handleKeybinding(mods, syms[i]);

        if (!handled)
        { // also try the unshifted symbol, so "Alt+Shift+1" works
            xkb_layout_index_t layout
                = xkb_state_key_get_layout(kb->xkb_state, keycode);
            const xkb_keysym_t *raw;
            int rn = xkb_keymap_key_get_syms_by_level(kb->keymap, keycode,
                                                      layout, 0, &raw);
            for (int i = 0; i < rn && !handled; i++)
                handled = wm->handleKeybinding(mods, raw[i]);
        }
        if (handled)
            consumed.insert(ev->keycode);
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
