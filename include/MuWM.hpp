#pragma once

#include "Binding.hpp"
#include "Config.hpp"
#include "EventBus.hpp"
#include "Listener.hpp"
#include "Toplevel.hpp"
#include "Workspace.hpp"
#include "argparse.hpp"
#include "lua.hpp"
#include "wlroots.hpp"

#include <vector>

class MuWM;
struct LayerShellSurface;
struct Keyboard;
struct Pointer;

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
    WindowInfo windowInfo(const Toplevel *t) const;
    void addToplevel(Toplevel *t);
    void removeToplevel(Toplevel *t);
    void arrange();
    void arrange(Workspace *workspace);
    void removeOutput(Output *output);
    wlr_ext_foreign_toplevel_list_v1 *foreignToplevelList() const
    {
        return m_foreignList;
    }
    void publishOutputConfig(); // tell wlr-output-management clients (kanshi)
    void loadConfig();
    void quit();

    // Window
    void focusNext();
    void focusPrev();
    void swapNext();
    void swapPrev();
    void swapMaster();
    void workspaceNext();
    void workspacePrev();
    void close();
    void focusWorkspace(int index);
    void windowToWorkspace(int index);
    void toggleFullscreen();
    void toggleFloating();
    void setFloating(Toplevel *, bool floating);
    void setFullscreen(Toplevel *, bool fullscreen);
    void setMasterRatio(float ratio);

    enum Change
    {
        ChangeNone     = 0,
        ChangeLayout   = 1 << 0, // gaps, border width
        ChangeRecolor  = 1 << 1, // border colors
        ChangePointer  = 1 << 2, // touchpad / pointer options
        ChangeKeyboard = 1 << 3, // keymap, repeat rate
    };
    void scheduleApply(int changes);

    // Settings are applied to a device when it appears and again, to every
    // device of that kind, when the matching option changes.
    void applyKeyboardConfig(Keyboard *keyboard);
    void applyPointerConfig(Pointer *pointer);
    void applyDeviceConfig(int changes);
    void removeKeyboard(Keyboard *keyboard);
    void removePointer(Pointer *pointer);

    // layer-shell
    void arrangeLayers(wlr_output *output);
    void moveLayerSurface(LayerShellSurface *s);
    void focusLayerSurface(LayerShellSurface *s);
    void releaseLayerFocus(LayerShellSurface *s);
    void removeLayerShellSurface(LayerShellSurface *s);
    void closeLayerSurfacesOn(wlr_output *output);
    bool handleKeybinding(uint32_t mods, xkb_keysym_t sym);
    wlr_seat *seat() const
    {
        return m_seat;
    }

    Toplevel *toplevelAt(double x, double y, wlr_surface **surface, double *sx,
                         double *sy);
    void processMotion(uint32_t timeMsec);
    void spawn(const std::string &cmd);
    void addBinding(Binding b);
    void clearBindings();

    EventBus &eventBus()
    {
        return m_eventBus;
    }

    const Config &config() const
    {
        return m_config;
    }

    Workspace *currentWorkspace();
    Toplevel *focusedToplevel()
    {
        return currentWorkspace() ? currentWorkspace()->focusedToplevel
                                  : nullptr;
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
    void onNewLayerShellSurface(void *data);
    void onToplevelCaptureRequest(void *data);
    void onOutputManagerApply(void *data);
    void onOutputManagerTest(void *data);
    bool applyOutputConfig(wlr_output_configuration_v1 *config, bool test);

    void initLua();
    void initLuaBindings();
    void cycleWorkspace(int delta);
    void switchWorkspace(Output *, Workspace *);
    void toplevelToWorkspace(Toplevel *, Workspace *);
    void updateFullscreenLayers(Output *);
    void placeFloating(Toplevel *);
    void raiseFloaters(Workspace *);
    void beginGrab(Toplevel *, bool resize);
    void endGrab();

private:
    bool m_ok = false;
    void failConfig();
    void installDefaultBindings();
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
    wlr_layer_shell_v1 *m_layerShell             = nullptr;
    wlr_output_manager_v1 *m_outputManager       = nullptr;
    wlr_ext_foreign_toplevel_list_v1 *m_foreignList = nullptr;
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1
        *m_toplevelCaptureManager = nullptr;
    Output *m_focusedOutput                      = nullptr;
    std::vector<Output *> m_outputs;
    std::vector<Keyboard *> m_keyboards;
    std::vector<Pointer *> m_pointers;
    Workspace m_unassigned; // for toplevels that don't have an output yet

    OutputListener m_outputListener;

    Listener<MuWM> m_cursorMotion;
    Listener<MuWM> m_cursorMotionAbs;
    Listener<MuWM> m_cursorButton;
    Listener<MuWM> m_cursorAxis;
    Listener<MuWM> m_cursorFrame;
    Listener<MuWM> m_newInput;
    Listener<MuWM> m_newToplevel;
    Listener<MuWM> m_newLayerShellSurface;
    Listener<MuWM> m_layoutChange;
    Listener<MuWM> m_newDecoration;
    Listener<MuWM> m_captureRequest;
    Listener<MuWM> m_outputApply;
    Listener<MuWM> m_outputTest;

    // Super+drag: left button moves a window, right button resizes it.
    enum class Grab
    {
        None,
        Move,
        Resize
    };
    Grab m_grab         = Grab::None;
    Toplevel *m_grabbed = nullptr;
    double m_grabX      = 0; // cursor offset (move) or start point (resize)
    double m_grabY      = 0;
    wlr_box m_grabBox   = {}; // floatBox when the grab started

    Config m_config;
    std::vector<LayerShellSurface *> m_layerSurfaces;
    LayerShellSurface *m_focusedLayer = nullptr; // holds the keyboard, if any

    // Stacking order, bottom to top: background, bottom, windows, top, overlay.
    // The four layer trees are indexed by zwlr_layer_shell_v1_layer.
    wlr_scene_tree *m_layerTrees[4] = {};
    wlr_scene_tree *m_windowTree    = nullptr;

    lua_State *m_lua = nullptr;

    void applyPending();
    void recolorBorders();
    int m_pendingChanges         = ChangeNone;
    wl_event_source *m_applyIdle = nullptr;

    std::vector<Binding> m_keybindings;
    EventBus m_eventBus;

    uint64_t m_globalTopLevelId
        = 0; // incremented for each new toplevel, used to identify it in events
};
