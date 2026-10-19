# ta_roienc

Adaptive ROI video encoding: keep the regions a detector found (plates, faces, people)
sharp and spend fewer bits on everything else. C++20, CMake, FFmpeg.

Engines: libx264 and libx265 through their own APIs, Intel oneVPL through its own API
(optional, needs the oneVPL SDK), and QSV, VA-API, libx264 and libx265 through libavcodec
(optional). A standalone hardware-capable decoder ships beside the encoder.

## Build

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset linux-default     # or linux-dev to reuse an installed classic vcpkg tree
cmake --build build -j
ctest --test-dir build
```

## Using it from another project

```cmake
set(TA_ROIENC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TA_ROIENC_INSTALL OFF CACHE BOOL "" FORCE)
set(TA_ROIENC_BUILD_ENGINE_FFMPEG ON CACHE BOOL "" FORCE)   # QSV / VA-API
add_subdirectory(ta_roienc)
target_link_libraries(app PRIVATE ta_roienc_all)
```

```cpp
#include "ta_roienc/ta_roienc.h"

ta_roienc::Recipe s;
s.quality.background_qp_delta = 20;
s.quality.kinds["plate"] = ta_roienc::ClassQuality{-8, 0.01};

// A file.
ta_roienc::Outcome r = ta_roienc::recode_file("in.mp4", "out.mp4", boxes, s);

// Or frames, raw or already compressed, each with a mask or boxes.
ta_roienc::TranscoderOptions p;
p.width = 1280; p.height = 720;
p.codec = ta_roienc::SourceCodec::H264;
p.settings = s;
ta_roienc::Transcoder session(p, &handler);   // or p.output_path to mux a file
session.submit(frame, regions);
ta_roienc::Outcome done = session.finish();
```

`AccessUnitSink` carries no FFmpeg types -- bytes, timestamps and a keyframe flag -- so
writing one needs nothing on the include path but this library's headers.

## License

MIT, see [LICENSE](LICENSE).
