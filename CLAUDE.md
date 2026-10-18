# Architecture rules

## What this repository is

`ta_roienc` -- the adaptive ROI video encoding library: code and its C++ unit tests, nothing
else. `ta_vms` (as the `roitrc/ta_roienc` submodule, linked by `media_server` and `roitrc`)
and `video_a` (as the `ta_roienc` submodule, for the decoder) consume it through
`add_subdirectory`. Nothing in `include/` or `src/` knows either consumer exists, and the
targets `headers_isolated` / `public_headers_selfcontained` keep the public headers
free of FFmpeg and self-contained.

If something would only make sense to one consumer, it belongs in that consumer.

## Language conventions

Chat communication with the user is in Russian. Code -- comments, identifiers, commit
messages, log messages, docs -- is always in English.

Everything addressed to the user rather than to the codebase follows the chat, not the
code: **implementation plans (including plan files) and code-review findings are written in
Russian.** The code they talk about stays English, so quoted identifiers, file paths and
code snippets are of course unchanged.

Comments explain the code: why it is shaped the way it is, what would break otherwise.
No references to external specifications, history, dates or past versions of the code.

## Engineering principles (mandatory)

For all new code and refactors, treat these as hard constraints rather than style
preferences:

- **SOLID:** keep single responsibility boundaries clear, design against
  interfaces/abstractions, and avoid coupling unrelated concerns in one class/module.
- **KISS:** prefer the simplest design that meets the requirement; avoid speculative
  abstractions and unnecessary indirection.
- **YAGNI:** do not implement features, extension points, or configuration knobs before
  there is a concrete requirement.
- **DRY:** avoid duplicated business logic and duplicated protocol/format conversions;
  extract shared helpers when two paths must stay behaviorally identical.
- **No magic constants.** A number that carries meaning gets a name and a definition --
  `constexpr` with a `k`-prefixed name at the narrowest scope that covers its uses, and a
  comment saying where the value comes from. `+ 1`, `/ 2` and an index of `0` stay as they
  are, while a quantiser range, an alignment, a block size or a protocol limit does not.

Built with **C++20**. Prefer modern C++20 techniques and standard library facilities over
legacy patterns whenever the result is clearer and safer (`std::span`, ranges, RAII,
`std::optional`, `std::variant`, structured bindings, constexpr helpers, etc.).

One deliberate exception, so nobody "fixes" it: the hand-written `Fallible<T>` in
`include/ta_roienc/verdict.h` is not `std::expected` because that is C++23 and this header
is compiled by the embedding project, not by us.

## Portability and dependency weight

**Code stays portable to Windows where it can.** `std::filesystem` instead of POSIX; no
`unistd.h`, `dirent.h`, `pthread.h`, `realpath`. Platform-specific code is allowed only
behind a backend interface -- VA-API is Linux by nature, which is why backends are split.

**Tests do not pull in heavy dependencies:** no metric or check that needs its own build
system, patches to someone else's code, or downloaded models or datasets.

## Docker images

One: `roi-deps`, a build environment rather than a product -- the toolchain and the vcpkg
tree, from `docker/intel/Dockerfile.deps`:

    docker build -t roi-deps -f docker/intel/Dockerfile.deps docker/intel

**The image tag is the git tree hash of `docker/intel/`** (ta_vms
`scripts/deps-image-tags.sh`). Any change inside that directory means a multi-hour rebuild
and push of the image, so touch it only when the image itself has to change.

`roi-deps` is deliberately not ta_vms' `ta-deps`: this is the only consumer that needs
qsv/vaapi (and the system `libdrm-dev` vcpkg's libva demands), and the only one that does
not link openvino. It is on ubuntu 24.04, matching the runtime image beside it.

The image uses an overlay vcpkg triplet (`docker/intel/triplets/x64-linux.cmake`) to build
release-only -- vcpkg's `VCPKG_BUILD_TYPE` env var is a no-op, it only works from inside a
triplet file. Consumers must still pass `-DCMAKE_BUILD_TYPE=Release` on their own
configure step to select the release libs.

## A stale build directory looks like a source error

Local builds configure against `/mnt/wdblue/vcpkg` in classic mode and link ffmpeg through
`pkg_check_modules(FFMPEG ...)`. That call caches its answer in `CMakeCache.txt` and never
asks pkg-config again. So when ffmpeg is rebuilt there with different features,
`libavutil.a` can gain `hwcontext_vaapi.o` while a build directory configured earlier keeps
a link line with no `-lva`, and the result is a wall of `undefined reference to
vaGetDisplayDRM` that looks like a source problem and is not. Delete the build directory
and reconfigure. Docker builds never hit this -- they configure from scratch.
