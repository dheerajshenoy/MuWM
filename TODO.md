# MuWM TODO

Tiling Wayland compositor in C++ on wlroots (CMake).

- [ ] Respect client min / max size
- [x] XWayland (`wlr_xwayland`) for X11 apps
- [x] Workspaces
- [ ] Tags
- [ ] Window rules (match on app_id / title)
- [ ] More layouts (monocle, grid) and a way to switch
- [ ] Option: insert new windows at master vs end of stack
- [ ] Window capture of windows on hidden workspaces (their scene node is disabled, so the frame is blank)
- [ ] xdg-activation, pointer-constraints (games), fractional-scale + viewporter
- [ ] Run from a TTY (DRM/KMS backend, libseat)
- [ ] Output and input hotplug (add and remove)
- [ ] Session entry: `/usr/share/wayland-sessions/muwm.desktop`, `XDG_CURRENT_DESKTOP`, `XDG_SESSION_TYPE`, dbus activation environment
- [ ] Run under ASan / UBSan, check listeners for leaks
- [ ] README and license
- [ ] Check that the name `muwm` is free on GitHub and the AUR
- [ ] AUR PKGBUILD; decide which wlroots version to depend on (the API changes between releases)
- [ ] CI build
