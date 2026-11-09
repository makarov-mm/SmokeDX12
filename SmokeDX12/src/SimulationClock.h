#pragma once

#include <algorithm>
#include <cmath>

// Decides how many simulation steps each rendered frame takes.
//
// The solver always advances by the same step, because its tuning assumes it
// and because some of its per step errors, such as the mass the MacCormack
// limiter adds, would otherwise scale with the frame rate. Taking one step per
// frame instead would tie the speed of the smoke to the refresh rate of the
// display. So frames add their duration to an accumulator and take a step
// whenever a whole one has built up, but never more than one: a GPU that
// cannot keep up gets slow motion rather than a spiral of ever longer frames.
class SimulationClock
{
public:
    explicit SimulationClock(float step) noexcept
        : m_step(step)
        , m_averageFrameTime(step)
    {
    }

    // Takes the duration of the last frame and returns whether it takes a step.
    bool Advance(float frameTime) noexcept
    {
        m_averageFrameTime += (frameTime - m_averageFrameTime) * 0.05f;

        // A nominal 60 Hz display really runs at 59.94 or 60.05 Hz and frame
        // times jitter around that, so the measured time alone would drop or
        // repeat a step every few seconds. While the average frame rate is
        // within 3 percent of a whole multiple of the step rate, every frame
        // counts as exactly that fraction of a step.
        const float framesPerStep = m_step / m_averageFrameTime;
        const float wholeFrames = std::round(framesPerStep);
        if (wholeFrames >= 1.0f && std::abs(framesPerStep - wholeFrames) < 0.03f * wholeFrames)
        {
            frameTime = m_step / wholeFrames;
        }

        // The tolerance absorbs the rounding of m_step / wholeFrames.
        m_accumulator += frameTime;
        if (m_accumulator < m_step * 0.999f)
        {
            return false;
        }

        m_accumulator = std::clamp(m_accumulator - m_step, 0.0f, m_step);
        m_time += m_step;
        return true;
    }

    float Step() const noexcept { return m_step; }

    // The end of the last step taken, which is what the solver runs on.
    float Time() const noexcept { return static_cast<float>(m_time); }

    // Simulation time plus the part of a step that has built up since, which
    // moves smoothly even on frames that take no step.
    float RenderTime() const noexcept { return static_cast<float>(m_time + m_accumulator); }

private:
    float m_step;
    float m_averageFrameTime;
    float m_accumulator = 0.0f;
    double m_time = 0.0;
};
