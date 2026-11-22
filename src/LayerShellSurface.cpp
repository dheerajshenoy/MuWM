#include "LayerShellSurface.hpp"

#include "MuWM.hpp"

bool
LayerShellSurface::wantsKeyboard() const
{
    const auto &state = layerSurface->current;
    return state.keyboard_interactive
               != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE
           && (state.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP
               || state.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
}

void
LayerShellSurface::onMap(void *)
{
    wlr_log(WLR_INFO, "Layer surface mapped: %s (layer %d, keyboard %d)",
            layerSurface->namespace_ ? layerSurface->namespace_ : "(unnamed)",
            static_cast<int>(layerSurface->current.layer),
            static_cast<int>(layerSurface->current.keyboard_interactive));
    wm->arrangeLayers(layerSurface->output);
    if (wantsKeyboard())
        wm->focusLayerSurface(this);
}

void
LayerShellSurface::onUnmap(void *)
{
    wm->releaseLayerFocus(this);
    wm->arrangeLayers(layerSurface->output);
}

void
LayerShellSurface::onCommit(void *)
{
    if (!layerSurface->output)
        return;

    // A surface can't map until it has been configured, and the configure
    // is computed from its *current* state. On the initial commit the
    // client's requested state is still in `pending`, so apply it temporarily.
    if (layerSurface->initial_commit)
    {
        const auto current   = layerSurface->current;
        layerSurface->current = layerSurface->pending;
        wm->arrangeLayers(layerSurface->output);
        layerSurface->current = current;
        return;
    }

    if (layerSurface->current.committed == 0)
        return;

    if (layerSurface->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER)
        wm->moveLayerSurface(this);

    wm->arrangeLayers(layerSurface->output);

    // The client may change what it wants after mapping.
    if (layerSurface->surface->mapped)
    {
        if (wantsKeyboard())
            wm->focusLayerSurface(this);
        else
            wm->releaseLayerFocus(this);
    }
}

void
LayerShellSurface::onDestroy(void *)
{
    wm->releaseLayerFocus(this); // normally unmap came first; be safe
    map.unbind();
    unmap.unbind();
    commit.unbind();
    destroy.unbind();
    wm->removeLayerShellSurface(this);
    delete this;
}
