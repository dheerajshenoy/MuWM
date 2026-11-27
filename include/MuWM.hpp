#pragma once

#include "Binding.hpp"
#include "Config.hpp"
#include "EventBus.hpp"
#include "GestureBinding.hpp"
#include "Listener.hpp"
#include "SwipeState.hpp"
#include "Toplevel.hpp"
#include "Workspace.hpp"
#include "argparse.hpp"
#include "lua.hpp"
#include "wlroots.hpp"

#include <optional>
#include <vector>

class MuWM;
struct LayerShellSurface;
struct LockSurface;
struct XUnmanaged;
struct Keyboard;
struct Pointer;
struct ClosingWindow;

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

    enum class AnimKind
    {
        Move = 0,
        Open,
        Close,
        Workspace,
    };

    struct AnimParams
    {
        bool on                   = false;
        int duration_ms           = 0;
        muwm::EasingFunction ease = muwm::easing::out_cubic;
        AnimStyle style           = AnimStyle::None;
    };

    bool ok() const
    {
        return m_ok;
    }
    bool run(const std::string &startupCmd = {});
    void handleNewToplevel(wlr_xdg_toplevel *xdg);
    void handleNewInput(wlr_input_device *device);
    void focusToplevel(Toplevel *t, bool raise = true);
    WindowInfo windowInfo(const Toplevel *t) const;
    void addToplevel(Toplevel *t);
    void removeToplevel(Toplevel *t);
    void arrange();
    void arrange(Workspace *workspace);
    void removeOutput(Output *output);
    void ensureActiveOutput();
    wlr_ext_foreign_toplevel_list_v1 *foreignToplevelList() const
    {
        return m_foreignList;
    }
    void publishOutputConfig(); // tell wlr-output-management clients (kanshi)
    void loadConfig();
    bool checkConfig(); // load the config without starting; false on errors
    void quit();

    wlr_output_layout *layout() const
    {
        return m_layout;
    }

    void activateToplevel(Toplevel *);
    void notifyActivity();
    void updateIdleInhibit(const wlr_idle_inhibitor_v1 *ignore = nullptr);
    bool wantsTearing(const Output *) const;

    void focusNext();
    void focusPrev();
    void focus(wlr_direction dir);
    void swapNext();
    void swapPrev();
    void swapMaster();
    void workspaceNext();
    void workspacePrev();
    void close();
    void close(Toplevel *t);
    void focusWorkspace(int index);
    void focusWorkspace(const std::string &name);
    // `focus`: follow the window (switch to its new workspace and focus it).
    // nullopt uses the focus_moved_window config option.
    using MoveFocus = std::optional<bool>;
    void windowToWorkspace(int index, MoveFocus focus = std::nullopt);
    void windowToWorkspace(const std::string &name,
                           MoveFocus focus = std::nullopt);
    void windowToWorkspace(Toplevel *t, int index,
                           MoveFocus focus = std::nullopt);
    void windowToWorkspace(Toplevel *t, const std::string &name,
                           MoveFocus focus = std::nullopt);
    void windowToWorkspace(Toplevel *t, Workspace *ws,
                           MoveFocus focus = std::nullopt);
    void toggleFullscreen();
    void toggleFullscreen(Toplevel *t);
    void toggleFloating();
    void toggleFloating(Toplevel *t);
    void setFloating(Toplevel *, bool floating);
    void setFullscreen(Toplevel *, bool fullscreen);
    void setSticky(Toplevel *, bool sticky);
    void setMasterRatio(Workspace *ws, float ratio);
    void changeVt(int vt);
    void focusOutput(wlr_direction dir);
    void focusOutput(Output *out);
    void moveToOutput(wlr_direction dir);
    void moveToOutput(Toplevel *t, Output *out);

    // What the Lua API needs to find and change things by handle.
    wl_event_loop *eventLoop() const
    {
        return wl_display_get_event_loop(m_display);
    }
    Output *focusedOutput() const
    {
        return m_focusedOutput;
    }
    const std::vector<Output *> &outputs() const
    {
        return m_outputs;
    }
    std::vector<Toplevel *> allToplevels() const;
    WorkspaceInfo workspaceInfo(const Workspace *ws) const;
    OutputInfo outputInfo(const Output *out) const;
    Workspace *workspaceById(uint64_t id);
    Output *outputByName(const std::string &name);
    Workspace *workspaceAt(Output *o,
                           size_t slot); // for on demand created workspaces
    Workspace *workspaceNamed(Output *o, const std::string &name);
    void switchWorkspace(Output *, Workspace *);
    void toplevelToWorkspace(Toplevel *, Workspace *);
    void setFocusedOutput(Output *out);
    Output *adjacentOutput(Output *from, wlr_direction dir);
    void clearLuaTimers();

    enum Change
    {
        ChangeNone      = 0,
        ChangeLayout    = 1 << 0, // gaps, border width
        ChangeRecolor   = 1 << 1, // border colors
        ChangePointer   = 1 << 2, // touchpad / pointer options
        ChangeKeyboard  = 1 << 3, // keymap, repeat rate
        ChangeWorkspace = 1 << 4, // workspaces name / count
        ChangeCursor    = 1 << 5, // cursor theme / size
        ChangeAnimation = 1 << 6, // animation enabled / duration
        ChangeShadow    = 1 << 7, // shadow color / blur / offset / enabled
    };

    void scheduleApply(int changes);

    // Settings are applied to a device when it appears and again, to every
    // device of that kind, when the matching option changes.
    void applyKeyboardConfig(Keyboard *keyboard);
    void applyPointerConfig(Pointer *pointer);
    void applyDeviceConfig(int changes);
    void removeKeyboard(Keyboard *keyboard);
    void removePointer(Pointer *pointer);

    void arrangeLayers(wlr_output *output, bool relayout = true);
    void moveLayerSurface(LayerShellSurface *s);
    void focusLayerSurface(LayerShellSurface *s);
    void releaseLayerFocus(LayerShellSurface *s);
    void removeLayerShellSurface(LayerShellSurface *s);
    void closeLayerSurfacesOn(wlr_output *output);
    wlr_scene_tree *windowTree() const
    {
        return m_windowTree;
    }
    bool locked() const
    {
        return m_locked;
    }
    void focusUnmanaged(XUnmanaged *u);
    void releaseUnmanagedFocus(XUnmanaged *u);
    void lockSurfaceMapped(LockSurface *s);
    void lockSurfaceDestroyed(LockSurface *s);
    bool handleKeybinding(uint32_t mods, xkb_keysym_t sym);
    wlr_seat *seat() const
    {
        return m_seat;
    }

    Toplevel *toplevelAt(double x, double y, wlr_surface **surface, double *sx,
                         double *sy);
    Toplevel *toplevelFromSurface(const wlr_surface *surface);
    void processMotion(uint32_t timeMsec, bool allowFocus = true);
    void spawn(const std::string &cmd);
    void addBinding(Binding b);
    void clearBindings();
    // A swipe with `fingers` fingers in direction `dir`. An action is a Lua
    // callback (a registry ref) or a native function. Binding the same swipe
    // again replaces the old action.
    void addGesture(int fingers, wlr_direction dir, int ref,
                    std::function<void()> native = {});
    void clearGestures();

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

    wlr_foreign_toplevel_manager_v1 *foreignToplevelManager() const
    {
        return m_foreignToplevelManager;
    }

    wlr_security_context_manager_v1 *securityContext() const
    {
        return m_securityContext;
    }
    Toplevel *toplevelById(uint64_t id);
    void scheduleFrames();
    bool tickAnimations(int64_t now);
    void animateClose(Toplevel *t);

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
    void onCursorShape(void *data);
    void onRequestSetCursor(void *data);
    void onToplevelCaptureRequest(void *data);
    void onOutputManagerApply(void *data);
    void onOutputManagerTest(void *data);
    void onNewPopup(void *data);
    bool applyOutputConfig(wlr_output_configuration_v1 *config, bool test);
    void onRequestSetSelection(void *data);
    void onRequestSetPrimarySelection(void *data);
    void onNewLock(void *data);
    void onXwaylandReady(void *data);
    void onNewXWaylandSurface(void *data);
    void onLockNewSurface(void *data);
    void onLockUnlock(void *data);
    void onLockDestroy(void *data);
    void onActivationRequest(void *data);
    void onNewConstraint(void *data);
    void onConstraintDestroy(void *data);
    void onWorkspaceCommit(void *data);
    void onSessionActive(void *data);
    void onNewInhibitor(void *data);
    void onKeyboardInhibit(void *data);
    void onOutputPowerSetMode(void *data);
    void onRequestStartDrag(void *data);
    void onStartDrag(void *data);
    void onDragIconDestroy(void *data);
    void onPinchBegin(void *data);
    void onPinchUpdate(void *data);
    void onPinchEnd(void *data);
    void runGesture(int fingers, wlr_direction dir);
    void onSwipeBegin(void *data);
    void onSwipeUpdate(void *data);
    void onSwipeEnd(void *data);
    void onHoldBegin(void *data);
    void onHoldEnd(void *data);
    void onSetIcon(void *data);
    void onNewDialog(void *data);
    void onToplevelTagSet(void *data);
    void onToplevelTagDescriptionSet(void *data);
    void onBellRing(void *data);

    void updateConstraint(wlr_surface *focus);
    void deactivateConstraint();
    void refreshPointerFocus();
    bool constrainMotion(double &dx, double &dy); // false = pointer is locked
    void startLock();
    void createLockCover(Output *);
    void layoutLock();
    void focusLockSurface();
    void checkLockReady();
    void restoreKeyboardFocus();
    void initLua();
    void initLuaBindings();
    void cycleWorkspace(int delta);
    void evacuateInactiveOutputs();
    void updateFullscreenLayers(Output *);
    void placeFloating(Toplevel *);
    void raiseFloaters(Workspace *);
    void beginGrab(Toplevel *, bool resize);
    void endGrab();
    void insertNewTopLevel(std::vector<Toplevel *> &toplevels, Toplevel *t);
    Output *adjacentOutput(wlr_direction dir);
    Workspace *createWorkspace(Output *o, const std::string &name,
                               size_t index);
    void applyWorkspaces();
    void applyCursor();
    Toplevel *nextToplevel(wlr_direction dir);
    void clearUrgent(Workspace *ws);
    void markUrgent(Workspace *ws);
    void updateWorkspaceHidden(Workspace *ws);
    void playBellSound();

private:
    bool m_ok           = false;
    bool m_checkOnly    = false; // --check-config: no backend, no side effects
    bool m_configFailed = false; // the last loadConfig() hit an error
    void failConfig();
    void installDefaultBindings();
    const argparse::ArgumentParser &m_parser;

    wl_display *m_display                                     = nullptr;
    wlr_backend *m_backend                                    = nullptr;
    wlr_renderer *m_renderer                                  = nullptr;
    wlr_compositor *m_compositor                              = nullptr;
    wlr_xwayland *m_xwayland                                  = nullptr;
    wlr_allocator *m_allocator                                = nullptr;
    wlr_scene *m_scene                                        = nullptr;
    wlr_output_layout *m_layout                               = nullptr;
    wlr_scene_output_layout *m_sceneLayout                    = nullptr;
    wlr_seat *m_seat                                          = nullptr;
    wlr_xdg_shell *m_xdgShell                                 = nullptr;
    wlr_cursor *m_cursor                                      = nullptr;
    wlr_xcursor_manager *m_xcursor                            = nullptr;
    wlr_xdg_toplevel_icon_manager_v1 *m_iconManager           = nullptr;
    wlr_xdg_decoration_manager_v1 *m_decoManager              = nullptr;
    wlr_layer_shell_v1 *m_layerShell                          = nullptr;
    wlr_output_manager_v1 *m_outputManager                    = nullptr;
    wlr_xdg_toplevel_tag_manager_v1 *m_tagManager             = nullptr;
    wlr_cursor_shape_manager_v1 *m_cursorShapeManager         = nullptr;
    wlr_ext_foreign_toplevel_list_v1 *m_foreignList           = nullptr;
    wlr_session_lock_manager_v1 *m_lockManager                = nullptr;
    wlr_session *m_session                                    = nullptr;
    wlr_session_lock_v1 *m_lock                               = nullptr;
    wlr_xdg_activation_v1 *m_xdgActivation                    = nullptr;
    wlr_ext_workspace_manager_v1 *m_workspaceManager          = nullptr;
    wlr_pointer_constraints_v1 *m_constraints                 = nullptr;
    wlr_relative_pointer_manager_v1 *m_relativePointer        = nullptr;
    wlr_pointer_constraint_v1 *m_activeConstraint             = nullptr;
    wlr_foreign_toplevel_manager_v1 *m_foreignToplevelManager = nullptr;
    wlr_linux_dmabuf_v1 *m_dmabuf                             = nullptr;
    wlr_idle_notifier_v1 *m_idleNotifier                      = nullptr;
    wlr_idle_inhibit_manager_v1 *m_idleInhibit                = nullptr;
    wlr_output_power_manager_v1 *m_outputPower                = nullptr;
    wlr_gamma_control_manager_v1 *m_gammaControl              = nullptr;
    wlr_content_type_manager_v1 *m_contentType                = nullptr;
    wlr_tearing_control_manager_v1 *m_tearing                 = nullptr;
    wlr_security_context_manager_v1 *m_securityContext        = nullptr;
    wlr_pointer_gestures_v1 *m_pointerGestures                = nullptr;
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1
        *m_toplevelCaptureManager = nullptr;
    wlr_keyboard_shortcuts_inhibit_manager_v1 *m_shortcutInhibitManager
        = nullptr;
    wlr_xdg_wm_dialog_v1 *m_dialogManager = nullptr;
    wlr_xdg_system_bell_v1 *m_systemBell  = nullptr;

    Output *m_focusedOutput = nullptr;
    std::vector<Output *> m_outputs;
    std::vector<Keyboard *> m_keyboards;
    std::vector<Pointer *> m_pointers;
    Workspace m_unassigned; // for toplevels that don't have an output yet

    OutputListener m_outputListener;
    Listener<MuWM> m_setIcon;
    Listener<MuWM> m_pinchBegin;
    Listener<MuWM> m_pinchUpdate;
    Listener<MuWM> m_pinchEnd;
    Listener<MuWM> m_swipeBegin;
    Listener<MuWM> m_swipeUpdate;
    Listener<MuWM> m_swipeEnd;
    Listener<MuWM> m_holdBegin;
    Listener<MuWM> m_holdEnd;
    Listener<MuWM> m_cursorMotion;
    Listener<MuWM> m_cursorMotionAbs;
    Listener<MuWM> m_cursorButton;
    Listener<MuWM> m_cursorAxis;
    Listener<MuWM> m_cursorFrame;
    Listener<MuWM> m_cursorShape;
    Listener<MuWM> m_requestSetCursor;
    Listener<MuWM> m_newInput;
    Listener<MuWM> m_newToplevel;
    Listener<MuWM> m_newLayerShellSurface;
    Listener<MuWM> m_layoutChange;
    Listener<MuWM> m_newDecoration;
    Listener<MuWM> m_captureRequest;
    Listener<MuWM> m_outputApply;
    Listener<MuWM> m_outputTest;
    Listener<MuWM> m_requestSetSelection;
    Listener<MuWM> m_requestSetPrimary;
    Listener<MuWM> m_newPopup;
    Listener<MuWM> m_newLock;
    Listener<MuWM> m_xwaylandReady;
    Listener<MuWM> m_xwaylandNewSurface;
    Listener<MuWM> m_lockNewSurface;
    Listener<MuWM> m_lockUnlock;
    Listener<MuWM> m_lockDestroy;
    Listener<MuWM> m_activationRequest;
    Listener<MuWM> m_newConstraint;
    Listener<MuWM> m_constraintDestroy;
    Listener<MuWM> m_workspaceCommit;
    Listener<MuWM> m_sessionActive;
    Listener<MuWM> m_newInhibitor;
    Listener<MuWM> m_outputPowerSetMode;
    Listener<MuWM> m_requestStartDrag;
    Listener<MuWM> m_startDrag;
    Listener<MuWM> m_dragIconDestroy;
    Listener<MuWM> m_keyboardInhibit;
    Listener<MuWM> m_toplevelTagSet;
    Listener<MuWM> m_toplevelTagDescriptionSet;
    Listener<MuWM> m_newDialog;
    Listener<MuWM> m_bellRing;

    // Super+drag: left button moves a window, right button resizes it.
    enum class Grab
    {
        None,
        Move,
        Resize
    };
    Grab m_grab         = Grab::None;
    Toplevel *m_grabbed = nullptr;
    bool m_grabWasTiled = false; // the grabbed window was a tile when picked up
    double m_grabX      = 0;     // cursor offset (move) or start point (resize)
    double m_grabY      = 0;
    wlr_box m_grabBox   = {}; // floatBox when the grab started

    bool m_cursorIsDefault = false; // skip redundant resets
    Config m_config;
    std::vector<LayerShellSurface *> m_layerSurfaces;
    // An X11 menu that asked for the keyboard; it keeps it until it closes.
    XUnmanaged *m_focusedUnmanaged    = nullptr;
    LayerShellSurface *m_focusedLayer = nullptr; // holds the keyboard, if any

    // Stacking order, bottom to top: background, bottom, windows, top, overlay.
    // The four layer trees are indexed by zwlr_layer_shell_v1_layer.
    wlr_scene_tree *m_layerTrees[4] = {};
    wlr_scene_tree *m_windowTree    = nullptr;
    wlr_scene_tree *m_lockTree = nullptr; // covers and lock surfaces, on top
    wlr_scene_tree *m_dragIconTree
        = nullptr; // icon of the current drag, on top

    // m_locked stays true until the locker really unlocks, even if the locker
    // process dies: then m_lock is null but the screen stays covered.
    bool m_locked   = false;
    bool m_lockSent = false; // `locked` has been sent to the current locker
    std::vector<LockSurface *> m_lockSurfaces;

    lua_State *m_lua = nullptr;

    void applyPending();
    void recolorBorders();
    int m_pendingChanges         = ChangeNone;
    wl_event_source *m_applyIdle = nullptr;

    std::vector<Toplevel *> m_tiled; // scratch for arrange(); not re-entrant
    std::vector<Binding> m_keybindings;
    EventBus m_eventBus;

    uint64_t m_globalWorkspaceId = 0; // ids for Workspace::id, from 1
    uint64_t m_globalTopLevelId
        = 0; // incremented for each new toplevel, used to identify it in events

    int64_t m_lastActivityMs = -1'000'000;
    int64_t m_lastBellMs     = -1'000'000;
    std::string m_bellSetting; // the bell_sound value m_bellFile came from
    std::string m_bellFile;    // resolved sound file; empty means silent
    bool m_bellResolved = false;

    std::vector<GestureBinding> m_gestures;
    std::vector<std::unique_ptr<ClosingWindow>> m_closingWindows;
    SwipeState m_swipe;

    // The effective settings for one kind: per-kind spec merged with the
    // global switch and default duration.
    AnimParams animParams(AnimKind k) const;
};
