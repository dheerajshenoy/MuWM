#include "LockSurface.hpp"

#include "MuWM.hpp"

void
LockSurface::onMap(void *)
{
    wm->lockSurfaceMapped(this);
}

void
LockSurface::onDestroy(void *)
{
    wm->lockSurfaceDestroyed(this);
    map.unbind();
    destroy.unbind();
    delete this;
}
