#include "MuWM.hpp"

#include "Decoration.hpp"
#include "IdleInhibitor.hpp"
#include "Keyboard.hpp"
#include "LayerShellSurface.hpp"
#include "LockSurface.hpp"
#include "Output.hpp"
#include "Pointer.hpp"
#include "Popup.hpp"
#include "Toplevel.hpp"
#include "XUnmanaged.hpp"

#include <algorithm>
#include <csignal>
#include <cstring>
#include <linux/input-event-codes.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
bool
isPrivilegedGlobal(const char *name)
{
    static const char *blocked[] = {
        "zwlr_screencopy_manager_v1",
        "ext_image_copy_capture_manager_v1",
        "ext_output_image_capture_source_manager_v1",
        "ext_foreign_toplevel_image_capture_source_manager_v1",
        "zwlr_foreign_toplevel_manager_v1",
        "ext_foreign_toplevel_list_v1",
        "zwlr_data_control_manager_v1",
        "ext_data_control_manager_v1",
        "zwlr_output_manager_v1",
        "zwlr_output_power_manager_v1",
        "zwlr_gamma_control_manager_v1",
        "ext_session_lock_manager_v1",
        "zwlr_layer_shell_v1",
        "ext_workspace_manager_v1",
        "zwp_virtual_keyboard_manager_v1", // when you add them
        "zwlr_virtual_pointer_manager_v1",
        // A sandboxed client must not make new security contexts (to get a
        // fresh, unrestricted-looking socket) or act as the X server.
        "wp_security_context_manager_v1",
        "xwayland_shell_v1",
    };
    for (const char *b : blocked)
        if (!strcmp(name, b))
            return true;
    return false;
}
} // namespace

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

    // X11 windows are only destroyed with the XWayland server, and their
    // handlers free scene nodes, so it has to go while the scene still exists.
    m_xwaylandReady.unbind();
    m_xwaylandNewSurface.unbind();
    if (m_xwayland)
    {
        wlr_xwayland_destroy(m_xwayland);
        m_xwayland = nullptr;
    }

    m_newToplevel.unbind();
    m_newPopup.unbind();
    m_newLayerShellSurface.unbind();
    m_newInput.unbind();
    m_newDecoration.unbind();
    m_layoutChange.unbind();
    m_captureRequest.unbind();
    m_newLock.unbind();
    m_xwaylandReady.unbind();
    m_xwaylandNewSurface.unbind();
    m_lockNewSurface.unbind();
    m_lockUnlock.unbind();
    m_lockDestroy.unbind();
    m_outputApply.unbind();
    m_outputTest.unbind();
    // wlroots asserts that a cursor has no listeners left when destroyed
    m_cursorMotion.unbind();
    m_cursorMotionAbs.unbind();
    m_cursorButton.unbind();
    m_cursorAxis.unbind();
    m_cursorFrame.unbind();
    m_requestSetSelection.unbind();
    m_requestSetPrimary.unbind();
    m_cursorShape.unbind();
    m_requestSetCursor.unbind();
    m_newConstraint.unbind();
    m_constraintDestroy.unbind();
    m_activationRequest.unbind();
    m_sessionActive.unbind();
    m_workspaceCommit.unbind();
    m_newInhibitor.unbind();
    m_outputPowerSetMode.unbind();
    m_requestStartDrag.unbind();
    m_startDrag.unbind();
    m_dragIconDestroy.unbind();

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

    m_backend = wlr_backend_autocreate(wl_display_get_event_loop(m_display),
                                       &m_session);

    if (m_session)
        m_sessionActive.bind<&MuWM::onSessionActive>(&m_session->events.active,
                                                     this);

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

    if (!wlr_renderer_init_wl_shm(m_renderer, m_display))
    {
        wlr_log(WLR_ERROR, "Failed to init wl_shm");
        return false;
    }

    if (wlr_renderer_get_texture_formats(m_renderer, WLR_BUFFER_CAP_DMABUF))
        m_dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(m_display, 5,
                                                            m_renderer);
    else
        wlr_log(WLR_INFO,
                "Renderer can't import dmabufs; linux-dmabuf disabled");

    // Explicit sync needs timelines in both the renderer and the backend. The
    // scene graph passes each surface's acquire and release points on itself.
    if (int drmFd = wlr_renderer_get_drm_fd(m_renderer);
        drmFd >= 0 && m_renderer->features.timeline
        && m_backend->features.timeline)
        wlr_linux_drm_syncobj_manager_v1_create(m_display, 1, drmFd);
    else
        wlr_log(WLR_INFO, "No timeline support; explicit sync disabled");

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

    if (m_dmabuf)
        wlr_scene_set_linux_dmabuf_v1(m_scene, m_dmabuf);

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

    m_lockTree     = wlr_scene_tree_create(&m_scene->tree);
    m_dragIconTree = wlr_scene_tree_create(&m_scene->tree);

    m_compositor = wlr_compositor_create(m_display, 5, m_renderer);
    wlr_subcompositor_create(m_display);
    wlr_xdg_output_manager_v1_create(m_display, m_layout); // bars need it
    wlr_data_device_manager_create(m_display);
    wlr_ext_image_copy_capture_manager_v1_create(m_display, 1);
    wlr_ext_output_image_capture_source_manager_v1_create(m_display, 1);
    wlr_screencopy_manager_v1_create(m_display);
    wlr_primary_selection_v1_device_manager_create(m_display);
    wlr_ext_data_control_manager_v1_create(m_display, 1);
    wlr_data_control_manager_v1_create(m_display);
    wlr_viewporter_create(m_display);
    wlr_fractional_scale_manager_v1_create(m_display, 1);
    m_workspaceManager = wlr_ext_workspace_manager_v1_create(m_display, 1);

    m_workspaceCommit.bind<&MuWM::onWorkspaceCommit>(
        &m_workspaceManager->events.commit, this);

    m_lockManager = wlr_session_lock_manager_v1_create(m_display);
    m_newLock.bind<&MuWM::onNewLock>(&m_lockManager->events.new_lock, this);

    m_cursorShapeManager = wlr_cursor_shape_manager_v1_create(m_display, 1);
    m_cursorShape.bind<&MuWM::onCursorShape>(
        &m_cursorShapeManager->events.request_set_shape, this);

    m_foreignList = wlr_ext_foreign_toplevel_list_v1_create(m_display, 1);
    m_toplevelCaptureManager
        = wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(
            m_display, 1);

    m_captureRequest.bind<&MuWM::onToplevelCaptureRequest>(
        &m_toplevelCaptureManager->events.new_request, this);

    m_xdgShell = wlr_xdg_shell_create(m_display, 6);
    m_newToplevel.bind<&MuWM::onNewToplevel>(&m_xdgShell->events.new_toplevel,
                                             this);
    m_newPopup.bind<&MuWM::onNewPopup>(&m_xdgShell->events.new_popup, this);

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
    m_requestSetCursor.bind<&MuWM::onRequestSetCursor>(
        &m_seat->events.request_set_cursor, this);
    m_requestSetSelection.bind<&MuWM::onRequestSetSelection>(
        &m_seat->events.request_set_selection, this);
    m_requestSetPrimary.bind<&MuWM::onRequestSetPrimarySelection>(
        &m_seat->events.request_set_primary_selection, this);
    m_requestStartDrag.bind<&MuWM::onRequestStartDrag>(
        &m_seat->events.request_start_drag, this);
    m_startDrag.bind<&MuWM::onStartDrag>(&m_seat->events.start_drag, this);

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

    // GTK3 and some Qt apps only know the KDE protocol.
    auto *kdeDeco = wlr_server_decoration_manager_create(m_display);
    wlr_server_decoration_manager_set_default_mode(
        kdeDeco, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);

    m_layerShell = wlr_layer_shell_v1_create(m_display, 4);

    m_newLayerShellSurface.bind<&MuWM::onNewLayerShellSurface>(
        &m_layerShell->events.new_surface, this);

    // kanshi and wlr-randr configure outputs through this protocol
    m_outputManager = wlr_output_manager_v1_create(m_display);
    m_outputApply.bind<&MuWM::onOutputManagerApply>(
        &m_outputManager->events.apply, this);
    m_outputTest.bind<&MuWM::onOutputManagerTest>(&m_outputManager->events.test,
                                                  this);

    m_xdgActivation = wlr_xdg_activation_v1_create(m_display);
    m_activationRequest.bind<&MuWM::onActivationRequest>(
        &m_xdgActivation->events.request_activate, this);

    m_relativePointer = wlr_relative_pointer_manager_v1_create(m_display);
    m_constraints     = wlr_pointer_constraints_v1_create(m_display);
    m_newConstraint.bind<&MuWM::onNewConstraint>(
        &m_constraints->events.new_constraint, this);

    m_foreignToplevelManager
        = wlr_foreign_toplevel_manager_v1_create(m_display);

    m_idleNotifier = wlr_idle_notifier_v1_create(m_display);

    m_idleInhibit = wlr_idle_inhibit_v1_create(m_display);
    m_newInhibitor.bind<&MuWM::onNewInhibitor>(
        &m_idleInhibit->events.new_inhibitor, this);

    m_outputPower = wlr_output_power_manager_v1_create(m_display);
    m_outputPowerSetMode.bind<&MuWM::onOutputPowerSetMode>(
        &m_outputPower->events.set_mode, this);

    m_gammaControl = wlr_gamma_control_manager_v1_create(m_display);
    wlr_scene_set_gamma_control_manager_v1(m_scene, m_gammaControl);

    wlr_presentation_create(m_display, m_backend, 2);

    m_pointerGestures = wlr_pointer_gestures_v1_create(m_display);
    m_pinchBegin.bind<&MuWM::onPinchBegin>(&m_cursor->events.pinch_begin, this);
    m_pinchUpdate.bind<&MuWM::onPinchUpdate>(&m_cursor->events.pinch_update,
                                             this);
    m_pinchEnd.bind<&MuWM::onPinchEnd>(&m_cursor->events.pinch_end, this);

    m_contentType = wlr_content_type_manager_v1_create(m_display, 1);

    m_tearing = wlr_tearing_control_manager_v1_create(m_display, 1);

    m_securityContext = wlr_security_context_manager_v1_create(m_display);

    wl_display_set_global_filter(
        m_display,
        [](const wl_client *client, const wl_global *global, void *data) -> bool
    {
        auto *wm = static_cast<MuWM *>(data);

        // Normal clients see every global.
        if (!wlr_security_context_manager_v1_lookup_client(
                wm->securityContext(), client))
            return true;

        // Sandboxed clients don't see the privileged ones.
        return !isPrivilegedGlobal(wl_global_get_interface(global)->name);
    }, this);

    // Lazy: Xwayland only starts when the first X11 client connects.
    m_xwayland = wlr_xwayland_create(m_display, m_compositor, true);
    if (m_xwayland)
    {
        // The socket exists already, so programs we spawn can find it.
        setenv("DISPLAY", m_xwayland->display_name, 1);
        m_xwaylandReady.bind<&MuWM::onXwaylandReady>(&m_xwayland->events.ready,
                                                     this);
        m_xwaylandNewSurface.bind<&MuWM::onNewXWaylandSurface>(
            &m_xwayland->events.new_surface, this);
    }
    else
        wlr_log(WLR_ERROR, "Failed to create XWayland; X11 apps won't work");

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
    wlr_output_state_set_scale(&state, m_config.scale);
    wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);

    wlr_xcursor_manager_load(m_xcursor, m_config.scale);

    auto *o            = new Output{};
    o->wm              = this;
    o->output          = output;
    o->sceneOutput     = wlr_scene_output_create(m_scene, output);
    output->data       = o;
    auto *layoutOutput = wlr_output_layout_add_auto(m_layout, output);
    wlr_scene_output_layout_add_output(m_sceneLayout, layoutOutput,
                                       o->sceneOutput);
    o->frame.bind<&Output::onFrame>(&output->events.frame, o);
    o->destroy.bind<&Output::onDestroy>(&output->events.destroy, o);
    o->requestState.bind<&Output::onRequestState>(&output->events.request_state,
                                                  o);
    // The workspace group is a handle that clients can use to find all the
    // workspaces on this output. It must exist before the workspaces, which
    // join it.
    o->group = wlr_ext_workspace_group_handle_v1_create(m_workspaceManager, 0);
    wlr_ext_workspace_group_handle_v1_output_enter(o->group, output);

    const auto &names = m_config.workspaces;
    for (size_t i = 0; i < names.size(); i++)
        createWorkspace(o, names[i], i);

    // Only the first workspace is visible.
    o->workspace = o->workspaces[0];
    wlr_scene_node_set_enabled(&o->workspace->sceneTree->node, true);

    wlr_ext_workspace_handle_v1_set_active(o->workspace->handle, true);

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
            t->syncForeignOutput(nullptr);
        }
        o->workspace->toplevels       = std::move(m_unassigned.toplevels);
        o->workspace->focusHistory    = std::move(m_unassigned.focusHistory);
        o->workspace->focusedToplevel = m_unassigned.focusedToplevel;
        m_unassigned                  = {};
    }

    arrangeLayers(output);
    arrange(o->workspace);
    if (m_locked)
    {
        createLockCover(o); // a monitor plugged in during the lock
        layoutLock();
    }
    publishOutputConfig();

    wlr_log(WLR_INFO, "New output: %s", output->name);
}

void
MuWM::publishOutputConfig()
{
    if (!m_outputManager)
        return;

    wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
    for (Output *o : m_outputs)
    {
        auto *head = wlr_output_configuration_head_v1_create(config, o->output);
        if (auto *lo = wlr_output_layout_get(m_layout, o->output))
        {
            head->state.x = lo->x;
            head->state.y = lo->y;
        }
    }
    wlr_output_manager_v1_set_configuration(m_outputManager, config);
}

bool
MuWM::applyOutputConfig(wlr_output_configuration_v1 *config, bool test)
{
    // A disabled output may have no current mode; enabling it then needs one,
    // so fall back to the preferred mode when the client didn't name any.
    wlr_output_configuration_head_v1 *h;
    wl_list_for_each(h, &config->heads, link)
    {
        wlr_output *wo = h->state.output;
        wlr_log(
            WLR_INFO,
            "Output config %s: %s %s mode=%p custom=%dx%d scale=%.2f pos=%d,%d",
            test ? "test" : "apply", wo->name,
            h->state.enabled ? "enable" : "disable", (void *)h->state.mode,
            h->state.custom_mode.width, h->state.custom_mode.height,
            h->state.scale, h->state.x, h->state.y);
        if (h->state.enabled && !h->state.mode
            && h->state.custom_mode.width == 0)
            h->state.mode = wlr_output_preferred_mode(wo);
    }

    size_t n = 0;
    wlr_backend_output_state *states
        = wlr_output_configuration_v1_build_state(config, &n);
    if (!states)
        return false;

    bool ok = test ? wlr_backend_test(m_backend, states, n)
                   : wlr_backend_commit(m_backend, states, n);

    for (size_t i = 0; i < n; i++)
        wlr_output_state_finish(&states[i].base);
    free(states);

    if (!ok)
        wlr_log(WLR_ERROR, "Output config %s failed", test ? "test" : "commit");
    if (!ok || test)
        return ok;

    // The backend doesn't know about positions; they live in the layout. The
    // layout's change event re-arranges the windows.
    wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link)
    {
        const wlr_output_head_v1_state &s = head->state;
        if (s.enabled)
        {
            // An output that was disabled left the layout and took its scene
            // link with it, so tie the scene output to the new entry again.
            const bool rejoin = !wlr_output_layout_get(m_layout, s.output);
            auto *lo = wlr_output_layout_add(m_layout, s.output, s.x, s.y);
            if (rejoin && s.output->data)
            {
                auto *out = static_cast<Output *>(s.output->data);
                wlr_scene_output_layout_add_output(m_sceneLayout, lo,
                                                   out->sceneOutput);
            }
            wlr_output_schedule_frame(s.output);
            wlr_xcursor_manager_load(m_xcursor, s.scale);
        }
        else
            wlr_output_layout_remove(m_layout, s.output);
    }
    return true;
}

void
MuWM::onToplevelCaptureRequest(void *data)
{
    auto *req = static_cast<
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *>(
        data);
    auto *t = static_cast<Toplevel *>(req->toplevel_handle->data);
    if (!t)
        return;

    wlr_ext_image_capture_source_v1 *source
        = wlr_ext_image_capture_source_v1_create_with_scene_node(
            &t->surfaceTree->node, wl_display_get_event_loop(m_display),
            m_allocator, m_renderer);
    if (!source)
        return;
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
        req, source);
}

void
MuWM::onOutputManagerApply(void *data)
{
    auto *config = static_cast<wlr_output_configuration_v1 *>(data);
    if (applyOutputConfig(config, false))
        wlr_output_configuration_v1_send_succeeded(config);
    else
        wlr_output_configuration_v1_send_failed(config);
    wlr_output_configuration_v1_destroy(config);

    arrange();
    publishOutputConfig(); // after a failed apply this resyncs the clients
}

void
MuWM::onOutputManagerTest(void *data)
{
    auto *config = static_cast<wlr_output_configuration_v1 *>(data);
    if (applyOutputConfig(config, true))
        wlr_output_configuration_v1_send_succeeded(config);
    else
        wlr_output_configuration_v1_send_failed(config);
    wlr_output_configuration_v1_destroy(config);
}

bool
MuWM::run(const std::string &startupCmd)
{
    const char *socket = wl_display_add_socket_auto(m_display);
    if (!socket)
        return false;

    setenv("WAYLAND_DISPLAY", socket, 1);
    wlr_log(WLR_INFO, "Running on WAYLAND_DISPLAY=%s", socket);

    // Portals, Quickshell and D-Bus activated services look at these to learn
    // which desktop they are in. Set before the config runs, so spawned
    // programs inherit them.
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    setenv("XDG_CURRENT_DESKTOP", "muwm", 1);
    setenv("XDG_SESSION_DESKTOP", "muwm", 1);

    spawn("dbus-update-activation-environment --systemd "
          "WAYLAND_DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE "
          "XDG_SESSION_DESKTOP");

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
    auto *t         = new Toplevel{};
    t->wm           = this;
    t->xdg          = xdg;
    t->tree         = wlr_scene_tree_create(m_windowTree);
    t->surfaceTree  = wlr_scene_xdg_surface_create(t->tree, xdg->base);
    xdg->base->data = t->surfaceTree; // popups look their parent's tree up here
    t->id           = m_globalTopLevelId++;

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
    t->setTitle.bind<&Toplevel::onSetTitle>(&xdg->events.set_title, t);
    t->setAppId.bind<&Toplevel::onSetAppId>(&xdg->events.set_app_id, t);
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

        auto *p   = new Pointer{};
        p->wm     = this;
        p->device = device;
        p->destroy.bind<&Pointer::onDestroy>(&device->events.destroy, p);
        m_pointers.push_back(p);
        applyPointerConfig(p);
        return;
    }
    if (device->type != WLR_INPUT_DEVICE_KEYBOARD)
        return;

    wlr_keyboard *kb = wlr_keyboard_from_input_device(device);

    auto *k = new Keyboard{};
    k->wm   = this;
    k->kb   = kb;
    k->key.bind<&Keyboard::onKey>(&kb->events.key, k);
    k->modifiers.bind<&Keyboard::onModifiers>(&kb->events.modifiers, k);
    k->destroy.bind<&Keyboard::onDestroy>(&kb->base.events.destroy, k);
    m_keyboards.push_back(k);
    applyKeyboardConfig(k);

    wlr_seat_set_keyboard(m_seat, kb);

    Toplevel *t = focusedToplevel();
    if (t && !m_seat->keyboard_state.focused_surface)
        wlr_seat_keyboard_notify_enter(m_seat, t->surface(), kb->keycodes,
                                       kb->num_keycodes, &kb->modifiers);
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

    // These are touchpad options; only touchpads support tapping, so this
    // leaves mice alone.
    if (libinput_device_config_tap_get_finger_count(dev) == 0)
        return;

    // Always write the full state: turning an option off must take effect too.
    if (libinput_device_config_scroll_has_natural_scroll(dev))
    {
        libinput_device_config_scroll_set_natural_scroll_enabled(
            dev, m_config.touchpad.natural_scroll);
        wlr_log(WLR_DEBUG, "%s: natural scroll wanted=%d now=%d",
                pointer->device->name, m_config.touchpad.natural_scroll,
                libinput_device_config_scroll_get_natural_scroll_enabled(dev));
    }

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
    if (m_locked)
        return false; // no bindings behind the lock screen

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
MuWM::focusToplevel(Toplevel *t, bool raise)
{

    Toplevel *oldFocus = focusedToplevel();

    if (oldFocus == t)
        return;
    if (oldFocus)
    {
        oldFocus->setActivated(false);
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

    // Erase first so the window can't appear twice, then make it the most
    // recent entry (the back).
    std::erase(ws->focusHistory, t);
    ws->focusHistory.push_back(t);

    // Focusing another window would hide it behind the fullscreen one.
    const bool otherFullscreen
        = std::ranges::any_of(ws->toplevels, [t](const Toplevel *o)
    { return o != t && o->fullscreen; });
    if (otherFullscreen)
        for (Toplevel *other : std::vector<Toplevel *>(ws->toplevels))
            if (other != t && other->fullscreen)
                setFullscreen(other, false);

    if (raise)
        wlr_scene_node_raise_to_top(&t->tree->node);

    if (!t->floating)
        raiseFloaters(ws); // a focused tile must not cover floating windows
    if (m_focusedLayer || m_focusedUnmanaged || m_locked)
        return; // the window is focused again when the layer surface lets go

    t->setFocused(true);
    t->setActivated(true);
    if (wlr_keyboard *kb = wlr_seat_get_keyboard(m_seat))
        wlr_seat_keyboard_notify_enter(m_seat, t->surface(), kb->keycodes,
                                       kb->num_keycodes, &kb->modifiers);
}

WindowInfo
MuWM::windowInfo(const Toplevel *t) const
{
    WindowInfo info;
    info.id     = t->id;
    info.title  = t->title();
    info.appId  = t->appId();
    info.width  = t->surface()->current.width;
    info.height = t->surface()->current.height;
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

    t->syncForeignOutput(nullptr);

    auto &toplevels = t->workspace->toplevels;
    insertNewTopLevel(toplevels, t);

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
MuWM::insertNewTopLevel(std::vector<Toplevel *> &toplevels, Toplevel *t)
{
    NewClientPosition pos = m_config.new_client_position;

    if (pos == NewClientPosition::Top)
        toplevels.insert(toplevels.begin(), t);
    else if (pos == NewClientPosition::Bottom)
        toplevels.push_back(t);
    else if (pos == NewClientPosition::AfterCurrent)
    {
        Toplevel *focused = focusedToplevel();
        if (!focused || focused->workspace != t->workspace)
            toplevels.push_back(t);
        else
        {
            auto it = std::find(toplevels.begin(), toplevels.end(), focused);
            toplevels.insert(it + 1, t);
        }
    }
    else if (pos == NewClientPosition::BeforeCurrent)
    {
        Toplevel *focused = focusedToplevel();
        if (!focused || focused->workspace != t->workspace)
            toplevels.push_back(t);
        else
        {
            auto it = std::find(toplevels.begin(), toplevels.end(), focused);
            toplevels.insert(it, t);
        }
    }
    else
    {
        wlr_log(WLR_ERROR, "Unknown new_client_position value %d",
                static_cast<int>(pos));
        toplevels.push_back(t);
    }
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

    if (t == m_grabbed)
    {
        // The window is going away: just drop the grab. Moving it to another
        // workspace now would leave a dangling pointer in that workspace.
        m_grab    = Grab::None;
        m_grabbed = nullptr;
    }

    auto &toplevels = ws->toplevels;

    std::erase(toplevels, t);
    std::erase(ws->focusHistory, t);

    t->workspace = nullptr;
    if (ws->focusedToplevel == t)
        ws->focusedToplevel = nullptr;
    arrange(ws);

    // The window used before the closed one is the most recent history entry.
    if (!ws->focusedToplevel && !ws->focusHistory.empty())
    {
        if (ws == currentWorkspace())
            focusToplevel(ws->focusHistory.back());
        else
            ws->focusedToplevel = ws->focusHistory.back(); // hidden workspace
    }

    refreshPointerFocus(); // the cursor may now be over something else
    m_eventBus.emit(EventType::WindowClose, closed);

    updateIdleInhibit();
}

void
MuWM::arrange()
{
    for (Output *output : m_outputs)
    {
        arrangeLayers(output->output, false); // arrange() below does the layout
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
        t->clampSize(w, h);
        t->slot = {x, y, w, h};
        wlr_scene_node_set_position(&t->tree->node, x, y);
        t->setSize(w, h);
        t->setBordersVisible(borders);
        if (borders)
            t->fitBorders();
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

    auto &tiled = m_tiled;
    tiled.clear();
    for (Toplevel *t : toplevels)
    {
        if (t->floating)
        {
            t->setTiled(false);
            placeFloating(t);
        }
        else
        {
            t->setTiled(true);
            tiled.push_back(t);
        }
    }
    if (tiled.empty())
        return;

    int gap;

    const int n = static_cast<int>(tiled.size());

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
        place(tiled[0], area.x, area.y, area.width, area.height, borders);
        return;
    }

    const int masterW
        = (area.width - gap)
          * (ws->masterRatio >= 0.0f ? ws->masterRatio
                                     : m_config.layout.master_ratio);
    const int stackW = area.width - gap - masterW;
    place(tiled[0], area.x, area.y, masterW, area.height, borders);

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
        place(tiled[i], area.x + masterW + gap, y, stackW, hi, borders);
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

// Finds the window a surface belongs to. Looks at the root surface, so
// subsurfaces and popups map to their window. Windows without an output
// (the unassigned workspace) are not searched.
Toplevel *
MuWM::toplevelFromSurface(const wlr_surface *surface)
{
    if (!surface)
        return nullptr;
    const wlr_surface *root
        = wlr_surface_get_root_surface(const_cast<wlr_surface *>(surface));
    for (Output *o : m_outputs)
        for (Workspace *ws : o->workspaces)
            for (Toplevel *t : ws->toplevels)
                if (t->surface() == root)
                    return t;
    return nullptr;
}

void
MuWM::processMotion(uint32_t timeMsec, bool allowFocus)
{
    if (m_seat->drag)
        wlr_scene_node_set_position(&m_dragIconTree->node, m_cursor->x,
                                    m_cursor->y);

    if (m_locked)
    {
        // Only lock surfaces may see the pointer.
        double sx = 0, sy = 0;
        wlr_scene_node *node = wlr_scene_node_at(&m_lockTree->node, m_cursor->x,
                                                 m_cursor->y, &sx, &sy);
        if (node && node->type == WLR_SCENE_NODE_BUFFER)
            if (auto *ss = wlr_scene_surface_try_from_buffer(
                    wlr_scene_buffer_from_node(node)))
            {
                wlr_seat_pointer_notify_enter(m_seat, ss->surface, sx, sy);
                wlr_seat_pointer_notify_motion(m_seat, timeMsec, sx, sy);
                return;
            }
        wlr_seat_pointer_notify_clear_focus(m_seat);
        return;
    }

    if (m_grab != Grab::None && m_grabbed)
    {
        wlr_box &b = m_grabbed->floatBox;
        if (m_grab == Grab::Move)
        {
            b.x = static_cast<int>(m_cursor->x - m_grabX);
            b.y = static_cast<int>(m_cursor->y - m_grabY);
        }
        else
        {
            b.width = std::max(
                100, m_grabBox.width + static_cast<int>(m_cursor->x - m_grabX));
            b.height = std::max(
                60, m_grabBox.height + static_cast<int>(m_cursor->y - m_grabY));
        }
        placeFloating(m_grabbed);
        return; // the client doesn't see the pointer during a grab
    }

    wlr_surface *surface = nullptr;
    double sx = 0, sy = 0;
    Toplevel *t = toplevelAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy);

    if (allowFocus && m_config.focus_follows_mouse)
    {
        if (t)
        {
            if (t != focusedToplevel())
                focusToplevel(t, m_config.raise_on_focus);
        }
        else if (wlr_output *wo = wlr_output_layout_output_at(
                     m_layout, m_cursor->x, m_cursor->y))
        {
            // Over an empty part of a monitor (no window): the monitor itself
            // gains focus.
            for (Output *o : m_outputs)
                if (o->output == wo)
                {
                    setFocusedOutput(o);
                    break;
                }
        }
    }

    // Judge by the surface, not the window: X11 menus and layer-shell surfaces
    // have a surface under the pointer but no Toplevel.
    if (!surface)
    {
        updateConstraint(nullptr);
        // Over empty space nothing changes the cursor, so set it once instead
        // of on every motion event.
        if (!m_cursorIsDefault)
        {
            wlr_cursor_set_xcursor(m_cursor, m_xcursor, "default");
            m_cursorIsDefault = true;
        }
        wlr_seat_pointer_clear_focus(m_seat);
        return;
    }

    updateConstraint(surface);
    wlr_seat_pointer_notify_enter(m_seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(m_seat, timeMsec, sx, sy);
}

void
MuWM::onCursorMotion(void *data)
{
    notifyActivity();
    auto *ev = static_cast<wlr_pointer_motion_event *>(data);
    wlr_relative_pointer_manager_v1_send_relative_motion(
        m_relativePointer, m_seat, uint64_t(ev->time_msec) * 1000, ev->delta_x,
        ev->delta_y, ev->unaccel_dx, ev->unaccel_dy);

    double dx = ev->delta_x, dy = ev->delta_y;
    if (!constrainMotion(dx, dy))
    {
        // Locked: the client gets relative motion only. Release the lock if
        // its window lost keyboard focus meanwhile.
        if (m_activeConstraint->surface
            != m_seat->keyboard_state.focused_surface)
            deactivateConstraint();
        return;
    }
    wlr_cursor_move(m_cursor, &ev->pointer->base, dx, dy);
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
    notifyActivity();
    auto *ev = static_cast<wlr_pointer_button_event *>(data);

    if (m_locked)
    {
        // No window grabs or focus changes: just pass it to the locker.
        wlr_seat_pointer_notify_button(m_seat, ev->time_msec, ev->button,
                                       ev->state);
        return;
    }

    if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED && m_grab != Grab::None)
    {
        endGrab();
        return;
    }

    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED
        && (ev->button == BTN_LEFT || ev->button == BTN_RIGHT))
    {
        wlr_keyboard *kb = wlr_seat_get_keyboard(m_seat);
        if (kb && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_LOGO))
        {
            wlr_surface *surface = nullptr;
            double sx, sy;
            Toplevel *t
                = toplevelAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
            if (t && !t->fullscreen)
            {
                beginGrab(t, ev->button == BTN_RIGHT);
                return;
            }
        }
    }

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
    notifyActivity();
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
    // Fork twice so the program is adopted by init and never becomes a
    // zombie. Ignoring SIGCHLD instead would be inherited by Xwayland and break
    // its xkbcomp child ("Failed to compile keymap").
    const pid_t pid = fork();
    if (pid == 0)
    {
        setsid();
        if (fork() == 0)
        {
            execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(1);
        }
        _exit(0); // the first child exits at once
    }
    if (pid > 0)
        waitpid(pid, nullptr, 0); // reap it; the program itself runs on
}

// An output that kanshi (or wlr-randr) disables leaves the output layout and
// has an empty box, so nothing on it can be tiled: arrange() returns early and
// its windows stay unplaced. Move the focus and the windows to an active
// output instead.
void
MuWM::evacuateInactiveOutputs()
{
    auto active = [this](Output *o)
    {
        return wlr_output_layout_get(m_layout, o->output) != nullptr;
    };

    Output *fallback = nullptr; // first output that is still in the layout
    for (Output *o : m_outputs)
        if (active(o))
        {
            fallback = o;
            break;
        }
    if (!fallback)
        return;

    if (m_focusedOutput && !active(m_focusedOutput))
        m_focusedOutput = fallback;

    for (Output *o : m_outputs)
    {
        if (active(o))
            continue;
        for (Workspace *ws : o->workspaces)
            for (Toplevel *t : std::vector<Toplevel *>(ws->toplevels))
                toplevelToWorkspace(t, fallback->workspace);
    }
}

// When the last active output goes away (unplugging a dock while the laptop
// panel is disabled) nothing would be shown, and the profile that re-enables
// the panel may be applied while the dock is still tearing down and fail.
// Turn on a remaining output ourselves.
void
MuWM::ensureActiveOutput()
{
    if (m_outputs.empty())
        return;
    for (Output *o : m_outputs)
        if (wlr_output_layout_get(m_layout, o->output))
            return;

    Output *o = m_outputs.front();
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    if (wlr_output_mode *mode = wlr_output_preferred_mode(o->output))
        wlr_output_state_set_mode(&state, mode);
    wlr_output_state_set_scale(&state, m_config.scale);
    const bool ok = wlr_output_commit_state(o->output, &state);
    wlr_output_state_finish(&state);
    if (!ok)
    {
        wlr_log(WLR_ERROR, "No active output left and enabling %s failed",
                o->output->name);
        return;
    }

    auto *lo = wlr_output_layout_add_auto(m_layout, o->output);
    wlr_scene_output_layout_add_output(m_sceneLayout, lo, o->sceneOutput);
    wlr_output_schedule_frame(o->output);
    wlr_log(WLR_INFO, "No active output left, enabled %s", o->output->name);
}

void
MuWM::onLayoutChange(void *)
{
    evacuateInactiveOutputs(); // before arrange(): dead outputs have no area
    arrange();
    if (m_locked)
        layoutLock();
    publishOutputConfig();
}

void
MuWM::onNewDecoration(void *data)
{
    auto *d = new Decoration{};
    d->deco = static_cast<wlr_xdg_toplevel_decoration_v1 *>(data);
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
    // pick: the focused output, or failing that the middle of the layout.
    if (!ls->output && m_focusedOutput)
        ls->output = m_focusedOutput->output;
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
    if (m_locked)
        return; // the keyboard belongs to the locker

    if (m_focusedLayer == s)
        return;

    if (Toplevel *t = focusedToplevel())
    {
        t->setActivated(false);
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
        t->setActivated(true);
        if (wlr_keyboard *kb = wlr_seat_get_keyboard(m_seat))
            wlr_seat_keyboard_notify_enter(m_seat, t->surface(), kb->keycodes,
                                           kb->num_keycodes, &kb->modifiers);
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
MuWM::arrangeLayers(wlr_output *wlrOutput, bool relayout)
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

    const wlr_box old  = output->usableArea;
    output->usableArea = usable;
    updateFullscreenLayers(output); // new surfaces must respect fullscreen

    // A bar appearing, closing or resizing changes the room left for windows.
    // Hidden workspaces are re-arranged when they are switched to.
    if (relayout
        && (usable.x != old.x || usable.y != old.y || usable.width != old.width
            || usable.height != old.height))
        arrange(output->workspace);
}

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

    if (changes & ChangeWorkspace)
        applyWorkspaces();
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

// Makes `out` the focused monitor: drops the focus on the old one, then
// focuses whatever the new monitor's workspace used last.
void
MuWM::setFocusedOutput(Output *out)
{
    if (!out || out == m_focusedOutput)
        return;

    // Drop the focus on the old monitor first (border, activated state).
    if (Toplevel *old = focusedToplevel())
    {
        old->setActivated(false);
        old->setFocused(false);
    }
    m_focusedOutput = out;

    Workspace *ws = out->workspace;
    Toplevel *t   = ws ? ws->focusedToplevel : nullptr;
    if (!t && ws && !ws->toplevels.empty())
        t = ws->toplevels.back();

    if (t)
    {
        ws->focusedToplevel = nullptr; // focusToplevel returns early otherwise
        focusToplevel(t);
    }
    else if (!m_focusedLayer && !m_focusedUnmanaged)
        wlr_seat_keyboard_notify_clear_focus(m_seat); // empty monitor
}

void
MuWM::focusOutput(wlr_direction dir)
{
    if (!m_focusedOutput || m_locked)
        return;

    Output *out = adjacentOutput(dir);
    if (!out)
        return; // no monitor in that direction

    setFocusedOutput(out);

    // Move the pointer along so mouse and keyboard stay on the same monitor.
    wlr_box b;
    wlr_output_layout_get_box(m_layout, out->output, &b);
    wlr_cursor_warp(m_cursor, nullptr, b.x + b.width / 2.0,
                    b.y + b.height / 2.0);
    refreshPointerFocus();
}

void
MuWM::moveToOutput(wlr_direction dir)
{
    if (!m_focusedOutput || m_locked)
        return;

    Output *out = adjacentOutput(dir);
    if (!out)
        return; // no monitor in that direction

    Toplevel *t = focusedToplevel();
    if (!t)
        return; // nothing to move

    if (t->floating)
    {
        const wlr_box &from = m_focusedOutput->usableArea;
        const wlr_box &to   = out->usableArea;
        t->floatBox.x += to.x - from.x;
        t->floatBox.y += to.y - from.y;
    }
    toplevelToWorkspace(t, out->workspace);

    wlr_box b;
    wlr_output_layout_get_box(m_layout, out->output, &b);
    wlr_cursor_warp(m_cursor, nullptr, b.x + b.width / 2.0,
                    b.y + b.height / 2.0);
    refreshPointerFocus();
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
        t->close();
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

        // The moved windows are older than anything the target already used,
        // so they go in front of its history, in their own order.
        target->focusHistory.insert(target->focusHistory.begin(),
                                    ws->focusHistory.begin(),
                                    ws->focusHistory.end());
        ws->focusHistory.clear();
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
MuWM::focusWorkspace(const std::string &name)
{
    if (!m_focusedOutput)
        return;

    auto &workspaces = m_focusedOutput->workspaces;
    auto it = std::find_if(workspaces.begin(), workspaces.end(),
                           [&](Workspace *ws) { return ws->name == name; });
    if (it == workspaces.end())
        return;

    switchWorkspace(m_focusedOutput, *it);
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
MuWM::windowToWorkspace(const std::string &name)
{
    if (!m_focusedOutput)
        return;

    auto &workspaces = m_focusedOutput->workspaces;
    auto it = std::find_if(workspaces.begin(), workspaces.end(),
                           [&](Workspace *ws) { return ws->name == name; });
    if (it == workspaces.end())
        return;

    Workspace *ws_target = *it;
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
        old->setActivated(false);
        old->setFocused(false);
    }

    wlr_scene_node_set_enabled(&ws_old->sceneTree->node, false);
    wlr_scene_node_set_enabled(&ws_target->sceneTree->node, true);
    out->workspace = ws_target;

    wlr_ext_workspace_handle_v1_set_active(ws_old->handle, false);
    wlr_ext_workspace_handle_v1_set_active(ws_target->handle, true);

    m_focusedOutput = out;

    arrange(ws_target);
    refreshPointerFocus();

    Toplevel *next = ws_target->focusedToplevel;
    if (!next && !ws_target->toplevels.empty())
        next = ws_target->toplevels.back();
    // focusToplevel returns early if the window is already "focused"
    ws_target->focusedToplevel = nullptr;

    if (next)
        focusToplevel(next);
    else if (!m_focusedLayer && !m_locked)
        wlr_seat_keyboard_clear_focus(m_seat);

    updateIdleInhibit();
}

void
MuWM::toplevelToWorkspace(Toplevel *toplevel, Workspace *ws_target)
{
    if (!toplevel || !ws_target || toplevel->workspace == ws_target)
        return;

    // A visible target is where the user is looking (a drag onto another
    // monitor, an evacuated window), so the window takes the focus there.
    // A hidden one (Super+Shift+N) must not steal the keyboard.
    const bool targetVisible
        = ws_target->output && ws_target->output->workspace == ws_target;

    Workspace *ws_old = toplevel->workspace;
    Toplevel *prev    = nullptr; // what the old workspace used before
    if (ws_old)
    {
        std::erase(ws_old->toplevels, toplevel);
        std::erase(ws_old->focusHistory, toplevel);
        if (!ws_old->focusHistory.empty())
            prev = ws_old->focusHistory.back();
        if (ws_old->focusedToplevel == toplevel)
            ws_old->focusedToplevel = nullptr;
    }

    wlr_output *previousOutput
        = ws_old && ws_old->output ? ws_old->output->output : nullptr;
    toplevel->workspace = ws_target;
    ws_target->toplevels.push_back(toplevel);
    toplevel->syncForeignOutput(previousOutput);

    wlr_scene_node_reparent(&toplevel->tree->node, ws_target->sceneTree);

    if (ws_old)
        arrange(ws_old);
    arrange(ws_target);

    if (targetVisible)
    {
        focusToplevel(toplevel); // also puts it at the back of the new history
        if (ws_old && !ws_old->focusedToplevel)
            ws_old->focusedToplevel = prev; // no keyboard change, just state
        updateIdleInhibit();
        return;
    }

    // Hidden target: remember the window as that workspace's most recent, so
    // it is focused when the user switches there.
    std::erase(ws_target->focusHistory, toplevel);
    ws_target->focusHistory.push_back(toplevel);
    ws_target->focusedToplevel = toplevel;
    toplevel->setActivated(false);
    toplevel->setFocused(false);

    if (ws_old == currentWorkspace())
    {
        if (prev)
            focusToplevel(prev); // keyboard goes to the old workspace's window
        else
            wlr_seat_keyboard_notify_clear_focus(m_seat);
    }
    else if (ws_old)
        ws_old->focusedToplevel = prev;

    updateIdleInhibit();
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
    toplevel->setFullscreen(fullscreen); // tells the client (once initialized)

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
MuWM::setMasterRatio(Workspace *ws, float ratio)
{
    if (ratio < 0.0f || ratio > 1.0f)
        return;
    if (!ws)
        return;
    ws->masterRatio = ratio;
    arrange(ws);
}

void
MuWM::toggleFloating()
{
    if (Toplevel *t = focusedToplevel())
    {
        setFloating(t, !t->floating);
        arrange(t->workspace);
        wlr_scene_node_raise_to_top(&t->tree->node);
    }
}

void
MuWM::setFloating(Toplevel *t, bool floating)
{
    if (t->floating == floating)
        return;
    t->floating = floating;
    if (!floating || t->floatBox.width > 0)
        return; // reuse the geometry from the last time it floated

    Workspace *ws = t->workspace ? t->workspace : currentWorkspace();
    wlr_box area  = ws && ws->output ? ws->output->usableArea : wlr_box{};
    wlr_box geo   = t->geometry();

    // Dialogs keep the size they asked for; a toggled tile gets 60% of the
    // output. Either way the window is centered.
    int w = geo.width, h = geo.height;
    if (!t->isDialog() || w <= 0 || h <= 0)
    {
        if (area.width > 0)
        {
            w = area.width * 6 / 10;
            h = area.height * 6 / 10;
        }
        else if (w <= 0 || h <= 0)
        {
            w = 800;
            h = 600;
        }
    }
    t->floatBox
        = {area.x + (area.width - w) / 2, area.y + (area.height - h) / 2, w, h};
}

void
MuWM::placeFloating(Toplevel *t)
{
    wlr_box &b = t->floatBox;
    t->clampSize(b.width, b.height);
    t->slot = b;
    wlr_scene_node_set_position(&t->tree->node, b.x, b.y);
    t->setSize(b.width, b.height);
    t->setBordersVisible(true);
    t->fitBorders();
}

void
MuWM::raiseFloaters(Workspace *ws)
{
    for (Toplevel *t : ws->toplevels)
        if (t->floating && t != ws->focusedToplevel)
            wlr_scene_node_raise_to_top(&t->tree->node);
    if (ws->focusedToplevel && ws->focusedToplevel->floating)
        wlr_scene_node_raise_to_top(&ws->focusedToplevel->tree->node);
}

void
MuWM::beginGrab(Toplevel *t, bool resize)
{
    deactivateConstraint(); // a locked pointer couldn't drag
    focusToplevel(t);
    m_grabWasTiled = !t->floating;
    if (!t->floating)
    {
        // Pull the tile out of the layout, keeping it where it is. A move puts
        // it back when it is dropped (see endGrab).
        setFloating(t, true);
        wlr_box geo = t->geometry();
        t->floatBox = {t->tree->node.x, t->tree->node.y, geo.width, geo.height};
        arrange(t->workspace);
    }
    wlr_scene_node_raise_to_top(&t->tree->node);

    m_grabbed = t;
    m_grabBox = t->floatBox;
    m_grab    = resize ? Grab::Resize : Grab::Move;
    // Move: offset of the cursor inside the window. Resize: the start point.
    m_grabX   = resize ? m_cursor->x : m_cursor->x - t->floatBox.x;
    m_grabY   = resize ? m_cursor->y : m_cursor->y - t->floatBox.y;
}

void
MuWM::endGrab()
{
    Toplevel *t = m_grabbed; // the window that was dragged, not the focused one
    // A tile that was dragged goes back into the layout when it is dropped. A
    // floating window stays where it was dropped.
    const bool retile = m_grab == Grab::Move && t && m_grabWasTiled;
    m_grab            = Grab::None;
    m_grabbed         = nullptr;
    m_grabWasTiled    = false;

    if (!t || !t->workspace || !t->workspace->output)
        return;

    // Dropped with its center on another monitor: it goes to that monitor.
    wlr_output *under = wlr_output_layout_output_at(
        m_layout, t->floatBox.x + t->floatBox.width / 2.0,
        t->floatBox.y + t->floatBox.height / 2.0);
    if (under)
        for (Output *o : m_outputs)
            if (o->output == under)
            {
                if (o != t->workspace->output)
                    toplevelToWorkspace(t, o->workspace);
                break;
            }

    if (retile)
    {
        setFloating(t, false);
        if (t->workspace)
            arrange(t->workspace);
    }
}

void
MuWM::onRequestSetSelection(void *data)
{
    auto *ev = static_cast<wlr_seat_request_set_selection_event *>(data);
    wlr_seat_set_selection(m_seat, ev->source, ev->serial);
}

void
MuWM::onRequestSetPrimarySelection(void *data)
{
    auto *ev
        = static_cast<wlr_seat_request_set_primary_selection_event *>(data);
    wlr_seat_set_primary_selection(m_seat, ev->source, ev->serial);
}

void
MuWM::onNewPopup(void *data)
{
    auto *xdg                  = static_cast<wlr_xdg_popup *>(data);
    wlr_scene_tree *parentTree = nullptr;

    if (xdg->parent)
    {
        if (wlr_xdg_surface *parent
            = wlr_xdg_surface_try_from_wlr_surface(xdg->parent))
            parentTree = static_cast<wlr_scene_tree *>(parent->data);
        else if (wlr_layer_surface_v1 *layer
                 = wlr_layer_surface_v1_try_from_wlr_surface(xdg->parent))
        {
            // A bar's menu: hang it off the layer surface's own tree.
            for (LayerShellSurface *s : m_layerSurfaces)
                if (s->layerSurface == layer)
                {
                    parentTree = s->scene->tree;
                    break;
                }
        }
    }

    if (!parentTree)
    {
        wlr_log(WLR_ERROR, "Dismissing popup: no scene tree for its parent");
        wlr_xdg_popup_destroy(xdg); // unknown parent: dismiss it
        return;
    }

    xdg->base->data = wlr_scene_xdg_surface_create(parentTree, xdg->base);

    auto *p       = new Popup{};
    p->wm         = this;
    p->xdg        = xdg;
    p->parentTree = parentTree;
    p->commit.bind<&Popup::onCommit>(&xdg->base->surface->events.commit, p);
    p->destroy.bind<&Popup::onDestroy>(&xdg->events.destroy, p);
}

void
MuWM::onCursorShape(void *data)
{
    auto *ev
        = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event *>(
            data);
    if (ev->seat_client == m_seat->pointer_state.focused_client)
    {
        wlr_cursor_set_xcursor(m_cursor, m_xcursor,
                               wlr_cursor_shape_v1_name(ev->shape));
        m_cursorIsDefault = false;
    }
}

void
MuWM::onRequestSetCursor(void *data)
{
    auto *ev = static_cast<wlr_seat_pointer_request_set_cursor_event *>(data);

    // Only the client the pointer is over may change the cursor.
    if (ev->seat_client != m_seat->pointer_state.focused_client)
        return;

    // A null surface hides the cursor.
    wlr_cursor_set_surface(m_cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
    m_cursorIsDefault = false;
}

// --- Session lock (ext-session-lock-v1) ------------------------------------

void
MuWM::onNewLock(void *data)
{
    auto *lock = static_cast<wlr_session_lock_v1 *>(data);

    // One locker at a time. A new one may take over after the old one died.
    if (m_lock)
    {
        wlr_log(WLR_INFO, "Rejecting lock request: a locker is already active");
        wlr_session_lock_v1_destroy(lock);
        return;
    }

    m_lock     = lock;
    m_lockSent = false;
    m_lockNewSurface.bind<&MuWM::onLockNewSurface>(&lock->events.new_surface,
                                                   this);
    m_lockUnlock.bind<&MuWM::onLockUnlock>(&lock->events.unlock, this);
    m_lockDestroy.bind<&MuWM::onLockDestroy>(&lock->events.destroy, this);

    if (!m_locked)
        startLock();
    checkLockReady();
}

// Covers every output at once, so the desktop is hidden before the locker
// has drawn anything, and takes input away from windows.
void
MuWM::startLock()
{
    deactivateConstraint(); // the cursor must be free to reach the lock surface
    m_locked  = true;
    m_grab    = Grab::None; // drop any move/resize in progress
    m_grabbed = nullptr;

    if (m_seat->drag)
        wlr_seat_pointer_end_grab(
            m_seat); // a drag can't reach the lock surface

    if (Toplevel *t = focusedToplevel())
    {
        t->setActivated(false);
        t->setFocused(false);
    }
    m_focusedLayer     = nullptr; // a launcher must not keep the keyboard
    m_focusedUnmanaged = nullptr; // nor an X11 menu
    wlr_seat_keyboard_notify_clear_focus(m_seat);
    wlr_seat_pointer_notify_clear_focus(m_seat);

    wlr_scene_node_set_enabled(&m_lockTree->node, true);
    wlr_scene_node_raise_to_top(&m_lockTree->node);
    for (Output *o : m_outputs)
        createLockCover(o);
}

void
MuWM::createLockCover(Output *o)
{
    // A disabled output is not in the layout and has nothing to cover.
    if (o->lockCover || !wlr_output_layout_get(m_layout, o->output))
        return;

    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // opaque
    wlr_box box;
    wlr_output_layout_get_box(m_layout, o->output, &box);
    o->lockCover
        = wlr_scene_rect_create(m_lockTree, box.width, box.height, black);
    wlr_scene_node_set_position(&o->lockCover->node, box.x, box.y);
}

// Keeps covers and lock surfaces matching their output's box.
void
MuWM::layoutLock()
{
    for (Output *o : m_outputs)
    {
        if (!o->lockCover)
        {
            createLockCover(o); // an output that came back into the layout
            continue;
        }
        wlr_box box;
        wlr_output_layout_get_box(m_layout, o->output, &box);
        wlr_scene_rect_set_size(o->lockCover, box.width, box.height);
        wlr_scene_node_set_position(&o->lockCover->node, box.x, box.y);
    }

    for (LockSurface *ls : m_lockSurfaces)
    {
        wlr_output *out = ls->surface->output;
        if (!out)
            continue;
        wlr_box box;
        wlr_output_layout_get_box(m_layout, out, &box);
        wlr_scene_node_set_position(&ls->tree->node, box.x, box.y);
        if (box.width > 0
            && (box.width != ls->configuredW || box.height != ls->configuredH))
        {
            wlr_session_lock_surface_v1_configure(ls->surface, box.width,
                                                  box.height);
            ls->configuredW = box.width;
            ls->configuredH = box.height;
        }
    }
}

void
MuWM::onLockNewSurface(void *data)
{
    auto *surface = static_cast<wlr_session_lock_surface_v1 *>(data);

    auto *ls    = new LockSurface{};
    ls->wm      = this;
    ls->surface = surface;
    ls->tree = wlr_scene_subsurface_tree_create(m_lockTree, surface->surface);
    ls->map.bind<&LockSurface::onMap>(&surface->surface->events.map, ls);
    ls->destroy.bind<&LockSurface::onDestroy>(&surface->events.destroy, ls);
    m_lockSurfaces.push_back(ls);

    layoutLock(); // positions it and sends the configure
}

void
MuWM::lockSurfaceMapped(LockSurface *)
{
    focusLockSurface();
    checkLockReady();
}

void
MuWM::lockSurfaceDestroyed(LockSurface *s)
{
    std::erase(m_lockSurfaces, s);
    if (m_locked)
        focusLockSurface(); // another surface, or nothing
}

// The keyboard goes to the lock surface on the focused output, else to any.
void
MuWM::focusLockSurface()
{
    LockSurface *pick = nullptr;
    for (LockSurface *ls : m_lockSurfaces)
    {
        if (!ls->surface->surface->mapped)
            continue;
        if (!pick)
            pick = ls;
        if (m_focusedOutput && ls->surface->output == m_focusedOutput->output)
        {
            pick = ls;
            break;
        }
    }

    if (!pick)
    {
        wlr_seat_keyboard_notify_clear_focus(m_seat);
        return;
    }
    if (wlr_keyboard *kb = wlr_seat_get_keyboard(m_seat))
        wlr_seat_keyboard_notify_enter(m_seat, pick->surface->surface,
                                       kb->keycodes, kb->num_keycodes,
                                       &kb->modifiers);
}

// `locked` tells the client the screen is secure, so send it only once every
// active output shows a lock surface.
void
MuWM::checkLockReady()
{
    if (!m_lock || m_lockSent)
        return;

    for (Output *o : m_outputs)
    {
        if (!wlr_output_layout_get(m_layout, o->output))
            continue; // disabled outputs are not shown to the locker
        bool covered = false;
        for (LockSurface *ls : m_lockSurfaces)
            if (ls->surface->output == o->output
                && ls->surface->surface->mapped)
            {
                covered = true;
                break;
            }
        if (!covered)
            return;
    }

    wlr_session_lock_v1_send_locked(m_lock);
    m_lockSent = true;
    wlr_log(WLR_INFO, "Session locked");
}

void
MuWM::onLockUnlock(void *)
{
    wlr_log(WLR_INFO, "Session unlocked");
    m_locked   = false;
    m_lockSent = false;

    for (Output *o : m_outputs)
        if (o->lockCover)
        {
            wlr_scene_node_destroy(&o->lockCover->node);
            o->lockCover = nullptr;
        }
    wlr_scene_node_set_enabled(&m_lockTree->node, false);

    wlr_seat_pointer_notify_clear_focus(m_seat); // the next motion re-enters
    restoreKeyboardFocus();
}

void
MuWM::onLockDestroy(void *)
{
    m_lockNewSurface.unbind();
    m_lockUnlock.unbind();
    m_lockDestroy.unbind();
    m_lock     = nullptr;
    m_lockSent = false;

    // The locker went away without unlocking (a crash): stay locked. The covers
    // stay up and nothing gets input until a new locker takes over.
    if (m_locked)
    {
        wlr_log(WLR_ERROR, "Locker died; the session stays locked");
        wlr_seat_keyboard_notify_clear_focus(m_seat);
        wlr_seat_pointer_notify_clear_focus(m_seat);
    }
}

// Same decision as when a launcher lets go: layer surface, window, or nothing.
void
MuWM::restoreKeyboardFocus()
{
    wlr_keyboard *kb = wlr_seat_get_keyboard(m_seat);

    if (m_focusedLayer && kb)
    {
        wlr_seat_keyboard_notify_enter(
            m_seat, m_focusedLayer->layerSurface->surface, kb->keycodes,
            kb->num_keycodes, &kb->modifiers);
        return;
    }
    if (Toplevel *t = focusedToplevel(); t && kb)
    {
        t->setFocused(true);
        t->setActivated(true);
        wlr_seat_keyboard_notify_enter(m_seat, t->surface(), kb->keycodes,
                                       kb->num_keycodes, &kb->modifiers);
        return;
    }
    wlr_seat_keyboard_notify_clear_focus(m_seat);
}

// --- XWayland --------------------------------------------------------------

// Xwayland has started (on the first X11 client) and the X window manager is
// running, so the seat and the cursor can be handed to it.
void
MuWM::onXwaylandReady(void *)
{
    wlr_log(WLR_INFO, "XWayland ready on DISPLAY=%s", m_xwayland->display_name);

    // Keyboard focus and the clipboard for X11 windows.
    wlr_xwayland_set_seat(m_xwayland, m_seat);

    // The cursor X11 windows show where they don't set their own.
    if (wlr_xcursor *cursor
        = wlr_xcursor_manager_get_xcursor(m_xcursor, "default", 1))
    {
        wlr_xcursor_image *image = cursor->images[0];
        if (wlr_buffer *buffer = wlr_xcursor_image_get_buffer(image))
            wlr_xwayland_set_cursor(m_xwayland, buffer, image->hotspot_x,
                                    image->hotspot_y);
    }
}

// A new X11 window. Menus and tooltips (override-redirect) place themselves;
// everything else is a normal window that gets tiled like an xdg toplevel.
void
MuWM::onNewXWaylandSurface(void *data)
{
    auto *xs = static_cast<wlr_xwayland_surface *>(data);

    if (xs->override_redirect)
    {
        auto *u     = new XUnmanaged{};
        u->wm       = this;
        u->xsurface = xs;
        u->associate.bind<&XUnmanaged::onAssociate>(&xs->events.associate, u);
        u->dissociate.bind<&XUnmanaged::onDissociate>(&xs->events.dissociate,
                                                      u);
        u->destroy.bind<&XUnmanaged::onDestroy>(&xs->events.destroy, u);
        u->requestConfigure.bind<&XUnmanaged::onRequestConfigure>(
            &xs->events.request_configure, u);
        u->setGeometry.bind<&XUnmanaged::onSetGeometry>(
            &xs->events.set_geometry, u);
        if (xs->surface)
            u->onAssociate(nullptr);
        return;
    }

    auto *t     = new Toplevel{};
    t->wm       = this;
    t->xsurface = xs;
    t->tree     = wlr_scene_tree_create(m_windowTree);
    t->id       = m_globalTopLevelId++;

    for (auto &r : t->border)
        r = wlr_scene_rect_create(t->tree, 1, 1,
                                  m_config.border.unfocused_color);

    t->tree->node.data = t;
    t->associate.bind<&Toplevel::onAssociate>(&xs->events.associate, t);
    t->dissociate.bind<&Toplevel::onDissociate>(&xs->events.dissociate, t);
    t->destroy.bind<&Toplevel::onDestroy>(&xs->events.destroy, t);
    t->requestFullscreen.bind<&Toplevel::onRequestFullscreen>(
        &xs->events.request_fullscreen, t);
    t->requestConfigure.bind<&Toplevel::onRequestConfigure>(
        &xs->events.request_configure, t);
    t->setTitle.bind<&Toplevel::onSetTitle>(&xs->events.set_title, t);
    t->setAppId.bind<&Toplevel::onSetAppId>(&xs->events.set_class, t);

    if (xs->surface)
        t->onAssociate(nullptr); // it already has a surface
}

// An X11 menu that wants the keyboard (arrow keys, type to search) gets it
// while it is open. The window underneath stays "focused" and keeps its
// activated state, so its title doesn't flicker; it gets the keyboard back
// when the menu closes.
void
MuWM::focusUnmanaged(XUnmanaged *u)
{
    if (m_locked || m_focusedUnmanaged == u)
        return;
    m_focusedUnmanaged = u;

    wlr_xwayland_surface_activate(u->xsurface, true);
    if (wlr_keyboard *kb = wlr_seat_get_keyboard(m_seat))
        wlr_seat_keyboard_notify_enter(m_seat, u->xsurface->surface,
                                       kb->keycodes, kb->num_keycodes,
                                       &kb->modifiers);
}

void
MuWM::releaseUnmanagedFocus(XUnmanaged *u)
{
    if (m_focusedUnmanaged != u)
        return;
    m_focusedUnmanaged = nullptr;

    if (u->xsurface->surface) // still alive: it is closing, not being freed
        wlr_xwayland_surface_activate(u->xsurface, false);
    if (!m_locked)
        restoreKeyboardFocus(); // layer surface, window, or nothing
}

// The pointer focus and cursor image are only updated on motion. When windows
// change under a stationary cursor, redo that now.
void
MuWM::refreshPointerFocus()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    processMotion(
        static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000),
        false);
}

void
MuWM::onActivationRequest(void *data)
{
    auto *ev
        = static_cast<wlr_xdg_activation_v1_request_activate_event *>(data);

    Toplevel *target = toplevelFromSurface(ev->surface);
    if (!target)
        return;

    // Anti focus-stealing: only honour tokens requested by the window that has
    // focus right now. Anything else should only be marked as urgent.
    wlr_surface *focused = m_seat->keyboard_state.focused_surface;
    if (ev->token->surface && ev->token->surface != focused)
    {
        if (target->workspace && target->workspace->handle)
            wlr_ext_workspace_handle_v1_set_urgent(target->workspace->handle,
                                                   true);
        return;
    }

    activateToplevel(target);
}

void
MuWM::onNewConstraint(void *data)
{
    auto *c = static_cast<wlr_pointer_constraint_v1 *>(data);
    if (c->surface == m_seat->pointer_state.focused_surface)
        updateConstraint(c->surface);
}

void
MuWM::updateConstraint(wlr_surface *focus)
{
    // A window under the cursor must not lock the pointer unless it also has
    // keyboard focus.
    if (focus != m_seat->keyboard_state.focused_surface)
        focus = nullptr;
    wlr_pointer_constraint_v1 *c
        = focus ? wlr_pointer_constraints_v1_constraint_for_surface(
                      m_constraints, focus, m_seat)
                : nullptr;
    if (c == m_activeConstraint)
        return;

    deactivateConstraint();
    if (!c)
        return;

    m_activeConstraint = c;
    m_constraintDestroy.bind<&MuWM::onConstraintDestroy>(&c->events.destroy,
                                                         this);
    wlr_pointer_constraint_v1_send_activated(c);
}

void
MuWM::deactivateConstraint()
{
    wlr_pointer_constraint_v1 *c = m_activeConstraint;
    if (!c)
        return;
    m_activeConstraint = nullptr;
    m_constraintDestroy.unbind();

    // Put the cursor where the client says it was, so it doesn't jump.
    if (c->current.committed & WLR_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT)
    {
        wlr_surface *s = nullptr;
        double sx = 0, sy = 0;
        if (toplevelAt(m_cursor->x, m_cursor->y, &s, &sx, &sy)
            && s == c->surface)
            wlr_cursor_warp(m_cursor, nullptr,
                            m_cursor->x - sx + c->current.cursor_hint.x,
                            m_cursor->y - sy + c->current.cursor_hint.y);
    }
    wlr_pointer_constraint_v1_send_deactivated(c);
}

void
MuWM::onConstraintDestroy(void *)
{
    m_activeConstraint = nullptr; // wlroots frees it; don't send deactivated
    m_constraintDestroy.unbind();
}

bool
MuWM::constrainMotion(double &dx, double &dy)
{
    wlr_pointer_constraint_v1 *c = m_activeConstraint;
    if (!c)
        return true;
    if (c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
        return false;

    // Confined: stop at the edge of the allowed region.
    wlr_surface *s = nullptr;
    double sx = 0, sy = 0;
    if (!toplevelAt(m_cursor->x, m_cursor->y, &s, &sx, &sy) || s != c->surface)
        return true;
    double nx, ny;
    if (wlr_region_confine(&c->region, sx, sy, sx + dx, sy + dy, &nx, &ny))
    {
        dx = nx - sx;
        dy = ny - sy;
    }
    return true;
}

void
MuWM::onWorkspaceCommit(void *data)
{
    auto *ev = static_cast<wlr_ext_workspace_v1_commit_event *>(data);

    wlr_ext_workspace_v1_request *req;
    wl_list_for_each(req, ev->requests, link)
    {
        // Only activation is advertised; ignore everything else.
        if (req->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE)
            continue;
        if (!req->activate.workspace) // destroyed meanwhile
            continue;

        auto *ws = static_cast<Workspace *>(req->activate.workspace->data);
        if (ws && ws->output)
            switchWorkspace(ws->output, ws);
    }
}

void
MuWM::changeVt(int vt)
{
    if (m_session)
    {
        wlr_session_change_vt(m_session, vt);

        if (m_session->active)
            arrange();
    }
}

void
MuWM::onSessionActive(void *)
{
    if (!m_session->active)
        return; // we just lost the VT
    for (Output *o : m_outputs)
        wlr_output_schedule_frame(o->output);
    arrange();
}

void
MuWM::activateToplevel(Toplevel *t)
{
    if (!t || !t->workspace)
        return;
    Workspace *ws = t->workspace;
    if (ws->output && ws != ws->output->workspace)
        switchWorkspace(ws->output, ws); // bring its workspace on screen first
    focusToplevel(t);
}

void
MuWM::notifyActivity()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
    const int64_t ms = int64_t(now.tv_sec) * 1000 + now.tv_nsec / 1'000'000;
    if (ms - m_lastActivityMs < 100)
        return; // idle timeouts are seconds long; no need to reset them at kHz
    m_lastActivityMs = ms;
    wlr_idle_notifier_v1_notify_activity(m_idleNotifier, m_seat);
}

void
MuWM::onNewInhibitor(void *data)
{
    auto *inh    = static_cast<wlr_idle_inhibitor_v1 *>(data);
    auto *i      = new IdleInhibitor{};
    i->wm        = this;
    i->inhibitor = inh;
    i->destroy.bind<&IdleInhibitor::onDestroy>(&inh->events.destroy, i);
    updateIdleInhibit();
}

// Inhibit while at least one inhibitor's window is on screen.
void
MuWM::updateIdleInhibit(const wlr_idle_inhibitor_v1 *ignore)
{
    bool inhibit = false;
    wlr_idle_inhibitor_v1 *inh;
    wl_list_for_each(inh, &m_idleInhibit->inhibitors, link)
    {
        if (inh == ignore)
            continue;

        wlr_surface *root  = wlr_surface_get_root_surface(inh->surface);
        // Not a window we manage (a layer surface): count it.
        const Toplevel *t  = toplevelFromSurface(root);
        const bool visible = !t || !t->workspace || !t->workspace->output
                             || t->workspace == t->workspace->output->workspace;

        inhibit = inhibit || visible;
    }
    wlr_idle_notifier_v1_set_inhibited(m_idleNotifier, inhibit);
}

void
MuWM::onOutputPowerSetMode(void *data)
{
    auto *ev = static_cast<wlr_output_power_v1_set_mode_event *>(data);

    const bool enable = ev->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON;
    if (ev->output->enabled == enable)
        return;

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, enable);
    wlr_output_commit_state(ev->output, &state);
    wlr_output_state_finish(&state);

    if (enable)
        wlr_output_schedule_frame(ev->output); // restart the frame loop
}

bool
MuWM::wantsTearing(const Output *o) const
{
    if (!o->workspace)
        return false;

    // Only a fullscreen window may tear, so nothing else can make the desktop
    // tear.
    for (const Toplevel *t : o->workspace->toplevels)
    {
        if (!t->fullscreen)
            continue;

        if (wlr_tearing_control_manager_v1_surface_hint_from_surface(
                m_tearing, t->surface())
            == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC)
            return true;

        // Games that didn't ask can still be recognised by their content type.
        // Video wants vsync, so it is not included.
        if (wlr_surface_get_content_type_v1(m_contentType, t->surface())
            == WP_CONTENT_TYPE_V1_TYPE_GAME)
            return true;
    }
    return false;
}

Output *
MuWM::adjacentOutput(wlr_direction dir)
{
    if (!m_focusedOutput)
        return nullptr;
    wlr_box box;
    wlr_output_layout_get_box(m_layout, m_focusedOutput->output, &box);
    wlr_output *next = wlr_output_layout_adjacent_output(
        m_layout, dir, m_focusedOutput->output, box.x + box.width / 2.0,
        box.y + box.height / 2.0);
    return next && next->data ? static_cast<Output *>(next->data) : nullptr;
}

Workspace *
MuWM::createWorkspace(Output *o, const std::string &name, size_t index)
{
    auto *w      = new Workspace{};
    w->name      = name;
    w->output    = o;
    w->sceneTree = wlr_scene_tree_create(m_windowTree);
    wlr_scene_node_set_enabled(&w->sceneTree->node,
                               false); // the caller shows one

    // The id must stay unique and stable, so use the index, not the name.
    const std::string id
        = std::string(o->output->name) + "-" + std::to_string(index + 1);
    w->handle = wlr_ext_workspace_handle_v1_create(
        m_workspaceManager, id.c_str(),
        EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
    w->handle->data = w;
    wlr_ext_workspace_handle_v1_set_name(w->handle, name.c_str());
    wlr_ext_workspace_handle_v1_set_group(w->handle, o->group);
    const uint32_t coord = static_cast<uint32_t>(index);
    wlr_ext_workspace_handle_v1_set_coordinates(w->handle, &coord, 1);

    o->workspaces.push_back(w);
    return w;
}

void
MuWM::applyWorkspaces()
{
    const auto &names = m_config.workspaces;

    for (Output *o : m_outputs)
    {
        // Shrinking: first make sure we are not sitting on a workspace that
        // is about to disappear, then move its windows to the last kept one.
        while (o->workspaces.size() > names.size())
        {
            Workspace *gone = o->workspaces.back();
            Workspace *keep = o->workspaces[names.size() - 1];

            if (o->workspace == gone)
                switchWorkspace(o, keep);
            for (Toplevel *t : std::vector<Toplevel *>(gone->toplevels))
                toplevelToWorkspace(t, keep);

            wlr_ext_workspace_handle_v1_destroy(gone->handle);
            wlr_scene_node_destroy(&gone->sceneTree->node);
            o->workspaces.pop_back();
            delete gone;
        }

        // Renaming what stays, growing what is missing.
        for (size_t i = 0; i < names.size(); i++)
        {
            if (i < o->workspaces.size())
            {
                o->workspaces[i]->name = names[i];
                wlr_ext_workspace_handle_v1_set_name(o->workspaces[i]->handle,
                                                     names[i].c_str());
            }
            else
                createWorkspace(o, names[i], i);
        }
    }
    arrange();
}

void
MuWM::focus(wlr_direction dir)
{
    // Focus next window if it exists, else next output if it exists, else
    // nothing, in the given direction.
    if (Toplevel *t = nextToplevel(dir))
        focusToplevel(t);
    else
        focusOutput(dir);
}

Toplevel *
MuWM::nextToplevel(wlr_direction dir)
{
    Workspace *ws = currentWorkspace();
    if (!ws)
        return nullptr;

    Toplevel *t = ws->focusedToplevel;
    if (!t)
        return nullptr;

    auto boxOf = [](Toplevel *w)
    {
        const wlr_box g = w->geometry();
        return wlr_box{w->tree->node.x, w->tree->node.y, g.width, g.height};
    };

    // The nearest window lying entirely in the given direction.
    const wlr_box a = boxOf(t);
    Toplevel *next  = nullptr;
    int best        = 0;
    for (Toplevel *cand : ws->toplevels)
    {
        if (cand == t)
            continue;
        const wlr_box b = boxOf(cand);
        int dist;
        switch (dir)
        {
            case WLR_DIRECTION_LEFT:
                dist = a.x - (b.x + b.width);
                break;
            case WLR_DIRECTION_RIGHT:
                dist = b.x - (a.x + a.width);
                break;
            case WLR_DIRECTION_UP:
                dist = a.y - (b.y + b.height);
                break;
            case WLR_DIRECTION_DOWN:
                dist = b.y - (a.y + a.height);
                break;
            default:
                continue;
        }
        if (dist >= 0 && (!next || dist < best))
        {
            next = cand;
            best = dist;
        }
    }

    return next;
}

void
MuWM::onRequestStartDrag(void *data)
{
    auto *ev = static_cast<wlr_seat_request_start_drag_event *>(data);
    if (m_locked
        || !wlr_seat_validate_pointer_grab_serial(m_seat, ev->origin,
                                                  ev->serial))
    {
        wlr_data_source_destroy(ev->drag->source);
        return;
    }
    wlr_seat_start_pointer_drag(m_seat, ev->drag, ev->serial);
}

void
MuWM::onStartDrag(void *data)
{
    auto *drag = static_cast<wlr_drag *>(data);
    if (drag->icon)
    {
        wlr_scene_drag_icon_create(m_dragIconTree, drag->icon);
        wlr_scene_node_set_position(&m_dragIconTree->node, m_cursor->x,
                                    m_cursor->y);
    }
    m_dragIconDestroy.bind<&MuWM::onDragIconDestroy>(&drag->events.destroy,
                                                     this);
    deactivateConstraint(); // a locked pointer can't carry a drag
}

void
MuWM::onDragIconDestroy(void *)
{
    m_dragIconDestroy.unbind(); // match your Listener API
    refreshPointerFocus();      // the surface under the cursor may have changed
}

void
MuWM::onPinchBegin(void *data)
{
    auto *ev = static_cast<wlr_pointer_pinch_begin_event *>(data);
    notifyActivity();
    wlr_pointer_gestures_v1_send_pinch_begin(m_pointerGestures, m_seat, ev->time_msec,
                                             ev->fingers);
}

void
MuWM::onPinchUpdate(void *data)
{
    auto *ev = static_cast<wlr_pointer_pinch_update_event *>(data);
    wlr_pointer_gestures_v1_send_pinch_update(m_pointerGestures, m_seat, ev->time_msec,
                                              ev->dx, ev->dy, ev->scale,
                                              ev->rotation);
}

void
MuWM::onPinchEnd(void *data)
{
    auto *ev = static_cast<wlr_pointer_pinch_end_event *>(data);
    wlr_pointer_gestures_v1_send_pinch_end(m_pointerGestures, m_seat, ev->time_msec,
                                           ev->cancelled);
}
