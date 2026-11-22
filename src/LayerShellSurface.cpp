#include "LayerShellSurface.hpp"

#include "MuWM.hpp"

void
LayerShellSurface::onMap(void *)
{
    wlr_log(WLR_INFO, "Layer surface mapped: %s",
            layerSurface->namespace_ ? layerSurface->namespace_ : "(unnamed)");
    wm->arrangeLayers(layerSurface->output);
}

void
LayerShellSurface::onUnmap(void *)
{
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
}

void
LayerShellSurface::onDestroy(void *)
{
    map.unbind();
    unmap.unbind();
    commit.unbind();
    destroy.unbind();
    wm->removeLayerShellSurface(this);
    delete this;
}
