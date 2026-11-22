#pragma once

#include "Listener.hpp"
#include "argparse.hpp"
#include "wlroots.hpp"

class MuWM;

struct OutputListener
{
    wl_listener listener; // must stay first
    MuWM *wm;
};

class MuWM
{

public:
    MuWM(const argparse::ArgumentParser &parser);
    ~MuWM();

    bool run();
    void handleNewToplevel(wlr_xdg_toplevel *xdg);

private:
    bool init();
    void handleNewOutput(wlr_output *output);
    void onNewToplevel(void *data);

private:
    const argparse::ArgumentParser &m_parser;

    wl_display *m_display                  = nullptr;
    wlr_backend *m_backend                 = nullptr;
    wlr_renderer *m_renderer               = nullptr;
    wlr_allocator *m_allocator             = nullptr;
    wlr_scene *m_scene                     = nullptr;
    wlr_output_layout *m_layout            = nullptr;
    wlr_scene_output_layout *m_sceneLayout = nullptr;

    OutputListener m_outputListener;
    // private:
    wlr_xdg_shell *m_xdgShell = nullptr;
    Listener<MuWM> m_newToplevel;
};
