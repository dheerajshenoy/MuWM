# CHANGELOG

## 0.1.2

### Animations
- Windows slide to their new place when the layout changes, slide in when
  they open and fade out when they close. Workspace switches slide
  horizontally, in the direction of the workspace order.
- `mu.config.animation.enabled` and `duration` set the defaults. `move`,
  `open`, `close` and `workspace` each take `enabled`, `duration`, `curve`
  (`linear`, `out_cubic`, `in_out_cubic`) and `style` (`none`, `slide`,
  `fade`, `zoom`; open and close only).
- So far only close animations read their own settings; move, open and
  workspace animations still use the global `enabled` and `duration`.

### Shadows
- In scenefx builds, windows cast a blurred shadow behind their borders. It
  hides with the borders, e.g. for fullscreen windows.
- `mu.config.shadow` takes `enabled`, `color`, `opacity`, `floating_only`,
  `focused_only`, `sigma` (blur radius) and `offset_x`/`offset_y`. Changes
  apply to open windows at once. Builds without scenefx accept the settings
  and ignore them, so one `init.lua` works with both.

### Blur
- In scenefx builds, translucent windows can blur what is behind them
  (`mu.config.blur.enabled`, off by default). The blur covers the window's
  surface and hides with its borders, e.g. for fullscreen windows.
- The blur is dual Kawase: `mu.config.blur.passes` (each pass roughly
  doubles its reach) and `radius` (offset per pass) set its strength for all
  windows; `-1` keeps the scenefx default.
- `alpha` sets the blur opacity, `focused_only` blurs only behind the focused
  window, and `bottom_only` blurs only the wallpaper instead of the windows
  behind. `bottom_only` needs a per-output optimized blur that isn't there
  yet. Builds without scenefx accept the settings and ignore them.

### Lua configuration
- `mu.gesture("swipe3+left", action)` binds touchpad swipes. A swipe runs its
  action once the fingers move `gestures.swipe_threshold`.
- `mu.config.mouse` has `accel_profile` and `accel_speed`.
- `mu.config.bell_sound` picks the sound for the system bell.
- `border.sticky` colors the bottom and right edges of sticky windows.
- `muwm --check-config` loads the config, reports errors and exits.
- Default bindings: Super+J/K focus the next/previous window, Super+Shift+J/K
  swap them, Super+N shows workspace N, Super+Shift+N sends the focused
  window there, and Super+Shift+R reloads the config. Config bindings for
  the same keys replace them.
- The LuaLS stubs type-check `mu.config { ... }` calls, so a wrong option
  value is flagged in the editor.

### Windows and workspaces
- Sticky windows (`window.sticky`) follow workspace switches on their output.
- The xdg-system-bell protocol plays the bell with `pw-play`. A bell from a
  window on a hidden workspace marks that workspace urgent.

### Input
- Mice get acceleration, left-handed and rotation settings. Touchpads get
  acceleration, left-handed, clickfinger, 3-finger drag, drag lock and
  button map settings. A keyboard can load a keymap file.
- Swipe and hold gestures are forwarded to clients.

### Build
- `-DWANT_SCENEFX=ON` builds against scenefx instead of the wlroots scene
  graph. Off by default.

### Fixes
- Fixed assertions on exit caused by listeners that were never unbound.
- Popup menus are no longer drawn under their window's border.

## 0.1.1

### Lua configuration
The Lua API was reworked, so a 0.1.0 `init.lua` needs updating:
`mu.opts` is now `mu.config` (and `gaps.value` is `gaps.size`), `mu.keybind`
is `mu.bind`, and event names use dots (`window.open`, `config.reload`).
`mu.focus_next`/`focus_prev` became `mu.focus("next")`/`mu.focus("prev")`,
`swap_next`/`swap_prev`/`swap_master` became `mu.swap(...)`, and
`mu.focus_workspace`, `mu.window_to_workspace` and `mu.set_master_ratio` are
properties and methods of the new objects. `mu.off(id)` is `listener:off()`.

- Options live in `mu.config`, set one by one (`mu.config.gaps.size = 8`) or
  with a table (`mu.config { ... }`).
- Windows, workspaces and outputs are objects with properties: `mu.window`
  (focused), `mu.windows`, `mu.workspace`, `mu.workspaces` (also by name),
  `mu.output`, `mu.outputs` (also by name). State is read and assigned
  (`w.floating = true`, `w.workspace = "web"`, `w.fullscreen`,
  `ws.master_ratio`, `out.workspace`); actions are methods (`w:close()`,
  `w:move_to_workspace(ws, focus)`, `ws:show()`, `out:neighbor("left")`).
  Objects keep working after the window closes (`w.valid`).
- Windows also have `x`, `y`, `width`, `height`, `focused`, `tag` and
  `tag_description`.
- `mu.bind` takes a key and a function or shell command, or a table of them.
- `mu.rule(match, apply)` applies properties to new windows.
- `mu.after` and `mu.every` run functions on a timer and return a timer with
  `cancel()`. `mu.log` writes to the compositor log.
- `mu.focus(where)` takes a direction, `next` or `prev`. `mu.swap` takes
  `next`, `prev` or `master`.
- Events: `mu.on` and `mu.once` return a listener with `off()`. New events are
  `quit`, `window.fullscreen` and `workspace.change`, which passes the new
  workspace.
- New options: `dynamic_workspaces`, `focus_moved_window`, `apply_tearing`,
  `cursor.theme`, `cursor.size`, `touchpad.middle_emulation`,
  `touchpad.disable_while_typing`, `dim` and `opacity`.
- Config reload clears bindings, rules, handlers and timers.

### Windows and workspaces
- Dynamic workspaces: with `dynamic_workspaces`, a workspace is created the
  first time it is focused or receives a window, and empty inactive ones are
  hidden from workspace clients.
- A workspace's urgent flag clears when it is shown or one of its windows is
  focused.
- Moving a window to another workspace can follow it (`focus_moved_window`, or
  per move).
- Dim unfocused windows with a black overlay (`mu.config.dim.unfocused`), and
  dim a window while one of its modal dialogs is open (`dim.modal`). The
  pointer still reaches a dimmed window.
- Make windows translucent the same way (`mu.config.opacity.focused`,
  `unfocused` and `modal`). A faded window replaces the alpha its client asked
  for.
- Both effects take values from 0 to 1; the defaults leave windows unchanged.

### Protocols
- `keyboard-shortcuts-inhibit-v1`: VMs, remote desktops and games get
  compositor keybindings while focused.
- `xdg-dialog-v1`, so clients can mark dialogs modal.
- `xdg-toplevel-tag-v1`: tags and descriptions, also when set before the
  window maps, exposed to Lua.
- `xdg-toplevel-icon-v1`, `alpha-modifier-v1`, `single-pixel-buffer-v1` and
  `xdg-foreign` (v1 and v2).
- Pointer gestures: touchpad pinch reaches clients.

### Input
- Two-finger scrolling on touchpads that support it.
- Touchpad middle button emulation and disable-while-typing.
- Cursor theme and size options. `XCURSOR_THEME` and `XCURSOR_SIZE` are set
  for clients, and the cursor reloads when they change.

### Packaging and session
- `muwm.desktop` session entry and a portals configuration.
- The session environment (`WAYLAND_DISPLAY`, `XDG_CURRENT_DESKTOP` and others)
  is exported to D-Bus activation and systemd on startup.

### Infrastructure
- Build type and flags, and precompiled headers.
- Less redundant work per frame and per event: cached border and window
  sizes, throttled idle-notifier activity and fewer allocations on focus.

### Fixes
- Turning a laptop screen back on after undocking.
- Natural scrolling applied only to touchpads, not to mice.
- Spawned programs can be terminated: SIGINT, SIGTERM and SIGHUP are no longer
  blocked in the child. SIGHUP also quits the compositor cleanly.

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
