#include "IdleInhibitor.hpp"

#include "MuWM.hpp"

void
IdleInhibitor::onDestroy(void *)
{
    wm->updateIdleInhibit(
        inhibitor); // it is still in the list during this signal
    destroy.unbind();
    delete this;
}
