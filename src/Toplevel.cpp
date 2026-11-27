#include "Toplevel.hpp"

#include "MuWM.hpp"

namespace
{
static void
applyOpacity(wlr_scene_buffer *buffer, int, int, void *data)
{
    wlr_scene_buffer_set_opacity(buffer, *static_cast<float *>(data));
}

// Config colors are straight alpha; wlroots wants premultiplied.
static void
setRectColor(wlr_scene_rect *r, const float c[4])
{
    const float pm[4] = {c[0] * c[3], c[1] * c[3], c[2] * c[3], c[3]};
    wlr_scene_rect_set_color(r, pm);
}
} // namespace

// A faded window shows through to whatever is behind it. This replaces the
// alpha a client asked for while it is faded.
void
Toplevel::updateOpacity()
{
    if (!surfaceTree)
        return;

    const auto &cfg = wm->config().opacity;
    float value     = focused ? cfg.focused : cfg.unfocused;
    if (cfg.modal < value && hasModalChild())
        value = cfg.modal;

    const bool fade = value < 1.0f;
    if (!fade && !faded)
        return; // nothing to apply or undo; leave the client's alpha alone

    faded = fade;
    wlr_scene_node_for_each_buffer(&surfaceTree->node, applyOpacity, &value);
}

void
Toplevel::onMap(void *)
{
    wlr_scene_node_set_enabled(&tree->node, true); // undo the hide from unmap
    wlr_log(WLR_INFO, "Window mapped: %s", title()[0] ? title() : "(untitled)");

    wlrHandle
        = wlr_foreign_toplevel_handle_v1_create(wm->foreignToplevelManager());

    requestActivate.bind<&Toplevel::onRequestActivate>(
        &wlrHandle->events.request_activate, this);

    requestClose.bind<&Toplevel::onRequestClose>(
        &wlrHandle->events.request_close, this);

    wlr_foreign_toplevel_handle_v1_set_title(wlrHandle, title());
    wlr_foreign_toplevel_handle_v1_set_app_id(wlrHandle, appId());

    // Dialogs (they have a parent) and fixed-size windows don't belong in the
    // tiled layout.
    if (isDialog() || isFixedSize())
        wm->setFloating(this, true);

    // Advertise the window to ext-foreign-toplevel-list; pickers and the
    // window capture source are keyed on this handle.
    wlr_ext_foreign_toplevel_handle_v1_state st{title(), appId()};
    foreignHandle = wlr_ext_foreign_toplevel_handle_v1_create(
        wm->foreignToplevelList(), &st);
    foreignHandle->data = this;

    wm->addToplevel(this);
}

void
Toplevel::updateForeignHandle()
{
    if (!foreignHandle)
        return;

    if (wlrHandle)
    {
        wlr_foreign_toplevel_handle_v1_set_title(wlrHandle, title());
        wlr_foreign_toplevel_handle_v1_set_app_id(wlrHandle, appId());
    }

    wlr_ext_foreign_toplevel_handle_v1_state st{title(), appId()};
    wlr_ext_foreign_toplevel_handle_v1_update_state(foreignHandle, &st);
}

void
Toplevel::destroyForeignHandle()
{
    if (!foreignHandle)
        return;
    wlr_ext_foreign_toplevel_handle_v1_destroy(foreignHandle);
    foreignHandle = nullptr;
}

void
Toplevel::onSetTitle(void *)
{
    updateForeignHandle();
}

void
Toplevel::onSetAppId(void *)
{
    updateForeignHandle();
}

void
Toplevel::onCommit(void *)
{
    if (xdg->base->initial_commit)
    {
        wlr_xdg_toplevel_set_size(xdg, 0, 0); // 0,0 = "client picks its size"
        sentW = sentH = 0; // a remapped window starts from "client picks"
        // A client can ask for fullscreen before its first commit.
        wm->setFullscreen(this, xdg->requested.fullscreen);

        setTiled(
            !floating); // tell the client its edges are held by the compositor
    }

    // The client's real size is only known once it commits.
    if (workspace && !fullscreen)
        fitBorders();

    // CSD clients (Firefox) draw shadows and edges outside their window
    // geometry; cut them off so they can't cover the borders. The clip is in
    // root-surface coordinates, which is what geometry is.
    wlr_scene_subsurface_tree_set_clip(&surfaceTree->node,
                                       &xdg->base->geometry);

    if (faded)
        updateOpacity();

    // A client may change its min/max size at any time. Re-lay out so the
    // window is clamped to the new limits.
    const auto &c = xdg->current;
    const SizeLimits now{c.min_width, c.min_height, c.max_width, c.max_height};
    if (now == limits)
        return;
    limits = now;
    if (workspace) // set once the window is mapped
        wm->arrange(workspace);
}

void
Toplevel::onRequestFullscreen(void *)
{
    wm->setFullscreen(this, xsurface ? xsurface->fullscreen
                                     : xdg->requested.fullscreen);
    // The client waits for a configure answering its request, even if we
    // changed nothing.
    scheduleConfigure();
}

void
Toplevel::onUnmap(void *)
{
    wm->animateClose(this); // snapshot while the last buffer is still attached
    wlr_scene_node_set_enabled(&tree->node, false); // hides the borders too
    placed = false;
    destroyForeignHandle();
    destroyWlrHandle();
    wm->removeToplevel(this);
}

void
Toplevel::onDestroy(void *)
{
    if (icon)
    {
        wlr_xdg_toplevel_icon_v1_unref(icon);
        icon = nullptr;
    }

#ifdef MUWM_HAS_SCENEFX
    if (shadow)
    {
        wlr_scene_node_destroy(&shadow->node);
        shadow = nullptr;
    }
#endif

    destroyForeignHandle();
    destroyWlrHandle();
    wm->removeToplevel(this);
    wlr_scene_node_destroy(&tree->node); // container, surface tree and borders
    map.unbind();
    unmap.unbind();
    commit.unbind();
    destroy.unbind();
    requestFullscreen.unbind();
    setTitle.unbind();
    setAppId.unbind();
    associate.unbind();
    dissociate.unbind();
    requestConfigure.unbind();
    requestActivate.unbind();
    requestClose.unbind();
    delete this;
}

void
Toplevel::resizeBorders(int w, int h)
{
    const int b = wm->config().border.width;
    if (w == borderW && h == borderH && b == borderB)
        return;

    borderW = w;
    borderH = h;
    borderB = b;

    wlr_scene_rect_set_size(border[0], w + 2 * b, b);
    wlr_scene_node_set_position(&border[0]->node, -b, -b);
    wlr_scene_rect_set_size(border[1], w + 2 * b, b);
    wlr_scene_node_set_position(&border[1]->node, -b, h);
    wlr_scene_rect_set_size(border[2], b, h);
    wlr_scene_node_set_position(&border[2]->node, -b, 0);
    wlr_scene_rect_set_size(border[3], b, h);
    wlr_scene_node_set_position(&border[3]->node, w, 0);

    if (dim)
        wlr_scene_rect_set_size(dim, w, h);

    updateShadow();
    updateBlur();
}

void
Toplevel::setBordersVisible(bool visible)
{
    for (auto *r : border)
        wlr_scene_node_set_enabled(&r->node, visible);

    updateShadow();
    updateBlur();
}

void
Toplevel::setFocused(bool focused)
{
    this->focused = focused;
    updateBorder();
    updateDim();
    updateOpacity();
    updateShadow(); // for shadow.focused_only
    updateBlur();   // for blur.focused_only
}

// The top and left edges show whether the window has focus. A sticky window
// shows that on its bottom and right edges, so it can be told apart without
// hiding the focus.
void
Toplevel::updateBorder()
{
    const auto &bd     = wm->config().border;
    const float *state = focused ? bd.focused_color : bd.unfocused_color;
    const float *other = sticky ? bd.sticky_color : state;

    setRectColor(border[0], state); // top
    setRectColor(border[1], other); // bottom
    setRectColor(border[2], state); // left
    setRectColor(border[3], other); // right
}

wlr_surface *
Toplevel::surface() const
{
    // An X11 window has no surface until it is associated with one.
    return xsurface ? xsurface->surface : xdg->base->surface;
}

wlr_box
Toplevel::geometry() const
{
    if (xsurface)
        return wlr_box{0, 0, xsurface->width, xsurface->height};
    return xdg->base->geometry;
}

void
Toplevel::setSize(int width, int height)
{
    if (!xsurface)
    {
        if (width == sentW && height == sentH)
            return;
        sentW = width;
        sentH = height;
        wlr_xdg_toplevel_set_size(xdg, width, height);
        return;
    }

    // X11 clients also need to be told where they are. Workspace trees sit at
    // the layout origin, so the scene coordinates are layout coordinates.
    int x = 0, y = 0;
    wlr_scene_node_coords(&tree->node, &x, &y);
    wlr_xwayland_surface_configure(xsurface, x, y, width, height);
}

void
Toplevel::setActivated(bool activated)
{
    if (xsurface)
        wlr_xwayland_surface_activate(xsurface, activated);
    else
        wlr_xdg_toplevel_set_activated(xdg, activated);

    if (wlrHandle)
        wlr_foreign_toplevel_handle_v1_set_activated(wlrHandle, activated);
}

void
Toplevel::setFullscreen(bool fullscreen)
{
    if (wlrHandle)
        wlr_foreign_toplevel_handle_v1_set_fullscreen(wlrHandle, fullscreen);

    if (xsurface)
    {
        wlr_xwayland_surface_set_fullscreen(xsurface, fullscreen);
        return;
    }
    // Before the first commit the state is sent with the initial configure.
    if (xdg->base->initialized)
        wlr_xdg_toplevel_set_fullscreen(xdg, fullscreen);

    setTiled(
        !fullscreen); // tell the client its edges are held by the compositor
}

void
Toplevel::scheduleConfigure()
{
    if (xsurface)
        return; // X11 has no configure handshake
    // Scheduling before the surface is initialized asserts in wlroots.
    if (xdg->base->initialized)
        wlr_xdg_surface_schedule_configure(xdg->base);
}

void
Toplevel::close()
{
    if (xsurface)
        wlr_xwayland_surface_close(xsurface);
    else
        wlr_xdg_toplevel_send_close(xdg);
}

const char *
Toplevel::title() const
{
    if (xsurface)
        return xsurface->title ? xsurface->title : "";
    return xdg->title ? xdg->title : "";
}

const char *
Toplevel::appId() const
{
    if (xsurface)
        return xsurface->class_ ? xsurface->class_ : "";
    return xdg->app_id ? xdg->app_id : "";
}

bool
Toplevel::isDialog() const
{
    if (xsurface)
        return xsurface->parent != nullptr
               || wlr_xwayland_surface_has_window_type(
                   xsurface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG);
    return xdg->parent != nullptr;
}

bool
Toplevel::isFixedSize() const
{
    if (xsurface)
    {
        const xcb_size_hints_t *h = xsurface->size_hints;
        constexpr uint32_t both
            = XCB_ICCCM_SIZE_HINT_P_MIN_SIZE | XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
        return h && (h->flags & both) == both && h->min_width > 0
               && h->min_width == h->max_width && h->min_height > 0
               && h->min_height == h->max_height;
    }
    const auto &c = xdg->current;
    return c.min_width > 0 && c.min_width == c.max_width && c.min_height > 0
           && c.min_height == c.max_height;
}

// --- X11 window lifecycle ----------------------------------------------------

// The window now has a wl_surface: show it and watch it map and unmap.
void
Toplevel::onAssociate(void *)
{
    surfaceTree = wlr_scene_subsurface_tree_create(tree, xsurface->surface);
    map.bind<&Toplevel::onMap>(&xsurface->surface->events.map, this);
    unmap.bind<&Toplevel::onUnmap>(&xsurface->surface->events.unmap, this);
    if (xsurface->surface->mapped)
        onMap(nullptr); // it mapped before we were listening
}

void
Toplevel::onDissociate(void *)
{
    map.unbind();
    unmap.unbind();
    surfaceTree = nullptr; // the scene tree goes away with the surface
}

void
Toplevel::onRequestConfigure(void *data)
{
    auto *ev = static_cast<wlr_xwayland_surface_configure_event *>(data);

    // Not on a workspace yet, or floating: the window may have what it asks
    // for. A tile keeps its size, so just repeat it.
    if (!workspace)
        wlr_xwayland_surface_configure(xsurface, ev->x, ev->y, ev->width,
                                       ev->height);
    else if (floating)
    {
        floatBox = {ev->x, ev->y, ev->width, ev->height};
        wm->arrange(workspace);
    }
    else
    {
        int x = 0, y = 0;
        wlr_scene_node_coords(&tree->node, &x, &y);
        wlr_xwayland_surface_configure(xsurface, x, y, xsurface->width,
                                       xsurface->height);
    }
}

void
Toplevel::clampSize(int &w, int &h) const
{
    int minW = 0, minH = 0, maxW = 0, maxH = 0;

    if (xsurface)
    {
        const xcb_size_hints_t *hints = xsurface->size_hints;
        if (hints)
        {
            if (hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)
            {
                minW = hints->min_width;
                minH = hints->min_height;
            }

            if (hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE)
            {
                maxW = hints->max_width;
                maxH = hints->max_height;
            }
        }
    }
    else
    {
        const auto &c = xdg->current;
        minW          = c.min_width;
        minH          = c.min_height;
        maxW          = c.max_width;
        maxH          = c.max_height;
    }

    // Apply max first, then min, so min wins if a client reports min > max.
    if (maxW > 0)
        w = std::min(w, maxW);
    if (maxH > 0)
        h = std::min(h, maxH);
    if (minW > 0)
        w = std::max(w, minW);
    if (minH > 0)
        h = std::max(h, minH);
}

// A client may ignore the size it was given. Draw the borders around what it
// actually has, never larger than what the layout asked for.
void
Toplevel::fitBorders()
{
    const wlr_box geo = geometry();
    // Until the client commits a buffer its size is unknown; assume it obeyed.
    const int w
        = (geo.width > 0 && geo.width < slot.width) ? geo.width : slot.width;
    const int h = (geo.height > 0 && geo.height < slot.height) ? geo.height
                                                               : slot.height;
    resizeBorders(w, h);
}

void
Toplevel::setTiled(bool tiled)
{
    if (xsurface)
        return; // X11 has no tiled state

    if (!xdg->base->initialized)
        return; // before the first commit the state is sent with the initial
                // configure

    // Tell the client its four edges are held by the compositor.
    const uint32_t edges = tiled ? (WLR_EDGE_TOP | WLR_EDGE_BOTTOM
                                    | WLR_EDGE_LEFT | WLR_EDGE_RIGHT)
                                 : WLR_EDGE_NONE;
    wlr_xdg_toplevel_set_tiled(xdg, edges);
}

void
Toplevel::onRequestActivate(void *)
{
    wm->activateToplevel(this);
}

void
Toplevel::onRequestClose(void *)
{
    close();
}

// Tell taskbars which output the window is on; they list per output.
void
Toplevel::syncForeignOutput(wlr_output *previous)
{
    if (!wlrHandle)
        return;
    wlr_output *now
        = workspace && workspace->output ? workspace->output->output : nullptr;
    if (now == previous)
        return;
    if (previous)
        wlr_foreign_toplevel_handle_v1_output_leave(wlrHandle, previous);
    if (now)
        wlr_foreign_toplevel_handle_v1_output_enter(wlrHandle, now);
}

void
Toplevel::destroyWlrHandle()
{
    if (!wlrHandle)
        return;
    requestActivate.unbind();
    requestClose.unbind();
    wlr_foreign_toplevel_handle_v1_destroy(wlrHandle);
    wlrHandle = nullptr;
}

// The overlay is created on first use, so both window kinds (xdg and X11)
// get it without touching their creation code.
void
Toplevel::updateDim()
{
    if (!dim)
    {
        const float none[4] = {0, 0, 0, 0};
        dim                 = wlr_scene_rect_create(tree, 1, 1, none);
        dim->node.data      = this;
    }

    const auto &cfg = wm->config().dim;
    float amount    = focused ? 0.0f : cfg.unfocused;
    if (cfg.modal > amount && hasModalChild())
        amount = cfg.modal;

    const float color[4] = {0, 0, 0, amount};
    wlr_scene_rect_set_color(dim, color);
    wlr_scene_rect_set_size(dim, borderW > 0 ? borderW : 1,
                            borderH > 0 ? borderH : 1);
    wlr_scene_node_set_enabled(&dim->node, amount > 0.0f);
}

bool
Toplevel::isModal() const
{
    if (xsurface)
        return xsurface->modal;
    const wlr_xdg_dialog_v1 *dialog
        = wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel(xdg);
    return dialog && dialog->modal;
}

bool
Toplevel::hasModalChild() const
{
    if (!workspace)
        return false;
    for (const Toplevel *other : workspace->toplevels)
    {
        if (other == this || !other->isModal())
            continue;
        const bool child
            = xsurface ? other->xsurface && other->xsurface->parent == xsurface
                       : other->xdg && other->xdg->parent == xdg;
        if (child)
            return true;
    }
    return false;
}

void
Toplevel::moveTo(int x, int y, bool animate)
{
    if (!animate)
    {
        placed = true;
        animX.set(x);
        animY.set(y);
        wlr_scene_node_set_position(&tree->node, x, y);
        return;
    }

    const int64_t now = muwm::now_ms();
    const int dur     = wm->config().animation.duration_ms;

    // First placement after map: slide in from a little below the slot
    // instead of snapping, so opening a window is visible.
    if (!placed)
    {
        placed                    = true;
        constexpr int kOpenOffset = 40;
        animX.set(x);
        animY.start(y + kOpenOffset, y, dur, now);
        wlr_scene_node_set_position(&tree->node, x, y + kOpenOffset);
        wm->scheduleFrames();
        return;
    }

    // already moving to the same place
    if (animX.target() == x && animY.target() == y)
        return;

    animX.retarget(x, dur, now);
    animY.retarget(y, dur, now);

    wm->scheduleFrames();
}

bool
Toplevel::tickMove(int64_t now)
{
    double x = animX.value(now);
    double y = animY.value(now);
    wlr_scene_node_set_position(&tree->node, std::lround(x), std::lround(y));
    return animX.running() || animY.running();
}

void
Toplevel::createShadow()
{
#ifdef MUWM_HAS_SCENEFX
    if (shadow)
        return;
    const float none[4] = {0, 0, 0, 0};
    shadow              = wlr_scene_shadow_create(tree, 0, 0, 0, 0.0f, none);
    wlr_scene_node_lower_to_bottom(&shadow->node); // below borders + surface
    updateShadow();
#endif
}

void
Toplevel::updateShadow()
{
#ifdef MUWM_HAS_SCENEFX
    if (!shadow)
        return;

    const Config::Shadow &cfg = wm->config().shadow;

    // Shown only with the borders (not fullscreen, not smart-hidden).
    const bool visible = cfg.enabled && border[0]->node.enabled
                         && (!cfg.focused_only || focused);
    wlr_scene_node_set_enabled(&shadow->node, visible);
    if (!visible)
        return;

    // The config holds straight alpha; scenefx wants premultiplied.
    const float a = cfg.color[3] * cfg.opacity;
    const float color[4]
        = {cfg.color[0] * a, cfg.color[1] * a, cfg.color[2] * a, a};
    wlr_scene_shadow_set_color(shadow, color);
    wlr_scene_shadow_set_blur_sigma(shadow, float(cfg.sigma));

    if (borderW < 0)
        return; // not sized yet; resizeBorders calls us again

    const int offX = cfg.offset_x, offY = cfg.offset_y;
    const int b   = borderB;
    const int pad = cfg.sigma;       // room for the blur
    const int ow  = borderW + 2 * b; // window plus borders
    const int oh  = borderH + 2 * b;

    wlr_scene_shadow_set_size(shadow, ow + 2 * pad, oh + 2 * pad);
    wlr_scene_node_set_position(&shadow->node, -b - pad + offX,
                                -b - pad + offY);

    // Don't draw under the window itself (visible through translucent ones).
    // The area is relative to the shadow node.
    wlr_scene_shadow_set_clipped_region(
        shadow, {.area    = {pad - offX, pad - offY, ow, oh},
                 .corners = corner_radii_none()});
#endif
}

void
Toplevel::destroyShadow()
{
#ifdef MUWM_HAS_SCENEFX
    if (!shadow)
        return;
    wlr_scene_node_destroy(&shadow->node);
    shadow = nullptr;
#endif
}

void
Toplevel::createBlur()
{
#ifdef MUWM_HAS_SCENEFX
    if (blur)
        return;
    blur = wlr_scene_blur_create(tree, 0, 0);
    wlr_scene_node_lower_to_bottom(&blur->node);
    updateBlur();
#endif
}

void
Toplevel::updateBlur()
{
#ifdef MUWM_HAS_SCENEFX
    if (!blur)
        return;

    const Config::Blur &cfg = wm->config().blur;

    // Shown only with the borders (not fullscreen, not smart-hidden).
    const bool visible = cfg.enabled && border[0]->node.enabled
                         && (!cfg.focused_only || focused);
    wlr_scene_node_set_enabled(&blur->node, visible);
    if (!visible)
        return;

    if (borderW < 0)
        return; // not sized yet; resizeBorders calls us again

    // Behind the surface only, like dim: it shows through wherever the
    // client is translucent. A blur doesn't spill past its node, so no pad.
    wlr_scene_node_set_position(&blur->node, 0, 0);
    wlr_scene_blur_set_size(blur, borderW, borderH);

    wlr_scene_blur_set_alpha(blur, cfg.alpha);
    wlr_scene_blur_set_should_only_blur_bottom_layer(blur, cfg.bottom_only);
    // Radius is scene-wide (MuWM::applyPending); strength stays at 1.
#endif
}

void
Toplevel::destroyBlur()
{
#ifdef MUWM_HAS_SCENEFX
    if (!blur)
        return;
    wlr_scene_node_destroy(&blur->node);
    blur = nullptr;
#endif
}
