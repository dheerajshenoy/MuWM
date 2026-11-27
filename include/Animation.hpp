#pragma once

#include <ctime>

namespace muwm
{

inline int64_t
now_ms()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
}

using EasingFunction = double (*)(double);

namespace easing
{

inline double
linear(double t)
{
    return t;
}

inline double
out_cubic(double t)
{
    double u = 1.0 - t;
    return 1.0 - u * u * u;
}

inline double
in_out_cubic(double t)
{
    return t < 0.5 ? 4.0 * t * t * t
                   : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

} // namespace easing

} // namespace muwm

class Animation
{
public:
    void start(double from, double to, int64_t duration_ms, int64_t now,
               muwm::EasingFunction ease = muwm::easing::out_cubic);

    double value(int64_t now);
    void retarget(double to, int64_t duration_ms, int64_t now);

    bool running() const
    {
        return m_running;
    }

    double target() const
    {
        return m_to;
    }

    // Jump straight to a value with no animation (e.g. animations disabled).
    void set(double v)
    {
        m_from = m_to = v;
        m_running     = false;
    }

private:
    double m_from               = 0.0f;
    double m_to                 = 0.0f;
    int64_t m_start             = 0;
    int64_t m_duration          = 1;
    muwm::EasingFunction m_ease = muwm::easing::out_cubic;
    bool m_running              = false;
};
