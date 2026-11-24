#include "Output.hpp"

#include "MuWM.hpp"

void
Output::onFrame(void *)
{
    if (wlr_scene_output_needs_frame(sceneOutput))
    {
        wlr_output_state state;
        wlr_output_state_init(&state);
        if (wlr_scene_output_build_state(sceneOutput, &state, nullptr))
        {
            // A fullscreen game that asked for it is shown without waiting
            // for vblank. Not every driver can do that, so test first and
            // fall back to a normal page flip.
            if (wm->wantsTearing(this))
            {
                state.tearing_page_flip = true;
                if (!wlr_output_test_state(output, &state))
                    state.tearing_page_flip = false;
            }
            wlr_output_commit_state(output, &state);
        }
        wlr_output_state_finish(&state);
    }

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(sceneOutput, &now);
}

void
Output::onDestroy(void *)
{
    wm->closeLayerSurfacesOn(output);
    if (lockCover) // its parent is the lock tree, not this output
    {
        wlr_scene_node_destroy(&lockCover->node);
        lockCover = nullptr;
    }
    wm->removeOutput(this);
    wm->publishOutputConfig(); // this output is no longer in m_outputs
    for (Workspace *ws : workspaces)
    {
        if (ws->sceneTree)
        {
            wlr_scene_node_destroy(&ws->sceneTree->node);
        }
        if (ws->handle)
        {
            wlr_ext_workspace_handle_v1_destroy(ws->handle);
            ws->handle = nullptr;
        }
        delete ws;
    }

    if (group)
    {
        wlr_ext_workspace_group_handle_v1_destroy(group);
        group = nullptr;
    }

    workspaces.clear();
    workspace    = nullptr;
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
