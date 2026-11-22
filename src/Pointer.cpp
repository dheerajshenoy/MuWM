#include "Pointer.hpp"

#include "MuWM.hpp"

void
Pointer::onDestroy(void *)
{
    destroy.unbind();
    wm->removePointer(this);
    delete this;
}
