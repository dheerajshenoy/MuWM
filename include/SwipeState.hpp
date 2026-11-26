#pragma once

struct SwipeState
{
    bool consumed = false, fired = false;
    uint32_t fingers = 0;
    double dx = 0, dy = 0;
};
