# MuWM TODO

Tiling Wayland compositor in C++ on wlroots (CMake).

## Done

- [x] CMake build against wlroots 0.20, generated protocol headers
- [x] Display, backend (nested), renderer, allocator
- [x] Outputs, frame loop, scene graph
- [x] xdg-shell toplevels and the configure handshake
- [x] Keyboard input, keybinding hook, focus
- [x] Pointer: cursor, hit-testing, click-to-focus
- [x] Startup command (`-s cmd`)
- [x] Master-stack tiling with gaps
- [x] Focus borders
- [x] Server-side decorations (xdg-decoration)

## Next: core usability

- [x] Focus next / previous window (Alt+J / Alt+K)
- [x] Close the focused window (Alt+Shift+Q, `wlr_xdg_toplevel_send_close`)
- [x] Swap / promote a window to master
- [x] Verify output resize re-tiles (`request_state` + layout `change`)
- [x] Clean shutdown on SIGINT / SIGTERM (destroy listeners, `wl_display_destroy_clients`)
- [x] Fullscreen (client requests and `toggle_fullscreen`)
- [x] Floating windows: `toggle_floating`, auto-float for dialogs and fixed-size windows, Super+drag move / resize
- [ ] xdg popups (menus, tooltips). Without them, right-click menus won't show
- [ ] Floating: honour client `request_move` / `request_resize`, and re-center on output change
- [ ] Client cursor shape (`request_set_cursor`, cursor-shape-v1)

## Configuration (Lua)

- [x] Wire `Config` into `init()` / `run()`: load before `wlr_backend_start`, use `m_config` for gap and border
- [x] Proxy-table options (`muwm.opts`) with validation
- [x] Config path: `$XDG_CONFIG_HOME/muwm/config.lua`, fallback `~/.config/muwm/config.lua`
- [x] `muwm.bind(key, fn)`: callbacks stored in the Lua registry, called with `lua_pcall`
- [x] Replace the hardcoded Alt+Q / Alt+Return with config bindings
- [x] Reload keybinding (reset `m_config`, rerun config, `arrange()`)
- [x] Keyboard layout, xkb options and repeat rate as config options
- [x] LuaLS annotation stub for editor completion

## Layout and windows

- [x] Per-output layout instead of the bounding box of all outputs (multi-monitor)
- [x] Workspaces
- [ ] Tags
- [ ] Window rules (match on app_id / title)
- [ ] More layouts (monocle, grid) and a way to switch
- [ ] Respect client min / max size
- [ ] Option: insert new windows at master vs end of stack

## Desktop protocols

- [x] wlr-layer-shell (bars, launchers such as waybar, fuzzel)
- [x] xdg-output and output management (wlr-randr, kanshi)
- [x] Screen capture (screencopy / ext-image-copy-capture) and xdg-desktop-portal-wlr
- [x] primary-selection and data-control (clipboard managers)
- [ ] Window capture of windows on hidden workspaces (their scene node is disabled, so the frame is blank)
- [ ] Test capture end to end: `grim`, then a browser share through the portal
- [ ] Disabling an output with kanshi should move its windows to another output
- [ ] Session lock (swaylock), idle-notify, idle-inhibit
- [ ] xdg-activation, pointer-constraints (games), fractional-scale + viewporter
- [ ] XWayland (`wlr_xwayland`) for X11 apps

## Real hardware

- [x] Touchpad settings via libinput (tap-to-click, natural scroll)
- [ ] Run from a TTY (DRM/KMS backend, libseat)
- [ ] Output and input hotplug (add and remove)
- [ ] Session entry: `/usr/share/wayland-sessions/muwm.desktop`, `XDG_CURRENT_DESKTOP`, `XDG_SESSION_TYPE`, dbus activation environment

## Quality and packaging

- [ ] Run under ASan / UBSan, check listeners for leaks
- [ ] README and license
- [ ] Check that the name `muwm` is free on GitHub and the AUR
- [ ] AUR PKGBUILD; decide which wlroots version to depend on (the API changes between releases)
- [ ] CI build
