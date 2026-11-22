#include "MuWM.hpp"

#include "Keyboard.hpp"
#include "Output.hpp"
#include "Toplevel.hpp"

#include <csignal>
#include <unistd.h>

MuWM::MuWM(const argparse::ArgumentParser &parser) : m_parser(parser)
{
    m_ok = init();
}

MuWM::~MuWM()
{
    if (m_display)
        wl_display_destroy_clients(m_display);

    m_newToplevel.unbind();
    m_newInput.unbind();

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

    // Advertises wl_shm (and dmabuf if supported); clients like foot need it.
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
    if (!wlr_backend_start(m_backend))
        return false;

    setenv("WAYLAND_DISPLAY", socket,
           1);                // must happen before spawning children
    signal(SIGCHLD, SIG_IGN); // don't leave zombie processes behind
    wlr_log(WLR_INFO, "Running on WAYLAND_DISPLAY=%s", socket);

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
    t->map.bind<&Toplevel::onMap>(&xdg->base->surface->events.map, t);
    t->commit.bind<&Toplevel::onCommit>(&xdg->base->surface->events.commit, t);
    t->destroy.bind<&Toplevel::onDestroy>(&xdg->events.destroy, t);
    t->tree->node.data = t; // new line, after tree is created
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
    xkb_keymap *km
        = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(kb, km);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    wlr_keyboard_set_repeat_info(kb, 25, 600);

    // Make the seat aware of a keyboard immediately, so the first window to
    // map can receive keyboard focus before any key has been pressed.
    wlr_seat_set_keyboard(m_seat, kb);

    // If a window mapped before any keyboard existed, give it focus now.
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
MuWM::handleKeybinding(xkb_keysym_t sym)
{
    switch (sym)
    {
        case XKB_KEY_q:
            wl_display_terminate(m_display);
            return true;
        default:
            return false;
    }
}

void
MuWM::focusToplevel(Toplevel *t)
{
    if (m_focused == t)
        return;
    if (m_focused)
        wlr_xdg_toplevel_set_activated(m_focused->xdg, false);
    m_focused = t;
    if (!t)
        return;

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
    std::erase(m_toplevels, t); // no-op if already removed (unmap then destroy)
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
        wlr_scene_node_set_position(&t->tree->node, x,
                                    y); // where it is: our decision
        wlr_xdg_toplevel_set_size(t->xdg, w,
                                  h); // how big: a request to the client
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

    // walk up the tree until we find the node tagged with a Toplevel
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
    // the relative_direction argument exists in 0.18+; remove it if the
    // compiler complains
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
        setsid(); // detach from our process group
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(1); // only reached if exec failed
    }
}

void
MuWM::onLayoutChange(void *)
{
    arrange(); // output geometry changed: re-tile
}
