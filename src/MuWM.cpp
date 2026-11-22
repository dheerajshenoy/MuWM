#include "MuWM.hpp"

#include "Decoration.hpp"
#include "Keyboard.hpp"
#include "LayerShellSurface.hpp"
#include "Output.hpp"
#include "Pointer.hpp"
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
    m_newLayerShellSurface.unbind();
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

    // Destroying the scene frees every workspace tree; Output::onDestroy runs
    // later (when the backend is torn down) and must not free them again.
    for (Output *output : m_outputs)
        for (Workspace *ws : output->workspaces)
            ws->sceneTree = nullptr;

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

    // Handlers hold Lua registry refs, so they must go before the state does.
    m_eventBus.clear();

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

    // Children stack in creation order, so this fixes the layer ordering.
    m_layerTrees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]
        = wlr_scene_tree_create(&m_scene->tree);
    m_layerTrees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM]
        = wlr_scene_tree_create(&m_scene->tree);
    m_windowTree = wlr_scene_tree_create(&m_scene->tree);
    m_layerTrees[ZWLR_LAYER_SHELL_V1_LAYER_TOP]
        = wlr_scene_tree_create(&m_scene->tree);
    m_layerTrees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]
        = wlr_scene_tree_create(&m_scene->tree);

    wlr_compositor_create(m_display, 5, m_renderer);
    wlr_subcompositor_create(m_display);
    wlr_xdg_output_manager_v1_create(m_display, m_layout); // bars need it
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

    m_layerShell = wlr_layer_shell_v1_create(m_display, 4);

    m_newLayerShellSurface.bind<&MuWM::onNewLayerShellSurface>(
        &m_layerShell->events.new_surface, this);

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
    output->data       = o;
    auto *layoutOutput = wlr_output_layout_add_auto(m_layout, output);
    wlr_scene_output_layout_add_output(m_sceneLayout, layoutOutput,
                                       o->sceneOutput);
    o->frame.bind<&Output::onFrame>(&output->events.frame, o);
    o->destroy.bind<&Output::onDestroy>(&output->events.destroy, o);
    o->requestState.bind<&Output::onRequestState>(&output->events.request_state,
                                                  o);
    for (int i = 1; i <= 9; i++)
    {
        Workspace *w = new Workspace{};
        w->name      = std::to_string(i);
        auto *tree   = wlr_scene_tree_create(m_windowTree);
        wlr_scene_node_set_enabled(
            &tree->node, i == 1); // only the first workspace is visible
        w->sceneTree = tree;
        w->output    = o;
        o->workspaces.push_back(w);
    }

    o->workspace = o->workspaces[0];
    m_outputs.push_back(o);
    if (!m_focusedOutput)
        m_focusedOutput = o;

    // adopt windows that mapped too early
    if (m_outputs.size() == 1)
    {
        for (Toplevel *t : m_unassigned.toplevels)
        {
            t->workspace = o->workspace;
            wlr_scene_node_reparent(&t->tree->node, t->workspace->sceneTree);
        }
        o->workspace->toplevels       = std::move(m_unassigned.toplevels);
        o->workspace->focusedToplevel = m_unassigned.focusedToplevel;
        m_unassigned                  = {};
    }

    arrangeLayers(output);
    arrange(o->workspace);

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

    m_eventBus.emit(EventType::Startup);

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
    t->tree = wlr_scene_xdg_surface_create(m_windowTree, xdg->base);
    t->id   = m_globalTopLevelId++;

    for (auto &r : t->border)
        r = wlr_scene_rect_create(t->tree, 1, 1,
                                  m_config.border.unfocused_color);

    t->map.bind<&Toplevel::onMap>(&xdg->base->surface->events.map, t);
    t->commit.bind<&Toplevel::onCommit>(&xdg->base->surface->events.commit, t);
    t->destroy.bind<&Toplevel::onDestroy>(&xdg->events.destroy, t);
    t->tree->node.data = t;
    t->unmap.bind<&Toplevel::onUnmap>(&xdg->base->surface->events.unmap, t);
    t->requestFullscreen.bind<&Toplevel::onRequestFullscreen>(
        &xdg->events.request_fullscreen, t);
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

        auto *p = new Pointer{this, device};
        p->destroy.bind<&Pointer::onDestroy>(&device->events.destroy, p);
        m_pointers.push_back(p);
        applyPointerConfig(p);
        return;
    }
    if (device->type != WLR_INPUT_DEVICE_KEYBOARD)
        return;

    wlr_keyboard *kb = wlr_keyboard_from_input_device(device);

    auto *k = new Keyboard{this, kb};
    k->key.bind<&Keyboard::onKey>(&kb->events.key, k);
    k->modifiers.bind<&Keyboard::onModifiers>(&kb->events.modifiers, k);
    k->destroy.bind<&Keyboard::onDestroy>(&kb->base.events.destroy, k);
    m_keyboards.push_back(k);
    applyKeyboardConfig(k);

    wlr_seat_set_keyboard(m_seat, kb);

    Toplevel *t = focusedToplevel();
    if (t && !m_seat->keyboard_state.focused_surface)
        wlr_seat_keyboard_notify_enter(m_seat, t->xdg->base->surface,
                                       kb->keycodes, kb->num_keycodes,
                                       &kb->modifiers);
}

void
MuWM::applyKeyboardConfig(Keyboard *keyboard)
{
    wlr_keyboard *kb = keyboard->kb;
    const auto &kc   = m_config.keyboard;

    // Empty strings mean "use the xkb default".
    auto opt = [](const std::string &s)
    {
        return s.empty() ? nullptr : s.c_str();
    };
    xkb_rule_names names = {};
    names.rules          = opt(kc.rules);
    names.model          = opt(kc.model);
    names.layout         = opt(kc.layout);
    names.variant        = opt(kc.variant);
    names.options        = opt(kc.xkb_options);

    xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap *km
        = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    bool configured = km != nullptr;
    if (!km && !kb->keymap)
    {
        // A new keyboard needs some keymap; an existing one keeps its old one.
        wlr_log(WLR_ERROR,
                "invalid keyboard layout/variant; using system defaults");
        km = xkb_keymap_new_from_names(ctx, nullptr,
                                       XKB_KEYMAP_COMPILE_NO_FLAGS);
    }

    if (km)
    {
        if (configured)
            wlr_log(WLR_DEBUG, "keyboard %s: layout=%s variant=%s options=%s",
                    kb->base.name, kc.layout.c_str(), kc.variant.c_str(),
                    kc.xkb_options.c_str());
        wlr_keyboard_set_keymap(kb, km);
        xkb_keymap_unref(km);
    }
    else
    {
        wlr_log(WLR_ERROR,
                "invalid keyboard layout/variant; keeping the "
                "current keymap for %s",
                kb->base.name);
    }
    xkb_context_unref(ctx);

    wlr_keyboard_set_repeat_info(kb, kc.repeat_rate, kc.repeat_delay);
}

void
MuWM::applyPointerConfig(Pointer *pointer)
{
    // Only libinput devices have these settings (not the nested or headless
    // backends).
    if (!wlr_input_device_is_libinput(pointer->device))
        return;
    libinput_device *dev = wlr_libinput_get_device_handle(pointer->device);

    // Always write the full state: turning an option off must take effect too.
    if (libinput_device_config_scroll_has_natural_scroll(dev))
        libinput_device_config_scroll_set_natural_scroll_enabled(
            dev, m_config.touchpad.natural_scroll);

    if (libinput_device_config_tap_get_finger_count(dev) > 0)
        libinput_device_config_tap_set_enabled(
            dev, m_config.touchpad.tap_to_click ? LIBINPUT_CONFIG_TAP_ENABLED
                                                : LIBINPUT_CONFIG_TAP_DISABLED);
}

// Re-applies the settings to every existing device of the affected kinds.
void
MuWM::applyDeviceConfig(int changes)
{
    if (changes & ChangeKeyboard)
        for (Keyboard *k : m_keyboards)
            applyKeyboardConfig(k);
    if (changes & ChangePointer)
        for (Pointer *p : m_pointers)
            applyPointerConfig(p);
}

void
MuWM::removeKeyboard(Keyboard *keyboard)
{
    std::erase(m_keyboards, keyboard);
}

void
MuWM::removePointer(Pointer *pointer)
{
    std::erase(m_pointers, pointer);
}

// Built-in bindings that exist even with no config. A config can rebind them
// (same key), and they are put back if the config fails to load.
void
MuWM::installDefaultBindings()
{
    constexpr uint32_t mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT;

    Binding quitKey;
    quitKey.mods   = mods;
    quitKey.sym    = XKB_KEY_c;
    quitKey.native = [this]
    {
        quit();
    };
    addBinding(std::move(quitKey));

    // $TERMINAL may include arguments, hence the unquoted expansion. Without
    // it, try a few common terminals.
    Binding terminalKey;
    terminalKey.mods   = mods;
    terminalKey.sym    = XKB_KEY_Return;
    terminalKey.native = [this]
    {
        spawn("if [ -n \"$TERMINAL\" ]; then exec $TERMINAL; fi; "
              "for t in foot kitty alacritty xterm; do "
              "command -v \"$t\" >/dev/null 2>&1 && exec \"$t\"; done");
    };
    addBinding(std::move(terminalKey));
}

void
MuWM::failConfig()
{
    // A half-run config may have rebound the defaults; make sure a way to
    // quit and to open a terminal exists.
    installDefaultBindings();
    wlr_log(WLR_ERROR, "Config failed to load; default keybindings restored "
                       "(Super+Shift+C quits)");
}

bool
MuWM::handleKeybinding(uint32_t mods, xkb_keysym_t sym)
{
    sym = xkb_keysym_to_lower(sym);

    for (const Binding &b : m_keybindings)
    {
        if (b.mods != mods || b.sym != sym)
            continue;

        if (b.native)
        {
            const auto action = b.native; // the action may change the bindings
            action();
            return true;
        }

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

Workspace *
MuWM::currentWorkspace()
{
    return m_focusedOutput ? m_focusedOutput->workspace : &m_unassigned;
}

void
MuWM::focusToplevel(Toplevel *t)
{
    Toplevel *oldFocus = focusedToplevel();

    if (oldFocus == t)
        return;
    if (oldFocus)
    {
        wlr_xdg_toplevel_set_activated(oldFocus->xdg, false);
        oldFocus->setFocused(false);
    }
    if (!t)
    {
        currentWorkspace()->focusedToplevel = nullptr;
        return;
    }

    Workspace *ws = t->workspace;
    if (ws->output)
        m_focusedOutput = ws->output;
    ws->focusedToplevel = t;

    // Focusing another window would hide it behind the fullscreen one.
    for (Toplevel *other : std::vector<Toplevel *>(ws->toplevels))
        if (other != t && other->fullscreen)
            setFullscreen(other, false);

    wlr_scene_node_raise_to_top(&t->tree->node);
    if (m_focusedLayer)
        return; // the window is focused again when the layer surface lets go

    t->setFocused(true);
    wlr_xdg_toplevel_set_activated(t->xdg, true);
    if (wlr_keyboard *kb = wlr_seat_get_keyboard(m_seat))
        wlr_seat_keyboard_notify_enter(m_seat, t->xdg->base->surface,
                                       kb->keycodes, kb->num_keycodes,
                                       &kb->modifiers);
}

WindowInfo
MuWM::windowInfo(const Toplevel *t) const
{
    WindowInfo info;
    info.id     = t->id;
    info.title  = t->xdg->title ? t->xdg->title : "";
    info.appId  = t->xdg->app_id ? t->xdg->app_id : "";
    info.width  = t->xdg->base->surface->current.width;
    info.height = t->xdg->base->surface->current.height;
    if (const Workspace *ws = t->workspace)
    {
        info.workspace = ws->name;
        if (ws->output)
            info.output = ws->output->output->name;
        info.focused = ws->focusedToplevel == t;
    }
    return info;
}

void
MuWM::addToplevel(Toplevel *t)
{
    if (!t->workspace)
    {
        t->workspace = currentWorkspace();
        // m_unassigned has no tree of its own; leave the window where it is
        if (t->workspace->sceneTree)
            wlr_scene_node_reparent(&t->tree->node, t->workspace->sceneTree);
    }

    auto &toplevels = t->workspace->toplevels;
    if (std::find(toplevels.begin(), toplevels.end(), t) == toplevels.end())
        toplevels.push_back(t);

    // A new window leaves any fullscreen window behind it, so lay out first.
    for (Toplevel *other : std::vector<Toplevel *>(toplevels))
        if (other != t && other->fullscreen)
            setFullscreen(other, false);

    arrange(t->workspace);
    focusToplevel(t);

    if (m_eventBus.hasHandlers(EventType::WindowOpen))
        m_eventBus.emit(EventType::WindowOpen, windowInfo(t));
}

void
MuWM::removeToplevel(Toplevel *t)
{
    Workspace *ws = t->workspace;
    if (!ws)
        return;

    // Snapshot before the window is detached from its workspace.
    EventData closed;
    if (m_eventBus.hasHandlers(EventType::WindowClose))
        closed = windowInfo(t);

    auto &toplevels = ws->toplevels;

    std::erase(toplevels, t);
    t->workspace = nullptr;
    if (ws->focusedToplevel == t)
        ws->focusedToplevel = nullptr;
    arrange(ws);
    if (ws == currentWorkspace() && !ws->focusedToplevel && !toplevels.empty())
        focusToplevel(toplevels.back());

    m_eventBus.emit(EventType::WindowClose, closed);
}

void
MuWM::arrange()
{
    for (Output *output : m_outputs)
    {
        arrangeLayers(output->output);
        arrange(output->workspace);
    }
}

void
MuWM::arrange(Workspace *ws)
{
    if (ws->output)
        updateFullscreenLayers(ws->output);

    wlr_box area    = ws->output ? ws->output->usableArea : wlr_box{};
    auto &toplevels = ws->toplevels;
    if (area.width <= 0 || toplevels.empty())
        return;

    auto place = [](Toplevel *t, int x, int y, int w, int h, bool borders)
    {
        wlr_scene_node_set_position(&t->tree->node, x, y);
        wlr_xdg_toplevel_set_size(t->xdg, w, h);
        t->setBordersVisible(borders);
        if (borders)
            t->resizeBorders(w, h);
    };

    // A fullscreen window gets the whole output (bars included) and nothing
    // else is laid out; the other windows stay hidden behind it.
    for (Toplevel *t : toplevels)
    {
        if (!t->fullscreen)
            continue;

        wlr_box full;
        wlr_output_layout_get_box(m_layout, ws->output->output, &full);
        const int bw = m_config.border.fullscreen ? m_config.border.width : 0;
        place(t, full.x + bw, full.y + bw, full.width - 2 * bw,
              full.height - 2 * bw, bw > 0);
        wlr_scene_node_raise_to_top(&t->tree->node);
        return;
    }

    int gap;

    const int n = static_cast<int>(toplevels.size());

    if (m_config.gaps.smart && n == 1)
        gap = 0;
    else
    {
        gap = m_config.gaps.value;
        area.x += gap;
        area.y += gap;
        area.width -= 2 * gap;
        area.height -= 2 * gap;
    }

    // Smart borders: a lone window needs no border to be told apart.
    const bool borders = !(m_config.border.smart && n == 1);

    if (n == 1)
    {
        place(toplevels[0], area.x, area.y, area.width, area.height, borders);
        return;
    }

    const int masterW = (area.width - gap) * m_config.layout.master_ratio;
    const int stackW  = area.width - gap - masterW;
    place(toplevels[0], area.x, area.y, masterW, area.height, borders);

    // Integer division would leave the remainder as empty space under the
    // stack, so hand one extra pixel to each of the first `rem` windows.
    const int stackN = n - 1;
    const int avail  = area.height - gap * (stackN - 1);
    const int h      = avail / stackN;
    const int rem    = avail % stackN;
    int y            = area.y;
    for (int i = 1; i < n; i++)
    {
        const int hi = h + (i - 1 < rem ? 1 : 0);
        place(toplevels[i], area.x + masterW + gap, y, stackW, hi, borders);
        y += hi + gap;
    }
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
MuWM::onNewLayerShellSurface(void *data)
{
    auto *ls = static_cast<wlr_layer_surface_v1 *>(data);

    // The client may leave the output unspecified; the compositor must
    // pick.
    if (!ls->output)
        ls->output = wlr_output_layout_get_center_output(m_layout);
    if (!ls->output)
    {
        wlr_log(WLR_ERROR, "Layer surface with no output available");
        wlr_layer_surface_v1_destroy(ls);
        return;
    }

    auto *s         = new LayerShellSurface{};
    s->wm           = this;
    s->layerSurface = ls;
    s->scene        = wlr_scene_layer_surface_v1_create(
        m_layerTrees[ls->pending.layer], ls);

    s->map.bind<&LayerShellSurface::onMap>(&ls->surface->events.map, s);
    s->unmap.bind<&LayerShellSurface::onUnmap>(&ls->surface->events.unmap, s);
    s->commit.bind<&LayerShellSurface::onCommit>(&ls->surface->events.commit,
                                                 s);
    s->destroy.bind<&LayerShellSurface::onDestroy>(&ls->events.destroy, s);

    m_layerSurfaces.push_back(s);
}

// Gives the keyboard to a layer surface (a launcher, say). The window that
// had it stays the "focused" one logically and gets the keyboard back later.
void
MuWM::focusLayerSurface(LayerShellSurface *s)
{
    if (m_focusedLayer == s)
        return;

    if (Toplevel *t = focusedToplevel())
    {
        wlr_xdg_toplevel_set_activated(t->xdg, false);
        t->setFocused(false);
    }
    m_focusedLayer = s;

    if (wlr_keyboard *kb = wlr_seat_get_keyboard(m_seat))
        wlr_seat_keyboard_notify_enter(m_seat, s->layerSurface->surface,
                                       kb->keycodes, kb->num_keycodes,
                                       &kb->modifiers);
    wlr_log(WLR_INFO, "Keyboard focus: layer surface %s",
            s->layerSurface->namespace_ ? s->layerSurface->namespace_
                                        : "(unnamed)");
}

void
MuWM::releaseLayerFocus(LayerShellSurface *s)
{
    if (m_focusedLayer != s)
        return;
    m_focusedLayer = nullptr;

    if (Toplevel *t = focusedToplevel())
    {
        t->setFocused(true);
        wlr_xdg_toplevel_set_activated(t->xdg, true);
        if (wlr_keyboard *kb = wlr_seat_get_keyboard(m_seat))
            wlr_seat_keyboard_notify_enter(m_seat, t->xdg->base->surface,
                                           kb->keycodes, kb->num_keycodes,
                                           &kb->modifiers);
        wlr_log(WLR_INFO, "Keyboard focus: back to the window");
    }
    else
    {
        wlr_seat_keyboard_clear_focus(m_seat);
    }
}

void
MuWM::moveLayerSurface(LayerShellSurface *s)
{
    wlr_scene_node_reparent(&s->scene->tree->node,
                            m_layerTrees[s->layerSurface->current.layer]);
}

void
MuWM::removeLayerShellSurface(LayerShellSurface *s)
{
    std::erase(m_layerSurfaces, s);
    if (s->layerSurface->output && s->layerSurface->output->data)
        arrangeLayers(s->layerSurface->output);
}

void
MuWM::closeLayerSurfacesOn(wlr_output *output)
{
    // The protocol requires closing surfaces whose output goes away. Their
    // destroy handlers edit the list, so iterate over a copy.
    const auto surfaces = m_layerSurfaces;
    for (LayerShellSurface *s : surfaces)
        if (s->layerSurface->output == output)
            wlr_layer_surface_v1_destroy(s->layerSurface);
}

// Positions every layer surface of `output` and records what area is left
// over for windows. Surfaces with an exclusive zone (bars) go first so that
// the others are laid out in the remaining space; higher layers win ties.
void
MuWM::arrangeLayers(wlr_output *wlrOutput)
{
    if (!wlrOutput || !wlrOutput->data)
        return;
    auto *output = static_cast<Output *>(wlrOutput->data);

    wlr_box full;
    wlr_output_layout_get_box(m_layout, wlrOutput, &full);
    wlr_box usable = full;

    for (bool exclusive : {true, false})
        for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
             layer >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; layer--)
            for (LayerShellSurface *s : m_layerSurfaces)
            {
                wlr_layer_surface_v1 *ls = s->layerSurface;
                if (ls->output != wlrOutput
                    || static_cast<int>(ls->current.layer) != layer
                    || (ls->current.exclusive_zone > 0) != exclusive)
                    continue;
                wlr_scene_layer_surface_v1_configure(s->scene, &full, &usable);
            }

    output->usableArea = usable;
    updateFullscreenLayers(output); // new surfaces must respect fullscreen
}

// While an output shows a fullscreen window, its top-layer surfaces (bars)
// are hidden. Derived from the current state each time, so every path that
// changes it (close, workspace switch, ...) only has to call arrange().
void
MuWM::updateFullscreenLayers(Output *output)
{
    bool fullscreen = false;
    if (output->workspace)
        for (const Toplevel *t : output->workspace->toplevels)
            fullscreen = fullscreen || t->fullscreen;

    for (LayerShellSurface *s : m_layerSurfaces)
        if (s->layerSurface->output == output->output
            && s->layerSurface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP)
            wlr_scene_node_set_enabled(&s->scene->tree->node, !fullscreen);
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

    if (changes & (ChangeKeyboard | ChangePointer))
        applyDeviceConfig(changes);
}

void
MuWM::recolorBorders()
{
    Toplevel *focused = focusedToplevel();
    for (Output *output : m_outputs)
        for (Workspace *ws : output->workspaces)
            for (Toplevel *t : ws->toplevels)
                t->setFocused(t == focused);
}

void
MuWM::addBinding(Binding b)
{
    for (Binding &existing : m_keybindings)
        if (existing.mods == b.mods && existing.sym == b.sym)
        {
            if (existing.ref != LUA_NOREF)
                luaL_unref(m_lua, LUA_REGISTRYINDEX, existing.ref);
            wlr_log(WLR_DEBUG, "Replacing keybinding for sym 0x%x", b.sym);
            existing.ref    = b.ref;
            existing.native = std::move(b.native);
            return;
        }
    m_keybindings.push_back(b);
}

void
MuWM::clearBindings()
{
    wlr_log(WLR_DEBUG, "Clearing %zu keybindings", m_keybindings.size());
    for (const Binding &b : m_keybindings)
        if (b.ref != LUA_NOREF)
            luaL_unref(m_lua, LUA_REGISTRYINDEX, b.ref);
    m_keybindings.clear();
}

void
MuWM::quit()
{
    wl_display_terminate(m_display);
}

// Index of the focused window in `ws`, or -1.
static int
focusedIndex(const Workspace *ws)
{
    for (size_t i = 0; i < ws->toplevels.size(); i++)
        if (ws->toplevels[i] == ws->focusedToplevel)
            return static_cast<int>(i);
    return -1;
}

void
MuWM::focusNext()
{
    Workspace *ws = currentWorkspace();
    const int i   = focusedIndex(ws);
    if (i >= 0)
        focusToplevel(ws->toplevels[(i + 1) % ws->toplevels.size()]);
}

void
MuWM::focusPrev()
{
    Workspace *ws = currentWorkspace();
    const int i   = focusedIndex(ws);
    if (i >= 0)
        focusToplevel(ws->toplevels[(i + ws->toplevels.size() - 1)
                                    % ws->toplevels.size()]);
}

void
MuWM::swapNext()
{
    Workspace *ws = currentWorkspace();
    const int i   = focusedIndex(ws);
    if (i < 0)
        return;
    std::swap(ws->toplevels[i], ws->toplevels[(i + 1) % ws->toplevels.size()]);
    arrange(ws);
}

void
MuWM::swapMaster()
{
    Workspace *ws = currentWorkspace();
    const int i   = focusedIndex(ws);
    if (i < 0)
        return;
    std::swap(ws->toplevels[i], ws->toplevels[0]);
    arrange(ws);
}

void
MuWM::swapPrev()
{
    Workspace *ws = currentWorkspace();
    const int i   = focusedIndex(ws);
    if (i < 0)
        return;
    std::swap(
        ws->toplevels[i],
        ws->toplevels[(i + ws->toplevels.size() - 1) % ws->toplevels.size()]);
    arrange(ws);
}

void
MuWM::close()
{
    if (Toplevel *t = focusedToplevel())
        wlr_xdg_toplevel_send_close(t->xdg);
}

void
MuWM::removeOutput(Output *output)
{
    std::erase(m_outputs, output);
    Workspace *target
        = m_outputs.empty() ? &m_unassigned : m_outputs.front()->workspace;

    wlr_scene_tree *targetTree
        = target->sceneTree ? target->sceneTree : m_windowTree;

    for (Workspace *ws : output->workspaces)
    {
        for (Toplevel *t : ws->toplevels)
        {
            t->workspace = target;
            wlr_scene_node_reparent(&t->tree->node, targetTree);
            target->toplevels.push_back(t);
        }

        if (!target->focusedToplevel)
            target->focusedToplevel = ws->focusedToplevel;

        ws->toplevels.clear();
        ws->focusedToplevel = nullptr;
    }

    if (m_focusedOutput == output)
        m_focusedOutput = m_outputs.empty() ? nullptr : m_outputs.front();
    arrange(target);
}

void
MuWM::cycleWorkspace(int delta)
{
    if (!m_focusedOutput)
        return;

    auto &workspaces = m_focusedOutput->workspaces;
    const int n      = static_cast<int>(workspaces.size());
    if (n <= 1)
        return;

    Workspace *ws = m_focusedOutput->workspace;
    int i         = std::find(workspaces.begin(), workspaces.end(), ws)
                    - workspaces.begin();
    i             = (i + delta + n) % n;
    switchWorkspace(m_focusedOutput, workspaces[i]);
}

void
MuWM::workspaceNext()
{
    cycleWorkspace(1);
}

void
MuWM::workspacePrev()
{
    cycleWorkspace(-1);
}

void
MuWM::focusWorkspace(int n)
{
    if (!m_focusedOutput)
        return;

    auto &workspaces = m_focusedOutput->workspaces;
    if (n < 1 || n > static_cast<int>(workspaces.size()))
        return;

    switchWorkspace(m_focusedOutput, workspaces[n - 1]);
}

void
MuWM::windowToWorkspace(int index)
{
    if (!m_focusedOutput)
        return;

    auto &workspaces = m_focusedOutput->workspaces;
    if (index < 1 || index > static_cast<int>(workspaces.size()))
        return;

    Workspace *ws_target = workspaces[index - 1];
    if (Toplevel *t = focusedToplevel())
        toplevelToWorkspace(t, ws_target);
}

void
MuWM::switchWorkspace(Output *out, Workspace *ws_target)
{
    if (!out || !ws_target || out->workspace == ws_target)
        return;

    Workspace *ws_old = out->workspace;

    // Deactivate the old window while it is still that workspace's focus.
    if (Toplevel *old = ws_old->focusedToplevel)
    {
        wlr_xdg_toplevel_set_activated(old->xdg, false);
        old->setFocused(false);
    }

    wlr_scene_node_set_enabled(&ws_old->sceneTree->node, false);
    wlr_scene_node_set_enabled(&ws_target->sceneTree->node, true);
    out->workspace  = ws_target;
    m_focusedOutput = out;

    arrange(ws_target);

    Toplevel *next = ws_target->focusedToplevel;
    if (!next && !ws_target->toplevels.empty())
        next = ws_target->toplevels.back();
    // focusToplevel returns early if the window is already "focused"
    ws_target->focusedToplevel = nullptr;

    if (next)
        focusToplevel(next);
    else if (!m_focusedLayer)
        wlr_seat_keyboard_clear_focus(m_seat);
}

void
MuWM::toplevelToWorkspace(Toplevel *toplevel, Workspace *ws_target)
{
    if (!toplevel || !ws_target || toplevel->workspace == ws_target)
        return;

    Workspace *ws_old = toplevel->workspace;
    if (ws_old)
    {
        std::erase(ws_old->toplevels, toplevel);
        if (ws_old->focusedToplevel == toplevel)
            ws_old->focusedToplevel = nullptr;
    }

    toplevel->workspace = ws_target;
    ws_target->toplevels.push_back(toplevel);
    wlr_scene_node_reparent(&toplevel->tree->node, ws_target->sceneTree);

    arrange(ws_old);
    arrange(ws_target);

    focusToplevel(toplevel);
}

void
MuWM::toggleFullscreen()
{
    if (Toplevel *t = focusedToplevel())
        setFullscreen(t, !t->fullscreen);
}

void
MuWM::setFullscreen(Toplevel *toplevel, bool fullscreen)
{
    if (!toplevel || toplevel->fullscreen == fullscreen)
        return;

    toplevel->fullscreen = fullscreen;
    // Before the first commit the state is sent with the initial configure.
    if (toplevel->xdg->base->initialized)
        wlr_xdg_toplevel_set_fullscreen(toplevel->xdg, fullscreen);

    Workspace *ws = toplevel->workspace;
    if (!ws)
        return; // not mapped yet; addToplevel lays it out

    // One fullscreen window per workspace.
    if (fullscreen)
        for (Toplevel *other : std::vector<Toplevel *>(ws->toplevels))
            if (other != toplevel && other->fullscreen)
                setFullscreen(other, false);

    arrange(ws);
}

void
MuWM::setMasterRatio(float ratio)
{
    if (ratio < 0.0f || ratio > 1.0f)
        return;
    scheduleApply(ChangeLayout);
}
