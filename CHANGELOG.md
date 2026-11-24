# CHANGELOG

## 0.1.0

### Compositor
- wlroots-based tiling Wayland compositor with a master/stack layout.
  Master gets 55% by default. The stack shares the rest evenly.
- The master ratio is kept per workspace on each output. `layout.master_ratio`
  is the default, and `mu.set_master_ratio` changes the current workspace only.
- Focus, swap and close window actions.
- Focus borders with active and inactive colors.
- Smart gaps and smart borders for a single window.
- Server-side decorations via xdg-decoration.
- Spawn programs from keybindings and at startup.
- Quit with `Super+Shift+C`. Launch `$TERMINAL` with `Super+Shift+Return`.
  It falls back to foot, kitty, alacritty or xterm.
- Default bindings are installed even without a config or when the config fails.

### Windows
- Multiple workspaces per output, with next and previous switching.
- Fullscreen from client requests and `toggle_fullscreen`.
- Floating windows with `toggle_floating`.
- Dialogs and fixed-size windows float automatically.
- Move a window with `Super` and left drag. Resize it with `Super` and right drag.
- A window dragged to another monitor moves to that monitor's workspace.
- Popups such as menus and tooltips.
- Closing a window focuses the one used before it, per workspace.
- A window sent to a hidden workspace leaves the keyboard on the old one.
- XWayland: X11 apps run, are tiled like other windows, and use the same
  floating and fullscreen rules. Xwayland starts on first use and `DISPLAY` is
  set for programs you spawn.
- X11 menus and tooltips place themselves. Menus that need it get the keyboard.

### Multi-monitor
- Independent outputs, each with its own workspaces and usable area.
- Windows of a removed or disabled output move to a remaining one.
- The focused output follows the focused window.

### Layer shell
- `wlr-layer-shell-unstable-v1` support for wallpapers, bars and launchers.
- Layers stack as background, bottom, top and overlay.
- Windows are tiled inside the area left free by exclusive-zone surfaces.
- Surfaces that ask for the keyboard, such as rofi, get focus.
  Focus returns to the window when they close.

### Protocols
- Output management, so `kanshi` and `wlr-randr` work.
- Screen capture of outputs and single windows.
  This covers screencopy and ext-image-copy-capture.
- Clipboard: primary selection and data control.
- Cursor shape and client-set cursors.
- Session lock (`ext-session-lock-v1`), for lockers such as `swaylock`.
  Every monitor is covered at once. Keybindings, focus and the pointer are
  blocked. If the locker dies, the session stays locked.
- Pointer events reach layer-shell surfaces such as bars.
- Drag and drop between clients, with the drag icon following the pointer.
  Drags are refused while locked and end if the session locks mid-drag.

### Input
- Keyboard layout, variant, options and repeat settings.
- Touchpad options such as natural scroll and tap to click.
- Settings can change at runtime and are reapplied to all devices.

### Lua configuration
- `mu` global table with `mu.opts` and functions such as `mu.keybind`,
  `mu.spawn`, `mu.reload`, focus, swap, close and quit.
- Workspace functions, `toggle_fullscreen` and `toggle_floating`.
- `mu.focus(dir)` focuses the nearest window in a direction (`up`, `down`,
  `left` or `right`), or the next output when there is none.
- `mu.opts.new_client_position` sets where new windows go:
  `after_current`, `before_current`, `top` or `bottom`.
- String colors such as `"#rrggbb"`.
- Option changes take effect immediately.
- Config reload clears duplicate bindings. A failing config falls back to the
  default bindings.
- Events: `mu.on`, `mu.once` and `mu.off`.
  Available events are `startup`, `config_reload`, `window_open` and
  `window_close`. The window events carry typed payloads.
- A generated LuaLS stub for editor completion.

### Infrastructure
- File logging to `$XDG_STATE_HOME/muwm/muwm.log`, rotated to `.old`.
  Verbosity is set with `MUWM_LOG`.
- CMake generates protocol headers with `add_protocol()` and the
  `muwm_protocols` target.

### Fixes
- Crashes when closing a window, quitting, and on startup events.
- Decoration mode was applied before the surface was initialized.
- First-window keyboard focus.
