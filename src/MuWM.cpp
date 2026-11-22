#include "MuWM.hpp"

#include "Decoration.hpp"
#include "Keyboard.hpp"
#include "Output.hpp"
#include "Toplevel.hpp"

#include <csignal>
#include <unistd.h>

MuWM::MuWM(const argparse::ArgumentParser &parser) : m_parser(parser)
{
    initLua();
    m_ok = init();
}

MuWM::~MuWM()
{
    if (m_applyIdle)
        wl_event_source_remove(m_applyIdle);

    if (m_display)
        wl_display_destroy_clients(m_display);

    m_newToplevel.unbind();
    m_newInput.unbind();
    m_newDecoration.unbind();
    m_layoutChange.unbind();
    // wlroots asserts that a cursor has no listeners left when destroyed
    m_cursorMotion.unbind();
    m_cursorMotionAbs.unbind();
    m_cursorButton.unbind();
    m_cursorAxis.unbind();
    m_cursorFrame.unbind();

    if (m_outputListener.listener.link.prev)
        wl_list_remove(&m_outputListener.listener.link);

    if (m_scene)
        wlr_scene_node_destroy(&m_scene->tree.node);

    if (m_xcursor)
        wlr_xcursor_manager_destroy(m_xcursor);

    if (m_cursor)
        wlr_cursor_destroy(m_cursor);

    if (m_allocator)
    {
        wlr_allocator_destroy(m_allocator);
    }

    if (m_renderer)
    {
        wlr_renderer_destroy(m_renderer);
    }

    if (m_backend)
    {
        wlr_backend_destroy(m_backend);
    }

    if (m_display)
    {
        wl_display_destroy(m_display);
    }

    if (m_lua)
    {
        lua_close(m_lua);
    }
}

bool
MuWM::init()
{
    m_display = wl_display_create();
    if (!m_display)
    {
        wlr_log(WLR_ERROR, "Failed to create display");
        return false;
    }

    for (int sig : {SIGINT, SIGTERM})
        wl_event_loop_add_signal(wl_display_get_event_loop(m_display), sig,
                                 [](int, void *d)
        {
            wl_display_terminate(static_cast<wl_display *>(d));
            return 0;
        }, m_display);

    m_backend
        = wlr_backend_autocreate(wl_display_get_event_loop(m_display), nullptr);
    if (!m_backend)
    {
        wlr_log(WLR_ERROR, "Failed to create backend");
        return false;
    }

    m_renderer = wlr_renderer_autocreate(m_backend);
    if (!m_renderer)
    {
        wlr_log(WLR_ERROR, "Failed to create renderer");
        return false;
    }

    if (!wlr_renderer_init_wl_display(m_renderer, m_display))
    {
        wlr_log(WLR_ERROR, "Failed to init renderer wl_display globals");
        return false;
    }

    m_allocator = wlr_allocator_autocreate(m_backend, m_renderer);
    if (!m_allocator)
    {
        wlr_log(WLR_ERROR, "Failed to create allocator");
        return false;
    }

    m_layout = wlr_output_layout_create(m_display);
    if (!m_layout)
    {
        wlr_log(WLR_ERROR, "Failed to create output layout");
        return false;
    }

    m_layoutChange.bind<&MuWM::onLayoutChange>(&m_layout->events.change, this);

    m_scene = wlr_scene_create();
    if (!m_scene)
    {
        wlr_log(WLR_ERROR, "Failed to create scene");
        return false;
    }

    m_sceneLayout = wlr_scene_attach_output_layout(m_scene, m_layout);

    wlr_compositor_create(m_display, 5, m_renderer);
    wlr_subcompositor_create(m_display);
    wlr_data_device_manager_create(m_display);

    m_xdgShell = wlr_xdg_shell_create(m_display, 6);
    m_newToplevel.bind<&MuWM::onNewToplevel>(&m_xdgShell->events.new_toplevel,
                                             this);

    m_seat = wlr_seat_create(m_display, "seat0");
    wlr_seat_set_capabilities(m_seat, WL_SEAT_CAPABILITY_POINTER
                                          | WL_SEAT_CAPABILITY_KEYBOARD
                                          | WL_SEAT_CAPABILITY_TOUCH);
    m_cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(m_cursor, m_layout);
    m_xcursor = wlr_xcursor_manager_create(nullptr, 24);

    m_cursorMotion.bind<&MuWM::onCursorMotion>(&m_cursor->events.motion, this);
    m_cursorMotionAbs.bind<&MuWM::onCursorMotionAbs>(
        &m_cursor->events.motion_absolute, this);
    m_cursorButton.bind<&MuWM::onCursorButton>(&m_cursor->events.button, this);
    m_cursorAxis.bind<&MuWM::onCursorAxis>(&m_cursor->events.axis, this);
    m_cursorFrame.bind<&MuWM::onCursorFrame>(&m_cursor->events.frame, this);
    m_newInput.bind<&MuWM::onNewInput>(&m_backend->events.new_input, this);

    m_outputListener.wm = this;

    m_outputListener.listener.notify = [](wl_listener *l, void *data)
    {
        auto *ol = reinterpret_cast<OutputListener *>(l);
        ol->wm->handleNewOutput(static_cast<wlr_output *>(data));
    };
    wl_signal_add(&m_backend->events.new_output, &m_outputListener.listener);

    m_decoManager = wlr_xdg_decoration_manager_v1_create(m_display);
    m_newDecoration.bind<&MuWM::onNewDecoration>(
        &m_decoManager->events.new_toplevel_decoration, this);

    return true;
}

void
MuWM::handleNewOutput(wlr_output *output)
{
    wlr_output_init_render(output, m_allocator, m_renderer);

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    if (wlr_output_mode *mode = wlr_output_preferred_mode(output))
        wlr_output_state_set_mode(&state, mode);
    wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);

    auto *o            = new Output{this, output};
    o->sceneOutput     = wlr_scene_output_create(m_scene, output);
    auto *layoutOutput = wlr_output_layout_add_auto(m_layout, output);
    wlr_scene_output_layout_add_output(m_sceneLayout, layoutOutput,
                                       o->sceneOutput);
    o->frame.bind<&Output::onFrame>(&output->events.frame, o);
    o->destroy.bind<&Output::onDestroy>(&output->events.destroy, o);
    o->requestState.bind<&Output::onRequestState>(&output->events.request_state,
                                                  o);

    wlr_log(WLR_INFO, "New output: %s", output->name);
}

bool
MuWM::run(const std::string &startupCmd)
{
    const char *socket = wl_display_add_socket_auto(m_display);
    if (!socket)
        return false;

    setenv("WAYLAND_DISPLAY", socket, 1);
    signal(SIGCHLD, SIG_IGN);
    wlr_log(WLR_INFO, "Running on WAYLAND_DISPLAY=%s", socket);

    // The config must be loaded before the backend starts: starting it
    // announces the existing keyboards, whose keymap is built from the config.
    // The socket already exists, so clients spawned by the config can connect.
    loadConfig();

    if (!wlr_backend_start(m_backend))
        return false;

    if (!startupCmd.empty())
        spawn(startupCmd);

    wl_display_run(m_display);
    return true;
}

void
MuWM::onNewToplevel(void *data)
{
    handleNewToplevel(static_cast<wlr_xdg_toplevel *>(data));
}

void
MuWM::handleNewToplevel(wlr_xdg_toplevel *xdg)
{
    auto *t = new Toplevel{this, xdg};
    t->tree = wlr_scene_xdg_surface_create(&m_scene->tree, xdg->base);

    for (auto &r : t->border)
        r = wlr_scene_rect_create(t->tree, 1, 1,
                                  m_config.border.unfocused_color);

    t->map.bind<&Toplevel::onMap>(&xdg->base->surface->events.map, t);
    t->commit.bind<&Toplevel::onCommit>(&xdg->base->surface->events.commit, t);
    t->destroy.bind<&Toplevel::onDestroy>(&xdg->events.destroy, t);
    t->tree->node.data = t;
    t->unmap.bind<&Toplevel::onUnmap>(&xdg->base->surface->events.unmap, t);
}

void
MuWM::onNewInput(void *data)
{
    handleNewInput(static_cast<wlr_input_device *>(data));
}

void
MuWM::handleNewInput(wlr_input_device *device)
{
    if (device->type == WLR_INPUT_DEVICE_POINTER)
    {
        wlr_cursor_attach_input_device(m_cursor, device);
        return;
    }
    if (device->type != WLR_INPUT_DEVICE_KEYBOARD)
        return;

    wlr_keyboard *kb = wlr_keyboard_from_input_device(device);

    xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    const auto &kc = m_config.keyboard;
    auto opt       = [](const std::string &s)
    {
        return s.empty() ? nullptr : s.c_str();
    };

    xkb_rule_names names = {};
    names.rules          = opt(kc.rules);
    names.model          = opt(kc.model);
    names.layout         = opt(kc.layout);
    names.variant        = opt(kc.variant);
    names.options        = opt(kc.xkb_options);

    xkb_keymap *km
        = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km)
    {
        wlr_log(WLR_ERROR,
                "invalid keyboard layout/variant; using system defaults");
        km = xkb_keymap_new_from_names(ctx, nullptr,
                                       XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    wlr_log(WLR_DEBUG, "keyboard %s: layout=%s variant=%s options=%s",
            device->name, kc.layout.c_str(), kc.variant.c_str(),
            kc.xkb_options.c_str());
    wlr_keyboard_set_keymap(kb, km);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    wlr_keyboard_set_repeat_info(
        kb, kc.repeat_rate, kc.repeat_delay); // replaces the hardcoded 25, 600

    wlr_seat_set_keyboard(m_seat, kb);

    if (m_focused && !m_seat->keyboard_state.focused_surface)
        wlr_seat_keyboard_notify_enter(m_seat, m_focused->xdg->base->surface,
                                       kb->keycodes, kb->num_keycodes,
                                       &kb->modifiers);

    auto *k = new Keyboard{this, kb};
    k->key.bind<&Keyboard::onKey>(&kb->events.key, k);
    k->modifiers.bind<&Keyboard::onModifiers>(&kb->events.modifiers, k);
    k->destroy.bind<&Keyboard::onDestroy>(&kb->base.events.destroy, k);
}

bool
MuWM::handleKeybinding(uint32_t mods, xkb_keysym_t sym)
{
    sym = xkb_keysym_to_lower(sym);
    for (const Binding &b : m_keybindings)
    {
        if (b.mods != mods || b.sym != sym)
            continue;

        lua_rawgeti(m_lua, LUA_REGISTRYINDEX,
                    b.ref); // read b.ref *before* calling:
        if (lua_pcall(m_lua, 0, 0, 0) != LUA_OK)
        { // a callback may add bindings
            wlr_log(WLR_ERROR, "keybinding: %s", lua_tostring(m_lua, -1));
            lua_pop(m_lua, 1);
        }
        return true;
    }
    return false;
}

void
MuWM::focusToplevel(Toplevel *t)
{
    if (m_focused == t)
        return;
    if (m_focused)
    {
        wlr_xdg_toplevel_set_activated(m_focused->xdg, false);
        m_focused->setFocused(false);
    }
    m_focused = t;
    if (!t)
        return;

    t->setFocused(true);
    wlr_scene_node_raise_to_top(&t->tree->node);
    wlr_xdg_toplevel_set_activated(t->xdg, true);
    if (wlr_keyboard *kb = wlr_seat_get_keyboard(m_seat))
        wlr_seat_keyboard_notify_enter(m_seat, t->xdg->base->surface,
                                       kb->keycodes, kb->num_keycodes,
                                       &kb->modifiers);
}

void
MuWM::addToplevel(Toplevel *t)
{
    if (std::find(m_toplevels.begin(), m_toplevels.end(), t)
        == m_toplevels.end())
        m_toplevels.push_back(t);
    arrange();
    focusToplevel(t);
}

void
MuWM::removeToplevel(Toplevel *t)
{
    std::erase(m_toplevels, t);
    if (m_focused == t)
        m_focused = nullptr;
    arrange();
    if (!m_focused && !m_toplevels.empty())
        focusToplevel(m_toplevels.back());
}

void
MuWM::arrange()
{
    wlr_box area;
    wlr_output_layout_get_box(m_layout, nullptr,
                              &area); // nullptr = bounding box of all outputs
    if (area.width <= 0 || m_toplevels.empty())
        return;

    int gap = m_config.gap;
    area.x += gap;
    area.y += gap;
    area.width -= 2 * gap;
    area.height -= 2 * gap;

    auto place = [](Toplevel *t, int x, int y, int w, int h)
    {
        wlr_scene_node_set_position(&t->tree->node, x, y);
        wlr_xdg_toplevel_set_size(t->xdg, w, h);
        t->resizeBorders(w, h);
    };

    const int n = static_cast<int>(m_toplevels.size());
    if (n == 1)
    {
        place(m_toplevels[0], area.x, area.y, area.width, area.height);
        return;
    }

    const int masterW = (area.width - gap) * 55 / 100;
    const int stackW  = area.width - gap - masterW;
    place(m_toplevels[0], area.x, area.y, masterW, area.height);

    const int stackN = n - 1;
    const int h      = (area.height - gap * (stackN - 1)) / stackN;
    for (int i = 1; i < n; i++)
        place(m_toplevels[i], area.x + masterW + gap,
              area.y + (i - 1) * (h + gap), stackW, h);
}

Toplevel *
MuWM::toplevelAt(double x, double y, wlr_surface **surface, double *sx,
                 double *sy)
{
    wlr_scene_node *node = wlr_scene_node_at(&m_scene->tree.node, x, y, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER)
        return nullptr;

    wlr_scene_buffer *buf = wlr_scene_buffer_from_node(node);
    wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buf);
    if (!ss)
        return nullptr;
    *surface = ss->surface;

    wlr_scene_tree *tree = node->parent;
    while (tree && !tree->node.data)
        tree = tree->node.parent;
    return tree ? static_cast<Toplevel *>(tree->node.data) : nullptr;
}

void
MuWM::processMotion(uint32_t timeMsec)
{
    wlr_surface *surface = nullptr;
    double sx = 0, sy = 0;
    Toplevel *t = toplevelAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy);

    if (!t)
    {
        wlr_cursor_set_xcursor(m_cursor, m_xcursor, "default");
        wlr_seat_pointer_clear_focus(m_seat);
        return;
    }
    wlr_seat_pointer_notify_enter(m_seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(m_seat, timeMsec, sx, sy);
}

void
MuWM::onCursorMotion(void *data)
{
    auto *ev = static_cast<wlr_pointer_motion_event *>(data);
    wlr_cursor_move(m_cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
    processMotion(ev->time_msec);
}

void
MuWM::onCursorMotionAbs(void *data)
{
    auto *ev = static_cast<wlr_pointer_motion_absolute_event *>(data);
    wlr_cursor_warp_absolute(m_cursor, &ev->pointer->base, ev->x, ev->y);
    processMotion(ev->time_msec);
}

void
MuWM::onCursorButton(void *data)
{
    auto *ev = static_cast<wlr_pointer_button_event *>(data);
    wlr_seat_pointer_notify_button(m_seat, ev->time_msec, ev->button,
                                   ev->state);

    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED)
    {
        wlr_surface *surface = nullptr;
        double sx, sy;
        if (Toplevel *t
            = toplevelAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy))
            focusToplevel(t);
    }
}

void
MuWM::onCursorAxis(void *data)
{
    auto *ev = static_cast<wlr_pointer_axis_event *>(data);
    wlr_seat_pointer_notify_axis(m_seat, ev->time_msec, ev->orientation,
                                 ev->delta, ev->delta_discrete, ev->source,
                                 ev->relative_direction);
}

void
MuWM::onCursorFrame(void *)
{
    wlr_seat_pointer_notify_frame(m_seat);
}

void
MuWM::spawn(const std::string &cmd)
{
    if (fork() == 0)
    {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(1);
    }
}

void
MuWM::onLayoutChange(void *)
{
    arrange();
}

void
MuWM::onNewDecoration(void *data)
{
    auto *d
        = new Decoration{static_cast<wlr_xdg_toplevel_decoration_v1 *>(data)};
    d->requestMode.bind<&Decoration::onRequestMode>(
        &d->deco->events.request_mode, d);
    d->commit.bind<&Decoration::onCommit>(
        &d->deco->toplevel->base->surface->events.commit, d);
    d->destroy.bind<&Decoration::onDestroy>(&d->deco->events.destroy, d);
    d->onRequestMode(nullptr);
}

void
MuWM::scheduleApply(int changes)
{
    m_pendingChanges |= changes;
    if (m_applyIdle || !m_display)
        return;

    // Deferred so several writes (e.g. a whole-table assignment) cause one
    // relayout, and so it never runs in the middle of a Lua call.
    m_applyIdle = wl_event_loop_add_idle(wl_display_get_event_loop(m_display),
                                         [](void *data)
    {
        auto *wm        = static_cast<MuWM *>(data);
        wm->m_applyIdle = nullptr; // idle sources are freed once they fire
        wm->applyPending();
    }, this);
}

void
MuWM::applyPending()
{
    const int changes = m_pendingChanges;
    m_pendingChanges  = ChangeNone;
    wlr_log(WLR_DEBUG, "Applying config changes (flags 0x%x)", changes);

    if (changes & ChangeLayout)
        arrange();
    if (changes & ChangeRecolor)
        recolorBorders();
}

void
MuWM::recolorBorders()
{
    for (Toplevel *t : m_toplevels)
        t->setFocused(t == m_focused);
}

void
MuWM::addBinding(Binding b)
{
    for (Binding &existing : m_keybindings)
        if (existing.mods == b.mods && existing.sym == b.sym)
        {
            luaL_unref(m_lua, LUA_REGISTRYINDEX, existing.ref);
            wlr_log(WLR_INFO, "Replacing keybinding for sym 0x%x", b.sym);
            existing.ref = b.ref;
            return;
        }
    m_keybindings.push_back(b);
}

void
MuWM::clearBindings()
{
    wlr_log(WLR_DEBUG, "Clearing %zu keybindings", m_keybindings.size());
    for (const Binding &b : m_keybindings)
        luaL_unref(m_lua, LUA_REGISTRYINDEX, b.ref);
    m_keybindings.clear();
}

void
MuWM::quit()
{
    wl_display_terminate(m_display);
}

void
MuWM::focusNext()
{
    for (size_t i = 0; i < m_toplevels.size(); i++)
    {
        if (m_toplevels[i] == m_focused)
        {
            focusToplevel(m_toplevels[(i + 1) % m_toplevels.size()]);
            return;
        }
    }
}

void
MuWM::focusPrev()
{
    for (size_t i = 0; i < m_toplevels.size(); i++)
    {
        if (m_toplevels[i] == m_focused)
        {
            focusToplevel(
                m_toplevels[(i + m_toplevels.size() - 1) % m_toplevels.size()]);
            return;
        }
    }
}

void
MuWM::swapNext()
{
    for (size_t i = 0; i < m_toplevels.size(); i++)
    {
        if (m_toplevels[i] == m_focused)
        {
            std::swap(m_toplevels[i],
                      m_toplevels[(i + 1) % m_toplevels.size()]);
            arrange();
            return;
        }
    }
}

void
MuWM::swapPrev()
{
    for (size_t i = 0; i < m_toplevels.size(); i++)
    {
        if (m_toplevels[i] == m_focused)
        {
            std::swap(
                m_toplevels[i],
                m_toplevels[(i + m_toplevels.size() - 1) % m_toplevels.size()]);
            arrange();
            return;
        }
    }
}

void
MuWM::close()
{
    if (m_focused)
        wlr_xdg_toplevel_send_close(m_focused->xdg);
}
