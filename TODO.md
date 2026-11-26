# MuWM TODO

Tiling Wayland compositor in C++ on wlroots (CMake).

- [ ] Window capture of windows on hidden workspaces (their scene node is disabled, so the frame is blank)
- [ ] pointer-gestures: touchpad pinch and swipe in clients; swipe to switch workspace
- [ ] IPC / control socket (`muwm-msg`) for scripting and bars that need layout and focus info
- [ ] Animations (optional)

## Protocols

- [ ] ext-transient-seat-v1
- [ ] drm-lease-v1
- [ ] tablet-unstable-v2
- [ ] xdg-system-bell
- [ ] wlr-virtual-pointer-unstable-v1
- [ ] virtual-keyboard-unstable-v1
- [ ] color-management-v1: HDR / wide-gamut; needs per-surface color conversion in the render path

# LOW PRIORITY

- [ ] Per input device config
- [ ] Touch and tablet input (`wlr_touch`, tablet-v2)
- [ ] text-input-v3 + input-method-v2: IME support, e.g. fcitx.
- [ ] Multi-GPU: document `WLR_DRM_DEVICES`
- [ ] virtual-keyboard + virtual-pointer: wtype, ydotool-style automation.
