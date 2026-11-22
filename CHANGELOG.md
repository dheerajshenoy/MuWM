# CHANGELOG

## 0.1.0

### Compositor
- wlroots-based tiling Wayland compositor with a master/stack layout
  (master 55%, stack shares the remainder evenly, no leftover gap).
- Focus, swap and close window actions; focus borders with active/inactive colors.
- Server-side decorations via xdg-decoration (client-side decorations removed).
- Spawning programs from keybindings and at startup.
- Quit with `Super+Shift+C`, and `Super+Shift+Return` to launch `$TERMINAL`
  (falling back to foot, kitty, alacritty or xterm). Both are installed by
  default, even without a config or when the config fails to load.

### Layer shell
- `wlr-layer-shell-unstable-v1` support (vendored protocol XML): wallpapers,
  bars and launchers, stacked in background/bottom/top/overlay scene trees.
- Windows are tiled inside the area left free by exclusive-zone surfaces.
- Layer surfaces that request keyboard interactivity (e.g. rofi) receive
  keyboard focus, and focus is returned to the window when they close.

### Multi-monitor
- Independent outputs, each with its own workspace and usable area.
- Output hotplug: windows of a removed output move to a remaining one.
- Focused output tracked and updated as windows are focused.

### Input
- Keyboard layout, variant, options and repeat settings, applied at startup.
- Touchpad options (natural scroll, tap to click) via libinput.
- Keyboard and pointer settings can be changed at runtime and are reapplied
  to all devices.

### Lua configuration
- `mu` global table with `mu.opts` (gap, border width/colors, touchpad,
  keyboard) and functions such as `mu.keybind`, `mu.spawn`, `mu.reload`,
  focus/swap/close/quit, `workspace_next` and `workspace_prev` (not yet implemented).
- String color support (e.g. `"#rrggbb"`).
- Runtime option changes take effect immediately.
- Config reload (duplicate bindings are cleared); a failing config falls
  back to the default bindings.
- Event system: `mu.on`, `mu.once`, `mu.off` with `startup` and
  `config_reload` events.

### Infrastructure
- File logging to `$XDG_STATE_HOME/muwm/muwm.log` (rotated to `.old`) with
  timestamps; verbosity controlled by `MUWM_LOG`.
- CMake generates protocol headers via `add_protocol()` and the
  `muwm_protocols` target.

### Fixes
- Crashes when closing a window, quitting, and on startup events.
- Decoration mode applied before the surface was initialized.
- First-window keyboard focus.
