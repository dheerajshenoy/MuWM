#include "Animation.hpp"

void
Animation::start(double from, double to, int64_t duration_ms, int64_t now,
                 muwm::EasingFunction ease)
{
    m_from     = from;
    m_to       = to;
    m_start    = now;
    m_duration = duration_ms > 0 ? duration_ms : 1;
    m_ease     = ease;
    m_running  = true;
}

// Change the target mid-flight, starting from wherever we are now,
// so the window doesn't jump back to the old start point.
void
Animation::retarget(double to, int64_t duration_ms, int64_t now)
{
    start(value(now), to, duration_ms, now, m_ease);
}

// Value at time `now`. Once time runs out, it snaps to m_to and stops.
double
Animation::value(int64_t now)
{
    if (!m_running)
        return m_to;
    double t = double(now - m_start) / double(m_duration);
    if (t >= 1.0)
    {
        m_running = false;
        return m_to;
    }
    if (t < 0.0)
        t = 0.0;
    return m_from + (m_to - m_from) * m_ease(t);
}
