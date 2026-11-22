#include "Output.hpp"

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
    wl_list_remove(&frame.listener.link);
    wl_list_remove(&destroy.listener.link);
    delete this;
}
