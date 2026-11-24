# MuWM TODO

Tiling Wayland compositor in C++ on wlroots (CMake).

- [ ] Tags
- [ ] Window rules (match on app_id / title)
- [ ] More layouts (monocle, grid) and a way to switch
- [ ] Window capture of windows on hidden workspaces (their scene node is disabled, so the frame is blank)
- [x] xdg-activation, pointer-constraints (games)
- [x] Explicit sync (`wlr_linux_drm_syncobj_manager_v1`; needs the DRM fd and a renderer with timeline support)
- [x] Run from a TTY (DRM/KMS backend, libseat)
- [ ] Output and input hotplug (add and remove)
- [ ] Session entry: `/usr/share/wayland-sessions/muwm.desktop`, `XDG_CURRENT_DESKTOP`, `XDG_SESSION_TYPE`, dbus activation environment
- [x] VT switching (Ctrl+Alt+F1..F12 via `XF86Switch_VT_n` and `wlr_session_change_vt`); redraw when the session becomes active again
- [ ] Portal config (`muwm-portals.conf` using `wlr;gtk`) so screen sharing and file pickers work; log to a file when run from a TTY
- [x] Idle and power: `ext-idle-notify-v1` and idle-inhibit (notify activity on input), `wlr_output_power_manager_v1` (DPMS), `wlr_gamma_control_manager_v1`
- [ ] Laptop input: libinput settings (tap-to-click, natural scroll, disable-while-typing), lid switch turns the internal panel off
- [ ] Test output hotplug: unplug a monitor with windows on it, unplug the last output
- [x] Presentation timing (`wlr_presentation_create`, `wp_presentation`) for mpv and games
- [x] dmabuf feedback for direct scanout (create `linux-dmabuf-v1` ourselves and call `wlr_scene_set_linux_dmabuf_v1`); check that `init_wl_shm` didn't drop the syncobj global
- [ ] Cursor theme: pass the theme to the xcursor manager and set `XCURSOR_THEME` / `XCURSOR_SIZE` for clients
- [ ] Multi-GPU: document `WLR_DRM_DEVICES`
- [x] Small protocols: tearing-control, content-type, security-context
- [ ] Small protocols: single-pixel-buffer, xdg-foreign
- [ ] Clean exit on SIGTERM; reap child processes
- [ ] Run under ASan / UBSan, check listeners for leaks
- [ ] README and license
- [ ] Check that the name `muwm` is free on GitHub and the AUR
- [ ] AUR PKGBUILD; decide which wlroots version to depend on (the API changes between releases)
- [ ] CI build
- [ ] text-input-v3 + input-method-v2: IME support, e.g. fcitx.
- [ ] virtual-keyboard + virtual-pointer: wtype, ydotool-style automation.
- [x] security-context-v1: restricts what sandboxed Flatpak apps can access.
