# MuWM TODO

Tiling Wayland compositor in C++ on wlroots (CMake).

- [ ] Tags
- [ ] Window rules (match on app_id / title)
- [ ] More layouts (monocle, grid) and a way to switch
- [ ] Window capture of windows on hidden workspaces (their scene node is disabled, so the frame is blank)
- [ ] Output and input hotplug (add and remove)
- [ ] Session entry: `/usr/share/wayland-sessions/muwm.desktop`, `XDG_CURRENT_DESKTOP`, `XDG_SESSION_TYPE`, dbus activation environment
- [ ] Portal config (`muwm-portals.conf` using `wlr;gtk`) so screen sharing and file pickers work; log to a file when run from a TTY
- [ ] Laptop input: libinput settings (tap-to-click, natural scroll, disable-while-typing), lid switch turns the internal panel off
- [ ] Test output hotplug: unplug a monitor with windows on it, unplug the last output
- [ ] Cursor theme: pass the theme to the xcursor manager and set `XCURSOR_THEME` / `XCURSOR_SIZE` for clients
- [ ] Multi-GPU: document `WLR_DRM_DEVICES`
- [ ] Small protocols: single-pixel-buffer, xdg-foreign
- [ ] Clean exit on SIGTERM; reap child processes
- [ ] Run under ASan / UBSan, check listeners for leaks
- [ ] Check that the name `muwm` is free on GitHub and the AUR
- [ ] AUR PKGBUILD; decide which wlroots version to depend on (the API changes between releases)
- [ ] text-input-v3 + input-method-v2: IME support, e.g. fcitx.
- [ ] virtual-keyboard + virtual-pointer: wtype, ydotool-style automation.
- [ ] Clear the urgent signal after focus
