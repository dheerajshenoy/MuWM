# MuWM TODO

Tiling Wayland compositor in C++ on wlroots (CMake).

- [ ] Multi-GPU: document `WLR_DRM_DEVICES`
- [ ] Tags
- [ ] Window rules (match on app_id / title)
- [ ] More layouts (monocle, grid) and a way to switch
- [ ] Window capture of windows on hidden workspaces (their scene node is disabled, so the frame is blank)
- [ ] Clean exit on SIGTERM; reap child processes
- [ ] Run under ASan / UBSan, check listeners for leaks
- [ ] Check that the name `muwm` is free on GitHub and the AUR
- [ ] AUR PKGBUILD; decide which wlroots version to depend on (the API changes between releases)
- [ ] text-input-v3 + input-method-v2: IME support, e.g. fcitx.
- [ ] virtual-keyboard + virtual-pointer: wtype, ydotool-style automation.
- [ ] Clear the urgent signal after focus
- [ ] keyboard-shortcuts-inhibit: let VMs, remote desktop and games receive compositor keybindings
- [ ] pointer-gestures: touchpad pinch and swipe in clients; swipe to switch workspace
- [ ] Touch and tablet input (`wlr_touch`, tablet-v2)
- [ ] IPC / control socket (`muwm-msg`) for scripting and bars that need layout and focus info
- [ ] Scratchpad and sticky windows
- [ ] Small protocols: xdg-toplevel-icon, xdg-system-bell, alpha-modifier, xdg-toplevel-tag
- [ ] Animations (optional)
