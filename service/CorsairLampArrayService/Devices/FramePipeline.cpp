/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 */

#include "Devices/FramePipeline.h"

namespace cla {

cla_u8 ApplyBrightness(cla_u8 channel, cla_u8 brightness)
{
    if (brightness == 255)
    {
        return channel;   /* exact identity at unity gain */
    }
    const cla_u32 scaled = (static_cast<cla_u32>(channel) * brightness + 127u) / 255u;
    return static_cast<cla_u8>(scaled > 255u ? 255u : scaled);
}

FramePipeline::FramePipeline(std::vector<cla_u32> lampIdToLedId,
                             cla_u64 minIntervalMicroseconds)
    : m_lampIdToLedId(std::move(lampIdToLedId)),
      m_minIntervalMicroseconds(minIntervalMicroseconds)
{
    m_shadow.resize(m_lampIdToLedId.size());
}

void FramePipeline::SetBrightness(cla_u8 brightness)
{
    if (brightness == m_brightness)
    {
        return;
    }
    m_brightness = brightness;
    /* Every lamp's output value just changed, so the shadow no longer
     * describes what the device is showing. */
    InvalidateShadow();
}

void FramePipeline::InvalidateShadow()
{
    m_shadowValid = false;
}

bool FramePipeline::Submit(const ClaLampColor *colors,
                           cla_u32 lampCount,
                           cla_u64 nowMicroseconds,
                           std::vector<BackendLedColor> &out)
{
    out.clear();

    if (colors == nullptr || lampCount != m_lampIdToLedId.size())
    {
        /* A frame that does not match this device's lamp count is not
         * something to guess about. */
        return false;
    }

    m_stats.FramesSubmitted++;

    /*
     * Throttle first, before doing any per-lamp work.
     *
     * Note the ordering choice: a throttled frame is dropped rather than
     * deferred.  The driver holds the newest frame, so whatever we skip here
     * is superseded almost immediately; buffering it would only add latency to
     * output that is about to be replaced.
     */
    if (m_haveForwarded && m_minIntervalMicroseconds > 0)
    {
        /* Unsigned arithmetic: guard against a non-monotonic clock rather than
         * wrapping into an enormous elapsed time. */
        const cla_u64 elapsed = (nowMicroseconds >= m_lastForwardedMicroseconds)
                                    ? (nowMicroseconds - m_lastForwardedMicroseconds)
                                    : 0;
        if (elapsed < m_minIntervalMicroseconds)
        {
            m_stats.FramesSkippedThrottled++;
            return false;
        }
    }

    const bool fullUpdate = !m_shadowValid;

    for (cla_u32 lampId = 0; lampId < lampCount; ++lampId)
    {
        const ClaLampColor &color = colors[lampId];

        if (!fullUpdate &&
            m_shadow[lampId].Red == color.Red &&
            m_shadow[lampId].Green == color.Green &&
            m_shadow[lampId].Blue == color.Blue &&
            m_shadow[lampId].Intensity == color.Intensity)
        {
            continue;   /* unchanged: nothing to send for this lamp */
        }

        BackendLedColor write;
        write.LedId = m_lampIdToLedId[lampId];

        /*
         * The LampArray intensity channel and our brightness setting are two
         * separate gains and both apply.  Intensity is per-lamp and comes from
         * Windows; brightness is a device-wide user preference.
         */
        const cla_u8 gain = ApplyBrightness(m_brightness, color.Intensity);
        write.Red = ApplyBrightness(color.Red, gain);
        write.Green = ApplyBrightness(color.Green, gain);
        write.Blue = ApplyBrightness(color.Blue, gain);
        write.Alpha = 255;

        out.push_back(write);
        m_shadow[lampId] = color;
    }

    if (out.empty())
    {
        /* Nothing changed.  Do NOT update the throttle timestamp: no write
         * happened, so the next genuinely different frame should not be made
         * to wait. */
        m_stats.FramesSkippedUnchanged++;
        return false;
    }

    m_shadowValid = true;
    m_haveForwarded = true;
    m_lastForwardedMicroseconds = nowMicroseconds;
    m_stats.FramesForwarded++;
    m_stats.LampWritesForwarded += out.size();
    return true;
}

} // namespace cla
