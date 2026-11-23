#include "Output.hpp"

#include "MuWM.hpp"

void
Output::onFrame(void *)
{
    wlr_scene_output_commit(sceneOutput, nullptr);
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(sceneOutput, &now);
}

void
Output::onDestroy(void *)
{
    wm->closeLayerSurfacesOn(output);
    wm->removeOutput(this);
    wm->publishOutputConfig(); // this output is no longer in m_outputs
    for (Workspace *ws : workspaces)
    {
        if (ws->sceneTree)
            wlr_scene_node_destroy(&ws->sceneTree->node);
        delete ws;
    }
    workspaces.clear();
    workspace = nullptr;
    output->data = nullptr;
    frame.unbind();
    requestState.unbind();
    destroy.unbind();
    delete this;
}

void
Output::onRequestState(void *data)
{
    auto *ev = static_cast<wlr_output_event_request_state *>(data);
    wlr_output_commit_state(output, ev->state);
}
