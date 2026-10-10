/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Turning LampArray frames into backend writes (project Phase 13).
 *
 * The driver already coalesces: it keeps only the newest completed frame per
 * device, so the service never sees a backlog.  What is left to this class is
 * everything that decides whether a frame is worth sending to the SDK at all,
 * and what exactly to send:
 *
 *   - dirty-lamp tracking, so an unchanged frame costs nothing;
 *   - frame throttling, so a host that updates faster than the device can
 *     accept does not flood the SDK;
 *   - brightness scaling;
 *   - the LampId -> backend LED id mapping.
 *
 * Time is injected rather than read from a clock, so the throttling logic is
 * tested deterministically instead of with sleeps.
 */
#ifndef CLA_FRAME_PIPELINE_H
#define CLA_FRAME_PIPELINE_H

#include "Corsair/ICorsairBackend.h"
#include "cla/LampArrayDevice.h"

#include <vector>
#include <cstdint>

namespace cla {

struct FramePipelineStats
{
    cla_u64 FramesSubmitted = 0;
    /* Frames dropped because nothing changed. */
    cla_u64 FramesSkippedUnchanged = 0;
    /* Frames dropped because they arrived inside the minimum interval. */
    cla_u64 FramesSkippedThrottled = 0;
    cla_u64 FramesForwarded = 0;
    cla_u64 LampWritesForwarded = 0;
};

class FramePipeline
{
public:
    /*
     * lampIdToLedId maps LampId -> backend LED id and must have one entry per
     * lamp.  minIntervalMicroseconds is the floor between two writes; it
     * should match the MinUpdateIntervalInMicroseconds advertised to Windows.
     */
    FramePipeline(std::vector<cla_u32> lampIdToLedId, cla_u64 minIntervalMicroseconds);

    /*
     * Offer a frame.
     *
     * Returns true and fills `out` when the frame should be written to the
     * backend; returns false when it was skipped (unchanged, throttled, or
     * malformed).  `nowMicroseconds` must be monotonic.
     *
     * When `out` is produced it contains only the lamps whose colour actually
     * changed since the last forwarded frame -- typically a handful even
     * during a full-keyboard animation, since most effects move a wavefront
     * rather than repainting everything.
     */
    bool Submit(const ClaLampColor *colors,
                cla_u32 lampCount,
                cla_u64 nowMicroseconds,
                std::vector<BackendLedColor> &out);

    /* 0..255; 255 is unity gain. */
    void SetBrightness(cla_u8 brightness);
    cla_u8 Brightness() const { return m_brightness; }

    /*
     * Forget what the device is currently showing.
     *
     * Must be called whenever the backend may have changed the lighting behind
     * our back -- iCUE restarting, the device reconnecting, control being
     * released.  Otherwise dirty-lamp tracking would suppress the very writes
     * needed to restore the correct state, and the device would sit on
     * whatever iCUE last drew until a lamp happened to change.
     */
    void InvalidateShadow();

    /* Send everything on the next frame regardless of what changed. */
    void ForceFullUpdate() { InvalidateShadow(); }

    const FramePipelineStats &Stats() const { return m_stats; }
    cla_u32 LampCount() const { return static_cast<cla_u32>(m_lampIdToLedId.size()); }

private:
    std::vector<cla_u32> m_lampIdToLedId;
    /* Last colours actually written to the backend. */
    std::vector<ClaLampColor> m_shadow;
    bool m_shadowValid = false;

    cla_u64 m_minIntervalMicroseconds;
    cla_u64 m_lastForwardedMicroseconds = 0;
    bool m_haveForwarded = false;

    cla_u8 m_brightness = 255;
    FramePipelineStats m_stats;
};

/* Scale a channel by brightness with rounding, so that full brightness is
 * exactly the identity and half brightness of 255 is 128, not 127. */
cla_u8 ApplyBrightness(cla_u8 channel, cla_u8 brightness);

} // namespace cla

#endif /* CLA_FRAME_PIPELINE_H */
