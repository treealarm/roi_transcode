#pragma once

// The wire format on one side, the library's vocabulary on the other.
//
// It lives in its own file because it is neither service's business and both need it: it
// used to be three hundred lines of anonymous namespace at the top of a file that also held
// two service implementations, and a reader looking for either had to scroll past it.
//
// The direction of the dependency is the point. The library deliberately knows nothing of
// FFmpeg's numbering or of ta_vms' proto; this is the edge where that stops mattering, and
// keeping it in one file is what stops the two vocabularies leaking into each other.

#include "job/roi_job_manager.h"
#include "roi_transcode.grpc.pb.h"
#include "sve/session.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace roisvc {

sve::PixelFormat to_library_format(AVPixelFormat f);
sve::Preset to_library_preset(roi::transcode::Preset p);
sve::BackendChoice from_proto(roi::transcode::Encoder e);
roi::transcode::Encoder to_proto(sve::Backend k);
roi::transcode::JobState to_proto(RoiJobState s);
sve::ObjectPriority priority_from_proto(roi::transcode::ObjectPriority p);
sve::InputCodec codec_from_proto(roi::transcode::InputCodec c);

int64_t to_unix_ms(std::chrono::system_clock::time_point tp);

void fill_lag(bool measured, int64_t samples, float min_iou, double p95_frames,
    double p99_frames, double p95_ms, double p99_ms, bool acceptance,
    roi::transcode::RoiLagStats* out);
void fill_job(const RoiJobStatus& st, roi::transcode::Job* out);

sve::Box box_from_proto(const roi::transcode::Box& b);
RoiJobRequest build_request(const roi::transcode::FileJobRequest& req);
sve::SessionParams params_from_open(const roi::transcode::StreamOpen& open);

// A template because the file job and the stream open carry the same settings under two
// different proto messages. Defined here rather than instantiated twice, which is what the
// two call sites would otherwise do to each other.
template <typename ProtoSettings>
void apply_settings(const ProtoSettings& in, sve::Settings& out)
{
    const sve::BackendChoice choice = from_proto(in.encoder());
    out.encoder = choice.kind;
    out.backend_selection = choice.selection;
    switch (in.codec()) {
        case roi::transcode::CODEC_H264: out.codec = sve::Codec::H264; break;
        case roi::transcode::CODEC_SAME_AS_SOURCE:
            out.codec = sve::Codec::SameAsSource; break;
        default: out.codec = sve::Codec::H265; break;
    }
    if (in.crf() > 0) {
        out.crf = in.crf();
    }
    if (in.gop() > 0) {
        out.gop = in.gop();
    }
    out.preset = to_library_preset(in.preset());

    if (in.has_rate_control()) {
        const auto& rc = in.rate_control();
        if (rc.target_bitrate() > 0) {
            out.rate_control.target_bitrate = rc.target_bitrate();
        }
        if (rc.has_max_average_overshoot()) {
            out.rate_control.max_average_overshoot = rc.max_average_overshoot();
        }
        if (rc.has_critical_importance()) {
            out.rate_control.critical_importance = rc.critical_importance();
        }
        if (rc.has_max_peak_overshoot()) {
            out.rate_control.max_peak_overshoot = rc.max_peak_overshoot();
        }
        if (rc.has_recovery_window_ms()) {
            out.rate_control.recovery_window_ms = rc.recovery_window_ms();
        }
    }

    const auto& q = in.roi();
    if (q.has_background_qp_delta()) {
        out.quality.background_qp_delta = q.background_qp_delta();
    }
    if (q.has_hold_sec() && q.hold_sec() > 0) {
        out.quality.hold_sec = q.hold_sec();
    }
    if (q.max_regions() > 0) {
        out.quality.max_regions = q.max_regions();
    }
    for (const auto& k : q.kinds()) {
        sve::KindQuality cfg;
        if (k.has_qp_delta()) {
            cfg.qp_delta = k.qp_delta();
        }
        if (k.has_pad() && k.pad() > 0) {
            cfg.pad = k.pad();
        }
        out.quality.kinds.emplace(k.kind(), cfg);
    }
    if (q.has_mask_levels() && q.mask_levels() > 0) {
        out.quality.mask_levels = q.mask_levels();
    }
    if (q.has_mask_qp_delta_at_255()) {
        out.quality.mask_qp_delta_at_255 = q.mask_qp_delta_at_255();
    } else {
        // Not asked for: a mask carries one strength, so it takes the strongest
        // kind on offer. Sending boxes and a mask with consistent settings is
        // then one less thing to get right.
        for (const auto& [name, cfg] : out.quality.kinds) {
            (void)name;
            out.quality.mask_qp_delta_at_255 =
                std::min(out.quality.mask_qp_delta_at_255, cfg.qp_delta);
        }
    }
    if (q.has_pad() && q.pad() > 0) {
        out.quality.pad = q.pad();
    }
    if (q.has_max_protected_fraction()) {
        out.quality.max_protected_fraction = q.max_protected_fraction();
    }
    switch (q.margin_mode()) {
        case roi::transcode::MARGIN_MODE_FRACTION_OF_FRAME:
            out.quality.margin_mode = sve::MarginMode::FractionOfFrame;
            break;
        default:
            out.quality.margin_mode = sve::MarginMode::FixedPixels;
            break;
    }
    if (q.has_margin_px()) {
        out.quality.margin_px = q.margin_px();
    }
    if (q.has_min_roi_side()) {
        out.quality.min_roi_side = q.min_roi_side();
    }
    if (q.has_class_weighting()) {
        const auto& p = q.class_weighting();
        auto& dst = out.quality.importance;
        for (const auto& [k, v] : p.semantic_priority()) {
            dst.semantic_priority[k] = v;
        }
        for (const auto& [k, v] : p.task_priority()) {
            dst.task_priority[k] = v;
        }
        if (p.has_unknown_priority()) {
            dst.unknown_priority = p.unknown_priority();
        }
        if (p.has_assumed_confidence()) {
            dst.assumed_confidence = p.assumed_confidence();
        }
        if (p.has_weigh_confidence_on_known_classes()) {
            dst.weigh_confidence_on_known_classes = p.weigh_confidence_on_known_classes();
        }
        if (p.has_temporal_stability_floor()) {
            dst.temporal_stability_floor = p.temporal_stability_floor();
        }
        if (p.has_temporal_stability_tau_sec()) {
            dst.temporal_stability_tau_sec = p.temporal_stability_tau_sec();
        }
        if (p.has_use_motion_weight()) {
            dst.use_motion_weight = p.use_motion_weight();
        }
        if (p.has_motion_weight_floor()) {
            dst.motion_weight_floor = p.motion_weight_floor();
        }
        if (p.has_motion_speed_at_full()) {
            dst.motion_speed_at_full = p.motion_speed_at_full();
        }
    }

    if (in.has_temporal_roi()) {
        const auto& t = in.temporal_roi();
        if (t.has_max_missing_frames()) {
            out.temporal.max_missing_frames = t.max_missing_frames();
        }
        if (t.has_max_hold_sec()) {
            out.temporal.max_hold_sec = t.max_hold_sec();
        }
        if (t.has_confidence_decay()) {
            out.temporal.confidence_decay = t.confidence_decay();
        }
        if (t.has_max_qp_delta_per_frame()) {
            out.temporal.max_qp_delta_per_frame = t.max_qp_delta_per_frame();
        }
        if (t.has_spatial_smoothing()) {
            out.temporal.spatial_smoothing = t.spatial_smoothing();
        }
        if (t.has_temporal_smoothing()) {
            out.temporal.temporal_smoothing = t.temporal_smoothing();
        }
        if (t.has_use_track_prediction()) {
            out.temporal.use_track_prediction = t.use_track_prediction();
        }
    }
}

} // namespace roisvc
