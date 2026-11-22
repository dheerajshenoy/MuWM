#include "MuWM.hpp"

#include "Output.hpp"
#include "Toplevel.hpp"

MuWM::MuWM(const argparse::ArgumentParser &parser) : m_parser(parser)
{
    init();
}

MuWM::~MuWM()
{
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

    m_scene = wlr_scene_create();
    if (!m_scene)
    {
        wlr_log(WLR_ERROR, "Failed to create scene");
        return false;
    }

    m_sceneLayout = wlr_scene_attach_output_layout(m_scene, m_layout);

    // temporary dark background so we can see rendering works
    const float bg[4] = {0.1f, 0.1f, 0.15f, 1.0f};
    wlr_scene_rect_create(&m_scene->tree, 4000, 4000, bg);

    wlr_compositor_create(m_display, 5, m_renderer);
    wlr_subcompositor_create(m_display);
    wlr_data_device_manager_create(m_display);

    m_xdgShell = wlr_xdg_shell_create(m_display, 6);
    m_newToplevel.bind<&MuWM::onNewToplevel>(&m_xdgShell->events.new_toplevel,
                                             this);

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

    wlr_log(WLR_INFO, "New output: %s", output->name);
}

bool
MuWM::run()
{
    const char *socket = wl_display_add_socket_auto(m_display);
    if (!socket)
        return false;
    if (!wlr_backend_start(m_backend))
        return false;

    setenv("WAYLAND_DISPLAY", socket, 1);
    wlr_log(WLR_INFO, "Running on WAYLAND_DISPLAY=%s", socket);
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
}
