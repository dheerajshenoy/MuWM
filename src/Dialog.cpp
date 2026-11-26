#include "Dialog.hpp"

#include "MuWM.hpp"

void
Dialog::onSetModal(void *)
{
    wm->scheduleApply(MuWM::ChangeRecolor); // re-evaluates every overlay
}

void
Dialog::onDestroy(void *)
{
    setModal.unbind();
    destroy.unbind();
    wm->scheduleApply(MuWM::ChangeRecolor); // after the dialog is gone
    delete this;
}
