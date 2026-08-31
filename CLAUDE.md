# Architecture rules

## What this repository is

`smart-video-encoder` — the adaptive ROI encoder built to the customer's specification
(`docs/smart-video-encoder_41.md`, the version in force). It left the `ta_vms` monorepo on
2026-08-31, at commit `c4bc9b8` of that repository, because it had become a deliverable of
its own with its own specification, its own acceptance tests and its own dependency
profile. The whole history came with it — `git log` here goes back to the first commit.

`ta_vms` is now a **client**: it deploys `roi-transcode-svc` as a prebuilt image and calls
it over `proto/roi_transcode.proto`, of which it keeps its own copy for its C# `VmsFs`
client. Nothing in `include/` or `src/` knows ta_vms exists, and the targets
`public_headers_isolated` / `public_headers_selfcontained` are what keep that true.

## Language conventions

Chat communication with the user is in Russian. Code — comments, identifiers, commit
messages, log messages, docs — is always in English.

Everything addressed to the user rather than to the codebase follows the chat, not the
code: **implementation plans (including plan files) and code-review findings are written in
Russian.** `docs/STATUS.md` is Russian for that reason. The code they talk about stays
English, so quoted identifiers, file paths and code snippets are of course unchanged.

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

Built with **C++20**. Prefer modern C++20 techniques and standard library facilities over
legacy patterns whenever the result is clearer and safer (`std::span`, ranges, RAII,
`std::optional`, `std::variant`, structured bindings, constexpr helpers, etc.). Do not
introduce pre-C++20 workarounds where a direct C++20 solution is available.

One deliberate exception, so nobody "fixes" it: the hand-written `Result<T>` in
`include/sve/status.h` is not `std::expected` because that is C++23 and this header is
compiled by the embedding project, not by us.

## Переносимость и вес зависимостей

Две конвенции, принятые 2026-08-23 (полностью, с обоснованием — `docs/STATUS.md`,
решения Р1 и Р2):

**Код по возможности переносим на Windows.** `std::filesystem` вместо POSIX; никаких
`unistd.h`, `dirent.h`, `pthread.h`, `realpath`. Сегодня это выполняется: в `include/`,
`src/`, `tests/` и `integration/` нет ни одного POSIX-заголовка. Платформенное
допускается только за интерфейсом бэкенда — VA-API Linux по природе, и ровно ради этого
бэкенды разделены. Скрипты (`check.sh`, `rd_curve.sh`) и docker-образы остаются Linux.

**Тест не тянет за собой тяжёлых зависимостей.** Не добавляем метрику или проверку,
которой нужна отдельная система сборки, патчи к чужому коду, скачивание моделей или
датасетов. Именно поэтому VMAF отложен: порт `libvmaf` требует meson, модель
доставляется отдельно, а сам порт объявлен `"supports": "!windows"`.

## The library / integration split is load-bearing

`include/`, `src/`, `tests/` and `benchmarks/` are the library the specification describes
(§52). Everything of ours that the specification does not mention — the gRPC service, its
JSON adapter, the CLI, the scripts — lives under `integration/` and is built only with
`SVE_BUILD_INTEGRATION=ON`, which is **off** by default. `check.sh` configures the library
with the integration switched off on every run, so the separation is checked rather than
merely intended.

§20 of the specification makes FFmpeg opt-in interop rather than the engine. Direct
libx264 and libx265 backends exist for that reason; keep new encoder work on that side of
the line, and keep `include/sve/frame.h` the only public header that mentions an FFmpeg
type.

## Docker images

Two: `roi-deps` (the toolchain and the vcpkg tree, built from
`docker/intel/Dockerfile.deps`) and `treealarm/roitrc:latest` on top of it
(`docker/intel/Dockerfile`). `./docker/build.sh` builds both. That image name is ta_vms'
existing convention for this service, not a new one -- its compose and `ta_install`'s both
already name it, so keep it.

`roi-deps` is deliberately **not** ta_vms' `ta-deps`, and the split is load-bearing: this
is the only consumer that needs qsv/vaapi (and the system `libdrm-dev` vcpkg's libva
demands), and the only one that does not link openvino — which is 143 of the 234 minutes
`ta-deps` takes to build. It is on ubuntu 24.04, matching the runtime image beside it:
glibc is only forward-compatible, so builder and runtime being the same release is what
keeps that safe.

Both images use an overlay vcpkg triplet (`docker/intel/triplets/x64-linux.cmake`) to
build release-only — vcpkg's `VCPKG_BUILD_TYPE` env var is a no-op, it only works from
inside a triplet file. Consumers must still pass `-DCMAKE_BUILD_TYPE=Release` on their own
configure step to select the release libs.

## A stale build directory looks like a source error

Local builds configure against `/mnt/wdblue/vcpkg` in classic mode and link ffmpeg through
`pkg_check_modules(FFMPEG ...)`. That call caches its answer in `CMakeCache.txt` and never
asks pkg-config again. So when ffmpeg is rebuilt there with different features,
`libavutil.a` can gain `hwcontext_vaapi.o` while a build directory configured earlier keeps
a link line with no `-lva`, and the result is a wall of `undefined reference to
vaGetDisplayDRM` that looks like a source problem and is not: the `.pc` file is already
correct, the cache is not. Delete the build directory and reconfigure. Docker builds never
hit this — they configure from scratch.

## Fixtures are generated, not committed

`testdata/synth.mp4`, `testdata/scenes/` and `runtime/` are gitignored. A fresh checkout
produces them with `integration/scripts/fetch-intel-qsv.sh` (once, no sudo) and
`integration/scripts/make-synth.sh` / `benchmarks/synthetic/make-scenes.py`.

The two are not handled alike, and the difference is worth knowing before trusting a green
run: a missing `synth.mp4` is generated and a failure there stops `check.sh` outright,
while `make-scenes.py` is called with `|| true` and the whole synthetic-scene block is then
skipped in silence. So on a machine that cannot build the scenes, `check.sh` still says
"all checks passed" having never run the moving-object assertions. Read the step count, not
just the last line.
