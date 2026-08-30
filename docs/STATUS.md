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

## Что брать следующим агентом (принятый порядок)

Публичный API §8 идёт срезами; каждый срез — один коммит, проверяемый до пуша.

1. **Переименование в `sve`** — `namespace smartvideo` → `sve`, `include/smartvideo/` →
   `include/sve/`, таргет `sve`. Механически, ничего кроме имён. Дёшево: `smartvideo::` за
   пределами `roi_transcode/` не используется нигде.
2. **Фундаментальные типы (§8.3, 8.5–8.7, 8.10):** `InputVideoFormat`, `MediaTimestamp`,
   shared immutable `PacketBuffer`, владеющие `Frame`/`TimedFrame`, `RawFrameView`.
   FFmpeg допускается ровно в одном заголовке — `frame.h`; остальные остаются чистыми, и
   `public_headers_isolated` продолжает это охранять.
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

