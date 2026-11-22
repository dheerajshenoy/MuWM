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

- [ ] Focus next / previous window (Alt+J / Alt+K)
- [ ] Close the focused window (Alt+Shift+Q, `wlr_xdg_toplevel_send_close`)
- [ ] Swap / promote a window to master
- [ ] Verify output resize re-tiles (`request_state` + layout `change`)
- [ ] xdg popups (menus, tooltips). Without them, right-click menus won't show
- [ ] Floating windows for dialogs (toplevels with a parent) and fullscreen requests
- [ ] Client cursor shape (`request_set_cursor`, cursor-shape-v1)
- [ ] Clean shutdown on SIGINT / SIGTERM (destroy listeners, `wl_display_destroy_clients`)

## Configuration (Lua)

- [ ] Wire `Config` into `init()` / `run()`: load before `wlr_backend_start`, use `m_config` for gap and border
- [ ] Proxy-table options (`muwm.opts`) with validation
- [ ] Config path: `$XDG_CONFIG_HOME/muwm/config.lua`, fallback `~/.config/muwm/config.lua`
- [ ] `muwm.bind(key, fn)`: callbacks stored in the Lua registry, called with `lua_pcall`
- [ ] Replace the hardcoded Alt+Q / Alt+Return with config bindings
- [ ] Reload keybinding (reset `m_config`, rerun config, `arrange()`)
- [ ] Keyboard layout, xkb options and repeat rate as config options
- [ ] LuaLS annotation stub for editor completion

## Layout and windows

- [ ] Per-output layout instead of the bounding box of all outputs (multi-monitor)
- [ ] Workspaces / tags
- [ ] Window rules (match on app_id / title)
- [ ] More layouts (monocle, grid) and a way to switch
- [ ] Respect client min / max size
- [ ] Option: insert new windows at master vs end of stack

## Desktop protocols

- [ ] wlr-layer-shell (bars, launchers such as waybar, fuzzel)
- [ ] xdg-output and output management (wlr-randr, kanshi)
- [ ] Screen capture (screencopy / ext-image-copy-capture) and xdg-desktop-portal-wlr
- [ ] Session lock (swaylock), idle-notify, idle-inhibit
- [ ] primary-selection and data-control (clipboard managers)
- [ ] xdg-activation, pointer-constraints (games), fractional-scale + viewporter
- [ ] XWayland (`wlr_xwayland`) for X11 apps

## Real hardware

- [ ] Run from a TTY (DRM/KMS backend, libseat)
- [ ] Output and input hotplug (add and remove)
- [ ] Session entry: `/usr/share/wayland-sessions/muwm.desktop`, `XDG_CURRENT_DESKTOP`, `XDG_SESSION_TYPE`, dbus activation environment
- [ ] Touchpad settings via libinput (tap-to-click, natural scroll)

## Quality and packaging

- [ ] Run under ASan / UBSan, check listeners for leaks
- [ ] README and license
- [ ] Check that the name `muwm` is free on GitHub and the AUR
- [ ] AUR PKGBUILD; decide which wlroots version to depend on (the API changes between releases)
- [ ] CI build
