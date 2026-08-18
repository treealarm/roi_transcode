#pragma once

// The vocabulary both entry points share.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace roi {

// Which backend does the encoding. Says nothing about the codec -- the same
// silicon encodes both, and conflating the two is what made the old X265 name
// wrong the moment H.264 output existed.
enum class Encoder {
    // Tries Quick Sync, then VA-API, then the CPU. Which one opened is reported
    // back, because on this hardware the difference is worth knowing.
    Auto,
    Qsv,
    Vaapi,
    Nvenc,
    // libx264 or libx265, whichever the codec asks for.
    Cpu,
};

// What comes out. HEVC saves more, but a ta_vms archive is H.264 and one HLS
// playlist stitches many clips: re-encoding some of them to HEVC would leave a
// single playlist carrying two codecs, which browsers handle badly.
enum class Codec {
    H264,
    H265,
};

enum class InputCodec {
    // Tightly packed planes in the pixel format named by the frame.
    Raw,
    H264,
    H265,
    Jpeg,
};

// One region worth keeping sharp. Coordinates are normalized 0..1 from the
// top-left corner -- the same convention ta_vms archives boxes in.
struct Box {
    // Seconds from the start of the input. Only meaningful for a file, where
    // boxes lie on a timeline; a submitted frame carries the boxes that are its.
    double t_sec = 0;
    std::string kind;
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

// How much better than the background one kind of region gets.
struct KindQuality {
    // Negative is sharper.
    int qp_delta = -6;
    // Halo around the box as a fraction of the frame, encoded at half the box's
    // delta, which softens the QP step at the edge.
    double pad = 0.02;
};

// Everything that decides how quality is redistributed. One of these serves
// both boxes and masks: a mask has no per-kind names, so it interpolates
// between the background and `mask_qp_delta_at_255`.
struct Quality {
    // 0 leaves the background alone -- not the same as "unset".
    int background_qp_delta = 6;
    // How long a box keeps applying after its sample, in seconds. Files only.
    double hold_sec = 0.25;
    // Cap on regions handed to the encoder: mfxExtEncoderROI takes at most 256
    // and a VA-API driver usually far fewer.
    int max_regions = 32;
    std::unordered_map<std::string, KindQuality> kinds;

    // Mask input only: the strength a mask value of 255 asks for, and how many
    // steps the 0..255 range is quantized into. Fewer steps means fewer
    // distinct deltas and so fewer rectangles to spend the budget on.
    int mask_qp_delta_at_255 = -6;
    int mask_levels = 4;
};

struct Settings {
    Encoder encoder = Encoder::Auto;
    Codec codec = Codec::H265;
    // Constant quantizer / CRF.
    int crf = 28;
    // Keyframe interval in frames; 0 means two seconds.
    int gop = 0;
    std::string preset = "medium";
    Quality quality;
    // Muxer "movflags", file output only. Empty leaves the container default;
    // a ta_vms archive clip needs fragmented mp4 to stay playable.
    std::string movflags;
};

// A box resolved to pixels, with its halo. Internal to the geometry, but named
// here because the geometry header is public for its tests.
struct Detection {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    int qp_delta = -6;
    int pad_qp_delta = -3;
    int pad_x = 0;
    int pad_y = 0;
};

// What an encoder actually takes: a pixel rectangle and a QP delta. On overlap
// the lowest index wins.
struct Rect {
    int top = 0;
    int bottom = 0;
    int left = 0;
    int right = 0;
    int qp_delta = 0;
};

} // namespace roi
