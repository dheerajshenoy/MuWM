#pragma once

#include "Config.hpp"
#include "Listener.hpp"
#include "Toplevel.hpp"
#include "argparse.hpp"
#include "lua.hpp"
#include "wlroots.hpp"

#include <vector>

class MuWM;

struct OutputListener
{
    wl_listener listener{}; // must stay first
    MuWM *wm = nullptr;
};

class MuWM
{

public:
    MuWM(const argparse::ArgumentParser &parser);
    ~MuWM();

    bool ok() const
    {
        return m_ok;
    }
    bool run(const std::string &startupCmd = {});
    void handleNewToplevel(wlr_xdg_toplevel *xdg);
    void handleNewInput(wlr_input_device *device);
    void focusToplevel(Toplevel *t);
    void addToplevel(Toplevel *t);
    void removeToplevel(Toplevel *t);
    void arrange();
    bool handleKeybinding(xkb_keysym_t sym);
    wlr_seat *seat() const
    {
        return m_seat;
    }

    Toplevel *toplevelAt(double x, double y, wlr_surface **surface, double *sx,
                         double *sy);
    void processMotion(uint32_t timeMsec);
    void spawn(const std::string &cmd);

    const Config &config() const
    {
        return m_config;
    }

private:
    bool init();
    void handleNewOutput(wlr_output *output);
    void onNewToplevel(void *data);
    void onNewInput(void *data);
    void onCursorMotion(void *data);
    void onCursorMotionAbs(void *data);
    void onCursorButton(void *data);
    void onCursorAxis(void *data);
    void onCursorFrame(void *data);
    void onLayoutChange(void *data);
    void onNewDecoration(void *data);

    void initLua();
    void initLuaBindings();
    void loadConfig();

private:
    bool m_ok = false;
    const argparse::ArgumentParser &m_parser;

    wl_display *m_display                        = nullptr;
    wlr_backend *m_backend                       = nullptr;
    wlr_renderer *m_renderer                     = nullptr;
    wlr_allocator *m_allocator                   = nullptr;
    wlr_scene *m_scene                           = nullptr;
    wlr_output_layout *m_layout                  = nullptr;
    wlr_scene_output_layout *m_sceneLayout       = nullptr;
    wlr_seat *m_seat                             = nullptr;
    wlr_xdg_shell *m_xdgShell                    = nullptr;
    wlr_cursor *m_cursor                         = nullptr;
    wlr_xcursor_manager *m_xcursor               = nullptr;
    wlr_xdg_decoration_manager_v1 *m_decoManager = nullptr;
    Toplevel *m_focused                          = nullptr;

    OutputListener m_outputListener;

    Listener<MuWM> m_cursorMotion;
    Listener<MuWM> m_cursorMotionAbs;
    Listener<MuWM> m_cursorButton;
    Listener<MuWM> m_cursorAxis;
    Listener<MuWM> m_cursorFrame;
    Listener<MuWM> m_newInput;
    Listener<MuWM> m_newToplevel;
    Listener<MuWM> m_layoutChange;
    Listener<MuWM> m_newDecoration;

    Config m_config;
    std::vector<Toplevel *> m_toplevels;

    lua_State *m_lua = nullptr;
};
