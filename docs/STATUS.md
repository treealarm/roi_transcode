# Текущий статус (handoff)

Дата среза: 2026-08-30. **Сверка идёт против ТЗ 3.4**
([`smart-video-encoder_34.md`](smart-video-encoder_34.md)), не против 3.1.

Коротко: библиотечный трек в части ROI/temporal/rate-control существенно продвинут — то, что
ниже перечислено как сделанное, сделано и проверяется `integration/scripts/check.sh`. Но
3.4 переписала публичный API (§8.3–8.13), и вот этого в коде нет вовсе: вход по-прежнему
`Session` и `transcode_file`. Полный разбор расхождений —
[`plan-v2.md`](plan-v2.md), раздел «Изменения ТЗ v3.1 → v3.4».

## Что уже сделано (по коду)

- **Slice A (§11.1):** публичная лестница приоритетов `ObjectPriority` внедрена.
- **Slice B (§11.2–11.3):** геометрические дефолты v3 (`min_roi_side`, `margin_px`) внедрены.
- **Slice C (§13):** temporal anti-flicker (smoothing/slew/prediction) реализован на library path.
- **Slice D (§15–16):**
  - CPU H.264: `target_bitrate` работает через CBR+filler.
  - Matched-bitrate проверяется скриптом `matched-bitrate.sh` (две acceptance-gates).
  - CPU H.265: native bitrate path отключен (fallback на CRF/CQP) из-за стабильности.
- **Slice E (§14):** ROI lag хелперы + runtime observability в `Report` и логах
  для `transcode_file` и `Session` path.
- **Service telemetry:** ROI lag и CPU H.265 guard прокинуты в gRPC/job ответы
  (`Job`, stream `Done`) и в статус-событие `roi-transcode-job-changed`.
- **SEI / stability:** для CPU H.265 отключен `sei` capability (практический guard от крэшей ROI+SEI).

## Последние шаги (что именно добавили недавно)

- Явный guard-лог для CPU H.265 при `target_bitrate > 0`:
  - `"guard: cpu h265 target-bitrate disabled; using CRF/CQP"`.
- Явный guard-лог для отключённого HEVC SEI path:
  - `"warning: bitstream SEI requested but not supported by encoder capabilities"`.
- Флаг в `Report`: `cpu_h265_target_guard` (fallback виден через API, не только в stderr).
- Метрики ROI lag в `Report`:
  - `roi_lag_measured`, `roi_lag_samples`, `roi_lag_min_iou`,
  - `roi_lag_p95_frames`, `roi_lag_p99_frames`,
  - `roi_lag_p95_ms`, `roi_lag_p99_ms`, `roi_lag_acceptance`.
- Интеграционные проверки в `check.sh`:
  - guard CPU H.265 виден в логах;
  - ROI lag метрики наблюдаемы на tracked ROI и проходят acceptance.
- Добавлен optional `CPU H.265` stability matrix:
  `integration/scripts/h265-stability-matrix.sh` + шаг в `check.sh`
  (`RUN_H265_MATRIX=1`) с CSV-отчётом по guard-сигналам.
- В real-clip ROI prepare/check path добавлена нормализация track IDs:
  если входные аннотации без треков, назначается synthetic `track_id=1`, чтобы
  lag telemetry оставалась измеримой и шаг не деградировал в `not measured`.

## На какой мы стадии сейчас

- По порядку из `plan-v2.md`: практически закрыты этапы 1–4 на текущем уровне реализации.
- Сверх того открылся новый и самый крупный фронт — публичный API 3.4 (§8.3–8.13).
- Прочие незакрытые направления: NVENC полноценный ROI path, Rockchip MPP backend,
  HW-native bitrate path с сохранением ROI semantics, датасеты/quality stack уровня
  приёмки ТЗ (VMAF/task-aware/regression framework).

## Лимит ROI-регионов у Intel — разный по кодекам (2026-08-30)

`roi-transcode --encoder qsv --codec h264` **падал** на реальном клипе (960×540, шесть
отслеживаемых машин): `*** stack smashing detected ***` внутри iHD, в потоке рантайма,
без единого сообщения об ошибке. Падение старше `d514901` — воспроизводится и на
`4148757`, и на `bb66ab8`.

Причина: оба Intel-бэкенда объявляли **один** лимит на оба кодека (8), хотя
`GetCapabilities` принимает `Codec` именно потому, что ответ разный. Настоящий лимит iHD
для H.264 — **четыре**. Подтверждено независимо от падения: на VA-API-пути libavcodec сам
подрезает список по `VAConfigAttribEncROI`, и файл на 4, 5 и 6 регионах побайтно одинаков,
а на 3 — уже другой. То есть регионы с пятого драйвер просто игнорирует.

Два API ломаются по-разному, поэтому это и всплыло поздно:
- **VA-API** — libavcodec тримит сам, ничего не падает, но регионы теряются **тихо**, мимо
  нашего `regions_dropped`;
- **oneVPL** — libmfx-gen передаёт всё как есть, iHD переполняет массив, процесс умирает.

Починено: `src/backend/common/intel_roi_limit.h` — один per-codec лимит на оба бэкенда, и
усечение теперь громкое (`warn_roi_regions_dropped_once`, §50 запрещает тихую деградацию).
H.265 не затронут: выход бит-в-бит тот же.

**Что осталось непонятым:** при каком именно раскладе регионов драйвер переполняет массив,
а при каком просто игнорирует лишние. Синтетика на 8 регионах не падает ни на 1280×720, ни
на 960×540 — падает только реальный клип с плотными мелкими боксами. Для фикса это неважно
(мы больше не превышаем лимит), но регрессия на синтетике проверяет **контракт**, а не
падение; сам крэш под тестом только в опциональном шаге на реальном клипе.

## Что брать следующим агентом (принятый порядок)

Публичный API §8 идёт срезами; каждый срез — один коммит, проверяемый до пуша.

1. ~~**Переименование в `sve`**~~ — **сделано 2026-08-30.** `namespace smartvideo` → `sve`,
   `include/smartvideo/` → `include/sve/`, `smartvideo.h` → `sve.h`, таргеты `sve` и
   `sve_adaptive`. На переходный период оставлены alias `namespace smartvideo = sve` в
   `include/sve/sve.h` и CMake-alias'ы `smartvideo` / `smartvideo_adaptive` / `roi_core` /
   `roi_geometry`; они снимаются вместе с `Session` в срезе 5.
2. ~~**Фундаментальные типы (§8.3, 8.5–8.7, 8.10)**~~ — **сделано 2026-08-30.**
   `include/sve/input_format.h`, `timestamp.h`, `packet.h`, `frame.h`. FFmpeg допущен ровно
   в один заголовок — `frame.h`, и он намеренно **не** включается из `sve.h`, поэтому
   `public_headers_isolated` по-прежнему компилирует всю остальную поверхность без FFmpeg.
   Что изменилось по существу, а не по именам:
   - `EncodedPacket` переехал из `packet_handler.h` в `packet.h` и держит payload в
     shared immutable `PacketBuffer` вместо заимствованного span'а с контрактом «до
     следующего вызова». Цена — одна аллокация и копия на пакет.
   - `WrapRawFrame` **удерживает** `RawFrameView::owner` внутри самого `AVFrame`
     (через `av_buffer_create`), а не только требует этого от вызывающего.
   - `StatusCode::InvalidArgument` добавлен (§25) — по месту использования, остальные
     коды §25 придут в срезе 3.
   - `TimedFrame` отложен до среза 3: он состоит из `FrameMetadata`, а её определяет §8.1
     вместе с самим энкодером.
   Покрытие: `tests/unit/frame_test.cpp` (девять случаев, из них шесть — отказы).
3. **`SmartVideoEncoder` и lifecycle (§8.1, 8.13, §25):** submit/receive, `EncoderState`,
   `Reconfigure`, `RequestKeyFrame`, `EncoderStatistics`; `StatusCode` дополняется кодами
   §25. Садится поверх существующих `IEncoderBackend` и `frame_roi_pipeline` — нижний слой
   уже соответствует §21.
4. **`BackendSelection` и `FallbackPolicy` (§8.4, §22)** + инжектируемый log sink: §50
   запрещает silent fallback, а логирование сейчас идёт прямым `fprintf(stderr)`.
5. **Push facade и снятие `Session` (§8.8–8.9):** `PushVideoEncoder` с типизированными
   событиями; `Session` — тонкий адаптер, потом удаляется. `transcode_file` остаётся
   примером приложения (§52).
6. **`CodecParameters` и фабрики (§8.11–8.12).**

Потом — долги 3.2/3.3, в этом порядке: CBOR + CRC-32C и лимит 16 KiB в SEI (§19.2),
sidecar NDJSON (§19.3), `sve-capabilities` CLI (§23), 10-bit P010/Main10 и AT-009,
GitLab CI (§44), Python wrapper (§58 — отложен осознанно, но не снят: он в DoD).

**Контракт CPU H.265 держится до отдельного снятия:** `target_bitrate` всегда
даунгрейдится в CRF/CQP, `sei=false`, guard обязан быть наблюдаем в логах и телеметрии
(`Report` / gRPC / job event). Вернуть SEI и native bitrate можно только отдельным срезом с
regression-покрытием, не тихой сменой поведения.

## Быстрые команды проверки

```bash
# из roi_transcode/
BUILD=build/integration LIBBUILD=build ./integration/scripts/check.sh
RUN_H265_MATRIX=1 BUILD=build/integration LIBBUILD=build ./integration/scripts/check.sh
./integration/scripts/matched-bitrate.sh <video> <roi.json> <out_dir> [target_bps] [target_tol_frac]
# optional: prepare local real-lag fixture used by check.sh
bash ./integration/scripts/prepare-real-lag-fixture.sh <source.mp4> <roi.json>
```

