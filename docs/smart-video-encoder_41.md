# Техническое задание. Smart Video Encoder (`smart-video-encoder`)

**Версия:** 4.1
**Статус:** Draft / для утверждения  
**Дата:** 2026-08-31  
**Целевая ОС P0:** Ubuntu 24.04, Linux x86_64  
**Система сборки:** CMake + vcpkg  
**CI/CD:** GitLab CI  
**Основной сценарий:** transcoding + adaptive recompression с приоритетом качества детектированных объектов.

## История изменений

| Версия | Изменения |
|---|---|
| 4.1 | Удалён FFmpeg encoder backend; software encoding реализуется независимыми direct backend-модулями `X264Backend` и `X265Backend` поверх libx264/libx265; FFmpeg оставлен только как опциональный `AVFrame` interop-компонент; software fallback полностью определяется пользовательским `BackendSelection::preferred`; добавлены codec-native quant-offset map, отдельные capability/CI/benchmark/license gates для x264 и x265. |
| 4.0 | FFmpeg преобразован из обязательного нижнего слоя в независимый software peer backend; NVIDIA NVENC, Intel oneVPL и Rockchip MPP реализуются как независимые native backend modules без зависимости от FFmpeg/libavcodec; backend'ы поставляются как опциональные runtime-модули со стабильной C ABI регистрации; сохранены публичный API без FFmpeg (кроме opt-in `sve/frame.h`), пользовательский `BackendSelection::preferred`, единые factory-функции и session-based backend interface для конкурентного кодирования нескольких потоков. |
| 3.4 | Рефакторинг публичного API (8.3–8.13): входной формат выделен в `InputVideoFormat`; добавлен управляемый порядок выбора backend через `BackendSelection`; введены владеющие `Frame`/`TimedFrame` и `RawFrameView` с явным owner; `EncodedPacket` переведён на shared immutable `PacketBuffer`; добавлен `MediaTimestamp` с явным `time_base`; введён push facade `PushVideoEncoder` и типизированные события packet/EOS/error без использования `nullptr`; добавлен `CodecParameters` с extradata; добавлены единые factory-функции; определена lifecycle-модель `EncoderState` и семантика `Stop`/`Flush`. |
| 3.3 | Правки по аудиту: методика ROI-метрик и раздельные метрики synthetic/natural (34, 35, 49); Python P0 синхронизирован с CI/тестами (30, 44, 47, 48, 49); покрытие 10-bit P010/Main10 (15.3, 32, 48, 49, 55); пиннинг MOT17 моделей (36.1); статистический протокол: прогоны, seeds, агрегация, coverage gate (41); `FallbackPolicy` (22, Прил. C); расширен `EncoderCapabilities` (7.2); GOP в секундах (15); согласованность rolling bitrate с конфигом (16); параллельные hardware-стадии (44); export/ABI/semver и стратегия Rockchip MPP (26.3, 26.4); retention артефактов (45); исправлен пример (52); RGB выведен из DoD (55); раздел 49 — единственный нормативный источник порогов (14, 16); Приложение C «Открытые определения API»; исправлена иерархия заголовков |
| 3.2 | `time_base` и правила PTS; `BinaryMask`; правила CQ/CQP; Submit/Receive backend interface; thread-safety контракт; CBOR + CRC-32C + лимиты входа; sidecar-формат; `RequestKeyFrame`/`Reconfigure` |
| 3.1 | Базовая версия, переданная на аудит |

---

# 1. Назначение

Разработать C++ библиотеку `smart-video-encoder` для аппаратного и программного кодирования видеопотока с адаптивным распределением качества по пространственным областям кадра.

Основная идея:

> Области кадра, содержащие объекты машинного зрения, должны кодироваться с более высоким приоритетом качества, чем фон, при сохранении заданной политики bitrate и latency.

Библиотека не выполняет детекцию объектов самостоятельно. Внешняя система передаёт для каждого кадра метаданные: bounding boxes, polygons, masks, track IDs, confidence и другие атрибуты. Библиотека преобразует эти данные в backend-specific ROI/QP/emphasis maps.

Проект должен поддерживать:
- offline transcoding;
- adaptive recompression;
- кодирование видеопотока камеры;
- CPU frames;
- `AVFrame` через opt-in FFmpeg interop adapter;
- CUDA-backed `AVFrame` / GPU memory;
- backend NVIDIA NVENC;
- backend Intel oneVPL;
- backend Rockchip MPP;
- direct software backend libx264 для H.264;
- direct software backend libx265 для H.265;
- FFmpeg только как опциональный `AVFrame` interop adapter, не как encoder backend.

---

# 2. Цели

## 2.1 Функциональные цели P0

1. Кодирование H.264/AVC.
2. Кодирование H.265/HEVC.
3. Object-first / ROI-aware compression.
4. Frame-level и region-level QP control там, где это поддерживается backend.
5. Поддержка нескольких ROI на кадр.
6. Temporal persistence ROI.
7. Сглаживание изменения качества между кадрами.
8. Передача пользовательской metadata, синхронизированной с PTS.
9. Работа с CPU и hardware frames.
10. Unified C++ API независимо от backend.
11. Capability discovery.
12. Fallback при отсутствии конкретной аппаратной функции.
13. Полная автоматическая сборка и тестирование в GitLab CI.
14. Воспроизводимый benchmark protocol.
15. Python wrapper с API, функционально эквивалентным основным сценариям публичного C++ API.

## 2.2 Архитектурные цели P1

1. Подготовка к H.266/VVC.
2. Возможность добавления AV1.
3. Дополнительные backend'ы без изменения публичного API.
4. Block/CTU-level quality map.
5. Multi-object priority scheduling.
6. Dynamic rate-budget allocation между ROI.

---

# 3. Не входит в scope

В P0 не входят:

- встроенный detector;
- встроенный OCR;
- tracker;
- обучение ML-моделей;
- собственный decoder;
- полноценный video container/muxing framework;
- Windows production support;
- собственная реализация H.264/H.265 codec;
- гарантированная идентичность битстрима между backend'ами.

Windows/Docker Desktop допускаются только как development/CI environment P2 и не являются production target.

---

# 4. Поддерживаемые кодеки

## P0

| Codec | Статус |
|---|---|
| H.264 / AVC | обязательно |
| H.265 / HEVC | обязательно |

## Архитектурно

| Codec | Статус |
|---|---|
| H.266 / VVC | API-ready, backend optional |
| AV1 | optional future backend |
| Другие | через расширяемый codec registry |

Публичный API не должен содержать жёсткую зависимость от H.264/H.265-specific структур, если параметр может быть представлен общим интерфейсом.

---

# 5. Целевые backend'ы

## 5.1 NVIDIA

`NvencBackend` — native backend, использующий NVIDIA NVENCODE API напрямую. Он не должен
линковаться с FFmpeg/libavcodec и не использует FFmpeg NVENC encoders. FFmpeg interop adapter
может преобразовать `AVFrame` в `IFrameBuffer` до вызова backend, не создавая зависимости
`NvencBackend -> FFmpeg`.

Основной путь:

```text
AVFrame / CUDA frame
        |
        v
smart-video-encoder
        |
        v
NvencBackend
        |
        v
NVIDIA NVENCODE API
        |
        v
NVENC
```

Требования:
- runtime capability detection;
- CUDA zero-copy, где технически возможно;
- использование backend-specific возможностей ROI/QP/emphasis map, если они доступны;
- отсутствие предположения, что одинаковые функции доступны на всех поколениях GPU.

## 5.2 Intel

`OneVplBackend` — native backend, использующий Intel oneVPL напрямую и не зависящий от
FFmpeg/QSV.

Основной путь:

```text
AVFrame / supported hardware surface
       |
       v
smart-video-encoder
       |
       v
OneVplBackend
       |
       v
Intel oneVPL
```

Для ROI использовать capability query. Поддерживаемый максимум ROI и доступные режимы определяются runtime API.

При наличии `DeltaQP`:
- отрицательная коррекция QP повышает качество;
- координаты и карты должны быть приведены к ограничениям backend;
- библиотека не должна предполагать фиксированное число ROI.

## 5.3 Rockchip

`RockchipMppBackend` использует Rockchip MPP напрямую и не зависит от FFmpeg/libavcodec.

Поддерживаемые P0:
- H.264;
- H.265;
- ROI;
- CTU/QP control, если доступно конкретному SoC/MPP;
- SEI/user data, если доступно.

Целевые SoC определяются capability layer. Минимальный hardware validation target должен включать один актуальный Rockchip SoC с H.264/H.265 encoder и ROI.

## 5.4 Direct software backend'ы

Software encoding реализуется двумя независимыми peer backend-модулями:

- `X264Backend` — H.264 через direct libx264 API;
- `X265Backend` — H.265 через direct libx265 API.

FFmpeg/libavcodec в software encode path не используется. Canonical importance map
преобразуется непосредственно в codec-native quantization offsets на сетке backend без
промежуточного преобразования в `AVRegionOfInterest`.

`X264Backend` формирует macroblock-level offsets и передаёт их через поддерживаемый pinned
libx264 API. `X265Backend` выполняет отдельное codec-specific преобразование в доступный
CU/CTU-oriented control pinned libx265 API. Backend'ы не обязаны иметь одинаковые grid,
диапазон, знак или единицы offsets; преобразование, clipping и lifetime map фиксируются в
adapter contract и telemetry.

Назначение:
- CI;
- deterministic functional tests;
- отсутствие hardware;
- reference baseline.

Software backend не обязан обеспечивать все object-first возможности hardware backend. В capability report должен быть явно указан фактический native control mode.

## 5.5 Независимость backend'ов

Все backend'ы являются peer-модулями. Нормативный dependency graph:

```text
sve_core
  +-- optional --> sve_backend_x264 --> libx264
  +-- optional --> sve_backend_x265 --> libx265
  +-- optional --> sve_backend_nvenc --> NVIDIA NVENCODE API
  +-- optional --> sve_backend_onevpl --> Intel oneVPL
  +-- optional --> sve_backend_rockchip --> Rockchip MPP
  +-- optional --> sve_ffmpeg_interop --> FFmpeg libavutil
```

Encoder backend modules не зависят от FFmpeg. Fallback выполняется между peer-backend'ами
через общий capability registry, а не внутри backend. FFmpeg interop только преобразует
внешний `AVFrame` в `Frame`/`IFrameBuffer` и не выполняет кодирование.

---

# 6. Архитектура

```text
+---------------------------------------------------------+
|                    Application                          |
+-------------------------+-------------------------------+
                          |
                          v
+---------------------------------------------------------+
|                SmartVideoEncoder API                    |
|                                                         |
| EncoderConfig | Frame | ObjectMetadata | ROI Policy     |
+-------------------------+-------------------------------+
                          |
                          v
+---------------------------------------------------------+
|                 Adaptive Control Layer                  |
|                                                         |
| ROI Normalizer                                           |
| Priority Resolver                                        |
| Temporal ROI Manager                                     |
| Motion/Track Predictor                                   |
| QP / Quality Map Generator                               |
| Bitrate Budget Controller                                |
+-------------------------+-------------------------------+
                          |
                          v
+---------------------------------------------------------+
|                  Capability Layer                       |
| x264 | x265 | NVENC native | oneVPL native | MPP native|
+-------------------------+-------------------------------+
                          |
                          v
+---------------------------------------------------------+
|                 Backend Adapters                        |
|                                                         |
| X264Backend                                              |
| X265Backend                                              |
| NvencBackend                                             |
| OneVplBackend                                            |
| RockchipMppBackend                                       |
+---------------------------------------------------------+
                          |
                          v
+---------------------------------------------------------+
|                    EncodedPacket                        |
+---------------------------------------------------------+
```

---

# 7. Основные архитектурные принципы

## 7.1 Separation of policy and mechanism

Политика:

```text
что считать важным
какой объект приоритетнее
насколько понизить QP
как долго сохранять ROI
какой bitrate budget выделить
```

не должна зависеть от конкретного encoder.

Механизм:

```text
ROI rectangle
DeltaQP
QP map
CTU QP
emphasis map
backend fallback
```

реализуется backend adapter.

## 7.2 Capability-driven design

Каждый backend возвращает:

```cpp
struct EncoderCapabilities {
    bool available;
    bool h264;
    bool h265;
    bool vvc;

    bool roi_rectangles;
    bool delta_qp;
    bool qp_map;
    bool ctu_qp;
    bool emphasis_map;

    bool sei_user_data;
    bool cuda_frames;
    bool zero_copy_input;

    uint32_t max_roi_regions;
    uint32_t qp_map_block_width;
    uint32_t qp_map_block_height;

    bool cbr;
    bool vbr;
    bool low_latency;

    bool av1;                 // архитектурная готовность (P1)
    bool main10;              // HEVC Main10 / 10-bit encode
    bool p010_input;          // приём PixelFormat::P010
    bool b_frames;

    uint32_t max_width;
    uint32_t max_height;
    int32_t min_qp;
    int32_t max_qp;
};
```

Запрещается silently fallback без записи в telemetry.

---

# 8. Публичный API

## 8.1 Создание encoder

```cpp
namespace sve {

class SmartVideoEncoder {
public:
    virtual ~SmartVideoEncoder() = default;

    virtual Status SubmitFrame(TimedFrame frame) = 0;

    virtual Status ReceivePacket(
        EncodedPacket* packet) = 0;

    virtual Status Flush() = 0;

    virtual Status RequestKeyFrame() = 0;

    virtual Status Reconfigure(
        const RuntimeEncoderSettings& settings) = 0;

    virtual EncoderCapabilities GetCapabilities() const = 0;

    virtual EncoderStatistics GetStatistics() const = 0;
};

}
```

Семантика вызовов:

- `SubmitFrame` неблокирующий: при заполненном input queue возвращает `Again` (backpressure); кадр не принимается частично.
- `ReceivePacket` неблокирующий: возвращает `Again`, если готового пакета нет, и `EndOfStream` после завершения flush.
- Соответствие «кадр → пакет» — 1 → 0..N: encoder может буферизовать кадры (B-frames, lookahead) и выдавать пакеты с задержкой в decode order.
- `RequestKeyFrame` запрашивает IDR/keyframe для ближайшего принятого кадра; выполняется best-effort и отражается в statistics.
- `Reconfigure` применяет изменяемые на лету параметры (см. `RuntimeEncoderSettings` в 8.2). Недопустимые для runtime изменения (codec, resolution, backend, pixel format) возвращают `InvalidArgument`.
- `Flush` переводит encoder в draining: после него `SubmitFrame` возвращает `InvalidArgument`, а `ReceivePacket` выдаёт оставшиеся пакеты до `EndOfStream`.

Основной интерфейс `SmartVideoEncoder` не зависит от FFmpeg. Overload
`SubmitFrame(const AVFrame*, const FrameMetadata&)` объявляется только в opt-in interop
header `sve/frame.h` как adapter над `Frame::Ref` и основным `SubmitFrame(TimedFrame)`.

## 8.2 Конфигурация

```cpp
enum class Codec {
    H264,
    H265,
    VVC
};

enum class Backend {
    Auto,
    X264,
    X265,
    NvidiaNvenc,
    IntelOneVpl,
    RockchipMpp
};

enum class RateControl {
    CBR,
    VBR,
    CQ,
    CQP,
    BackendDefault
};

enum class CodecProfile {
    Auto,
    H264Baseline,
    H264Main,
    H264High,
    H265Main,
    H265Main10
};

struct BackendSelection {
    std::vector<Backend> preferred;
    bool allow_unlisted = false;
};

struct EncoderConfig {
    Codec codec = Codec::H265;
    Backend backend = Backend::Auto;
    BackendSelection backend_selection;
    InputVideoFormat input;
    CodecProfile profile = CodecProfile::Auto;

    uint64_t target_bitrate = 0;
    uint64_t max_bitrate = 0;
    uint64_t vbv_buffer_size = 0;

    uint32_t gop_size = 60;
    uint32_t max_b_frames = 0;

    RateControl rate_control = RateControl::VBR;

    // Обязателен при RateControl::CQP: постоянный QP в единицах backend.
    std::optional<uint32_t> qp;

    // Обязателен при RateControl::CQ: целевой quality level backend.
    std::optional<uint32_t> cq_level;

    bool low_latency = true;

    AdaptiveConfig adaptive;
    TemporalROIConfig temporal_roi;
    MetadataConfig metadata;
};
```

Параметры, изменяемые на лету через `Reconfigure`:

```cpp
struct RuntimeEncoderSettings {
    std::optional<uint64_t> target_bitrate;
    std::optional<uint64_t> max_bitrate;
    std::optional<Rational> frame_rate;
    std::optional<AdaptiveConfig> adaptive;
};
```

Правила конфигурации:

- `CQ` — постоянное «качество» с переменным bitrate (backend-специфический quality level); `CQP` — фиксированный QP без rate control. Для обоих режимов bitrate acceptance (раздел 16) не применяется; вместо этого `qp`/`cq_level` фиксируются в benchmark manifest.
- `PixelFormat::P010` требует `CodecProfile::H265Main10` либо `Auto` с автоматическим выбором Main10; несовместимая пара `input.pixel_format`/`profile` приводит к исключению со статусом `InvalidArgument` при создании encoder.
- `qp`/`cq_level` вне диапазона backend возвращают `InvalidArgument` с указанием допустимого диапазона в сообщении ошибки.

## 8.3 Входной видеоформат

Формат входа отделяется от параметров кодирования. В публичном словаре используются только
собственные стабильные типы библиотеки:

```cpp
struct Rational {
    int32_t numerator = 0;
    int32_t denominator = 1;
};

enum class PixelFormat {
    Yuv420P,
    Nv12,
    P010,
};

struct InputVideoFormat {
    PixelFormat pixel_format = PixelFormat::Yuv420P;
    uint32_t width = 0;
    uint32_t height = 0;
    Rational frame_rate{30, 1};
    Rational time_base{1, 90000};
};
```

`EncoderConfig` содержит поле `InputVideoFormat input`; дублирующие поля `width`, `height`,
`frame_rate`, `time_base` и `pixel_format` непосредственно в `EncoderConfig` отсутствуют.
Изменение формата после начала обработки запрещено и возвращает `InvalidArgument`. Это
позволяет конфигурировать формат до запуска, использовать один контракт для `AVFrame` и raw
planes и не смешивать характеристики входа с rate-control.

Основной публичный заголовок `sve/sve.h` и все публичные заголовки, кроме FFmpeg interop
header `sve/frame.h`, не должны включать заголовки FFmpeg, упоминать типы FFmpeg в
декларациях или требовать FFmpeg include paths у потребителя. `sve/frame.h` содержит
изолированный interop API для `AVFrame` и подключается только пользователями, которым он
необходим. Преобразования `Rational`/`PixelFormat` в типы FFmpeg выполняются внутри interop
или backend implementation и не являются частью стабильного ABI библиотеки.

Это свойство проверяется отдельной CI compile-target: она компилирует все публичные
заголовки, кроме `sve/frame.h`, в окружении без заголовков и include paths FFmpeg. Попадание
типа или include FFmpeg в основной публичный API является ошибкой сборки.

## 8.4 Выбор backend

При `Backend::Auto` пользователь передаёт предпочтительный порядок backend через
`BackendSelection::preferred`; библиотека не использует hardcoded-последовательность:

`EncoderConfig` содержит `BackendSelection backend_selection`. Пользователь обязан заполнить
`preferred` при выборе `Backend::Auto`; пустой список возвращает `InvalidArgument`. В списке
разрешены только конкретные backend, поэтому `Backend::Auto` в нём возвращает
`InvalidArgument`; дубликаты также запрещены. Backend перебираются строго в переданном
порядке. Выбор, отказ и fallback фиксируются в telemetry. `allow_unlisted = false` запрещает
использование backend, отсутствующих в списке. Software fallback не добавляется неявно:
пользователь включает `Backend::X264` или `Backend::X265` в `preferred` в требуемой позиции.
Backend, не поддерживающий `EncoderConfig::codec`, пропускается при `Backend::Auto` с записью
причины в telemetry; при явном выборе такого backend создание завершается `Unsupported`.

Пример пользовательского порядка:

```cpp
sve::EncoderConfig config;
config.backend = sve::Backend::Auto;
config.backend_selection.preferred = {
    sve::Backend::NvidiaNvenc,
    sve::Backend::IntelOneVpl,
    sve::Backend::X265,
};
```

## 8.5 Владение кадром и FFmpeg interop

Основной API использует владеющий backend-neutral тип `Frame`:

```cpp
class Frame {
public:
    Frame(const Frame&) = default;
    Frame(Frame&&) noexcept = default;
    Frame& operator=(const Frame&) = default;
    Frame& operator=(Frame&&) noexcept = default;

    const IFrameBuffer& buffer() const noexcept;
    IFrameBuffer& mutable_buffer() noexcept;
};

struct TimedFrame {
    Frame frame;
    FrameMetadata metadata;
};
```

`SubmitFrame(TimedFrame)` передаёт encoder безопасное shared ownership underlying storage.
Конкретная реализация `IFrameBuffer` может представлять CPU planes, CUDA/device memory или
другой hardware surface.

Только `sve/frame.h` предоставляет FFmpeg adapter:

```cpp
class AvFrameAdapter {
public:
    static Frame Ref(const AVFrame* frame);   // выполняет av_frame_ref
    static Frame Adopt(AVFrame* frame);       // принимает ownership
};

Status SubmitAvFrame(
    SmartVideoEncoder& encoder,
    const AVFrame* frame,
    const FrameMetadata& metadata);
```

Пользователь, не использующий этот adapter, не обязан устанавливать headers или libraries
FFmpeg. Если кадр используется после возврата из interop-вызова, adapter обязан удерживать
корректный `av_frame_ref` до уничтожения внутреннего frame storage.

## 8.6 Владение encoded packet

Payload пакета использует shared immutable storage:

```cpp
class PacketBuffer {
public:
    explicit PacketBuffer(std::vector<uint8_t> data);
    const uint8_t* data() const noexcept;
    size_t size() const noexcept;
    bool empty() const noexcept;
};

struct EncodedPacket {
    PacketBuffer payload;
    int64_t pts = kNoPts;
    int64_t dts = kNoPts;
    int64_t duration = 0;
    bool key_frame = false;
    Codec codec = Codec::H265;
};
```

Копирование `PacketBuffer` и `EncodedPacket` является shallow copy и не копирует encoded bytes. Payload неизменяем после публикации.

## 8.7 Временные метки

Для API, где timestamp передаётся отдельно от `AVFrame`, используется тип:

```cpp
struct MediaTimestamp {
    int64_t value = kNoTimestamp;
    Rational time_base{1, 90000};
    bool valid() const noexcept;
};
```

Единицы timestamp всегда задаются `time_base`; не допускается неявное предположение о миллисекундах или микросекундах. При преобразовании в `AVFrame::pts` выполняется явный rescale.

## 8.8 Push API

Основной неблокирующий submit/receive API остаётся нормативным. Для pipeline-интеграций предоставляется push facade:

```cpp
enum class EncoderEventType { Packet, EndOfStream, Error };

struct EncoderEvent {
    EncoderEventType type = EncoderEventType::Packet;
    std::shared_ptr<const EncodedPacket> packet;
    Status status;
};

using EncoderCallback = std::function<void(EncoderEvent)>;

class PushVideoEncoder {
public:
    virtual Status SetInputFormat(const InputVideoFormat&) = 0;
    virtual Status Subscribe(EncoderCallback) = 0;
    virtual Status Process(TimedFrame) = 0;
    virtual Status Flush() = 0;
    virtual Status Stop() = 0;
};
```

Facade реализуется поверх `SmartVideoEncoder`, сохраняет backpressure и не содержит отдельной encoder logic.

## 8.9 EOS и ошибки push API

`nullptr` не используется как признак EOS. Callback всегда получает типизированный `EncoderEvent`:

- `Packet` — `packet != nullptr`, `status = Ok`;
- `EndOfStream` — packet отсутствует, `status = EndOfStream`;
- `Error` — packet отсутствует, status содержит код и диагностику.

После события `EndOfStream` новые packet events запрещены. После `Error` дальнейшее поведение определяется `EncoderState`; fatal error переводит encoder в `Failed`.

## 8.10 Raw planes API

Для интеграций без готового `AVFrame` предоставляется невладеющий view с явным owner:

```cpp
struct RawFramePlane {
    const uint8_t* data = nullptr;
    int32_t stride = 0;
    size_t size = 0;
};

struct RawFrameView {
    InputVideoFormat format;
    std::array<RawFramePlane, 4> planes{};
    size_t plane_count = 0;
    MediaTimestamp timestamp;
    std::shared_ptr<const void> owner;
};

Status WrapRawFrame(const RawFrameView& view, Frame* output);
```

`owner` обязан удерживать underlying memory до уничтожения `Frame`. Валидируются число planes, strides, размеры, pixel format и соответствие dimensions. При отсутствии гарантии lifetime вызывающая сторона обязана передать копию либо использовать owning `AVFrame`.

## 8.11 Codec parameters и extradata

Codec initialization data запрашиваются отдельно от packet stream:

```cpp
struct CodecParameters {
    Codec codec = Codec::H265;
    CodecProfile profile = CodecProfile::Auto;
    PacketBuffer extradata;
    Rational time_base{1, 90000};
    uint32_t width = 0;
    uint32_t height = 0;
};

Status GetCodecParameters(CodecParameters* parameters) const;
```

`extradata` содержит backend-specific codec configuration, например SPS/PPS/VPS в формате, документированном для выбранного output mode. Изменение extradata после reconfigure/keyframe публикуется как обновление codec parameters и отражается в telemetry.

## 8.12 Фабрики

Публичный factory API:

```cpp
std::unique_ptr<SmartVideoEncoder>
CreateVideoEncoder(const EncoderConfig& config);

std::unique_ptr<PushVideoEncoder>
CreatePushVideoEncoder(const EncoderConfig& config);
```

Свободные factory-функции являются единственным публичным способом создания encoder.
Конкретный backend и режим его выбора полностью определяются `EncoderConfig`: для
автоматического выбора пользователь задаёт `Backend::Auto` и заполняет
`backend_selection.preferred` согласно разделу 8.4. Отдельная factory-функция для
автоматического выбора не предоставляется.

Обе factory-функции используют одну реализацию валидации и выбора backend. Codec и backend
не кодируются в имени factory. При ошибке создания factory выбрасывает типизированное
исключение, содержащее `StatusCode` и диагностические поля из раздела 25. Возврат `nullptr`
как способ сообщить об ошибке запрещён.

## 8.13 Lifecycle

Состояние encoder доступно явно:

```cpp
enum class EncoderState {
    Created,
    Configured,
    Running,
    Draining,
    EndOfStream,
    Stopped,
    Failed,
};
```

Допустимые переходы:

```text
Created -> Configured -> Running -> Draining -> EndOfStream -> Stopped
                         |                         |
                         +---------> Failed <------+
```

`Stop()` идемпотентен и освобождает worker/backend resources. `Flush()` переводит encoder в `Draining`; `ReceivePacket` выдаёт остаток и затем `EndOfStream`. `SubmitFrame` в состояниях `Draining`, `EndOfStream`, `Stopped` и `Failed` возвращает `InvalidArgument` либо сохранённую fatal error.


---

# 9. Object metadata API

```cpp
enum class ObjectPriority : uint8_t {
    Background = 0,
    Low,
    Normal,
    High,
    Critical
};

struct BoundingBox {
    float x;
    float y;
    float width;
    float height;
};

struct Polygon {
    std::vector<PointF> points;
};

// Бинарная маска объекта, 1 байт на пиксель (row-major).
struct BinaryMask {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;              // байт на строку
    bool relative_to_bbox = true;     // false = координаты кадра
    std::vector<uint8_t> data;        // ownership у metadata
};

struct ObjectMetadata {
    // Уникален в пределах кадра; 0 — валидный ID (не sentinel).
    uint64_t object_id = 0;

    // nullopt = объект без track association; 0 — валидный track ID.
    std::optional<uint64_t> track_id;

    BoundingBox bbox;
    std::optional<Polygon> polygon;
    std::optional<BinaryMask> mask;

    float confidence = 1.0f;

    ObjectPriority priority = ObjectPriority::Normal;

    std::string class_name;

    std::optional<float> desired_qp_delta;
};

struct FrameMetadata {
    int64_t pts = kNoTimestamp;
    std::vector<ObjectMetadata> objects;
};
```

Правило PTS:

- все PTS интерпретируются в `EncoderConfig::input.time_base`;
- источник истины — timestamp в `Frame`/`IFrameBuffer`; `FrameMetadata.pts` служит для валидации соответствия metadata кадру;
- при расхождении timestamp кадра и `FrameMetadata.pts` вызов `SubmitFrame` возвращает `InvalidArgument`;
- `FrameMetadata.pts = kNoTimestamp` означает «использовать timestamp кадра без дополнительной проверки».

Правила геометрии:

- приоритет представлений при формировании ROI: `mask` > `polygon` > `bbox`; менее точные представления используются как fallback, если backend не поддерживает точное;
- `bbox` обязателен всегда, в том числе при наличии polygon/mask;
- `mask` в P0 растеризуется в ROI rectangles / QP map согласно capability backend; попиксельное применение маски — P1.

Правила `desired_qp_delta`:

- если задан, значение **переопределяет** default logical delta приоритета из раздела 11.1 (не суммируется);
- итоговое значение клампится в допустимый диапазон backend adapter;
- при overlap участвует в разрешении по правилам раздела 12 наравне с priority-based deltas;
- применённое (после клампа) значение фиксируется в telemetry как `applied_qp_delta`.

Лимиты входа (валидируются в `SubmitFrame`, превышение → `InvalidArgument`):

| Параметр | Лимит по умолчанию |
|---|---:|
| objects per frame | 256 |
| points per polygon | 1024 |
| mask size | не больше размера кадра |
| class_name length | 64 байта |

Лимиты конфигурируются через `MetadataConfig`, но не могут превышать hard limits, зафиксированные в документации API.

Координаты могут быть:
- pixel coordinates;
- normalized coordinates.

Единый формат задаётся `MetadataCoordinateSystem`.

---

# 10. ROI pipeline

Для каждого кадра:

```text
Input objects
     |
     v
Validation
     |
     v
Coordinate normalization
     |
     v
Clip to frame
     |
     v
Minimum ROI expansion
     |
     v
Priority resolution
     |
     v
Overlap merge / split
     |
     v
Temporal prediction
     |
     v
Spatial smoothing
     |
     v
Temporal smoothing
     |
     v
Backend quantization
     |
     v
ROI rectangles / QP map / CTU map
```

---

# 11. ROI policy

## 11.1 Object-first policy

P0 policy:

```text
Critical > High > Normal > Low > Background
```

Рекомендуемые logical QP offsets:

| Priority | Default logical delta |
|---|---:|
| Critical | -8 |
| High | -6 |
| Normal | -4 |
| Low | -2 |
| Background | +0 ... +6 |

Это **logical policy**, а не прямое требование к hardware QP.

Backend adapter обязан:
1. ограничить диапазон;
2. преобразовать знак/единицы, если требуется;
3. записать applied values в telemetry.

## 11.2 Minimum ROI size

```text
64 x 64 pixels
```

или больше, если backend требует alignment.

Малые объекты расширяются симметрично, но:
- не выходят за границы кадра;
- не перекрывают unrelated ROI без merge policy.

## 11.3 ROI margin

По умолчанию:

```text
8 pixels
```

до backend alignment.

Должны поддерживаться:
- fixed margin;
- relative margin;
- class-specific margin.

---

# 12. Overlapping objects

При пересечении ROI применяется:

```text
max importance wins
```

Для QP:

```text
minimum QP wins
```

При необходимости допускается weighted composition:

```text
priority score
+
confidence
+
object size
+
track age
```

Алгоритм выбора должен быть deterministic.

---

# 13. Temporal ROI

```cpp
struct TemporalROIConfig {
    bool enabled = true;

    uint32_t max_missing_frames = 3;

    float confidence_decay = 0.85f;

    float spatial_smoothing = 0.25f;
    float temporal_smoothing = 0.35f;

    bool use_track_prediction = true;

    float min_roi_tracking_iou = 0.50f;

    int max_qp_delta_per_frame = 2;
};
```

Целевые значения ROI lag (P95/P99) — acceptance-критерии (методика в разделе 14, пороги в разделе 49), а не параметры поведения; в `TemporalROIConfig` они не входят.

## 13.1 Поведение

Если объект временно пропал:

```text
detected
detected
MISS
MISS
detected
```

ROI сохраняется до `max_missing_frames`.

Если объект не возвращается:
- priority плавно уменьшается;
- QP correction плавно возвращается к background;
- запрещены резкие quality jumps сверх `max_qp_delta_per_frame`.

---

# 14. ROI lag

Для GT object определяется:

```text
t_gt = момент появления или существенного изменения объекта
```

Первый момент:

```text
t_roi
```

при котором:

```text
IoU(applied ROI, target ROI) >= 0.50
```

Тогда:

```text
lag_frames = frame(t_roi) - frame(t_gt)
lag_ms = PTS(t_roi) - PTS(t_gt)
```

Здесь определена только методика измерения; нормативные acceptance-пороги ROI lag заданы в разделе 49.

---

# 15. Bitrate policy

Основной режим:

```text
Object-first VBR
```

Цель:

> При ограниченном bitrate сначала сохранить task quality объектов, затем качество фона.

## 15.1 Primary benchmark profile

```yaml
pixel_format: yuv420p
bit_depth: 8

intra_period_seconds: 2        # gop_size = 2 * fps: 60 @30fps, 120 @60fps
keyint_min: equal_to_gop_size
scene_cut: disabled

b_frames: 0

rate_control: VBR

lookahead: disabled
spatial_aq: disabled
temporal_aq: disabled
```

Intra-период нормируется в секундах, чтобы для 1080p60 и 30fps operating points время между keyframes совпадало: `gop_size = intra_period_seconds × fps`.

В baseline и adaptive должны отличаться только механизмы adaptive spatial allocation.

## 15.2 Secondary random-access profile

```yaml
intra_period_seconds: 2
b_frames: 3
lookahead: 16
```

Значения должны быть идентичны baseline/adaptive.

## 15.3 10-bit profile

Обязательный профиль для backend'ов с capability `main10`:

```yaml
pixel_format: p010
bit_depth: 10
profile: h265_main10

intra_period_seconds: 2
b_frames: 0
rate_control: VBR

lookahead: disabled
spatial_aq: disabled
temporal_aq: disabled
```

Прогоняется минимум на functional codec tests (раздел 32, AT-009) и на одном operating point (R2) synthetic ROI suite.

---

# 16. Bitrate acceptance

Для matched-bitrate comparison:

```text
|adaptive_avg_bitrate - baseline_avg_bitrate|
--------------------------------------------- <= 2%
baseline_avg_bitrate
```

Дополнительно:

```text
average bitrate overshoot <= 5%
```

Rolling window:

```text
window = 1 second
P95 rolling bitrate <= 120% target
```

Правила согласованности:

- при `RateControl::VBR` конфигурация обязана удовлетворять `max_bitrate >= target_bitrate`; пара `max_bitrate`/`vbv_buffer_size`, при которой rolling-порог «P95 <= 120% target» недостижим или противоречив, приводит к исключению со статусом `InvalidArgument` из `CreateVideoEncoder`/`CreatePushVideoEncoder`;
- P95 rolling-статистика валидна только для последовательностей длительностью >= 30 секунд; для более коротких rolling-критерий помечается `NOT_APPLICABLE` в отчёте и не участвует в вердикте.

Нормативные пороги bitrate acceptance — в разделе 49.

---

# 17. Входные форматы

P0:

```text
PixelFormat::Yuv420P
PixelFormat::Nv12
PixelFormat::P010
```

Дополнительно допускается:
- RGB/BGR input через conversion path;
- CUDA hardware frames;
- другие hardware surfaces через backend-neutral `IFrameBuffer`;
- FFmpeg hardware frames через opt-in `sve/frame.h` adapter, если их можно безопасно сопоставить с выбранным native backend.

При hardware input библиотека должна предпочитать zero-copy или hardware mapping. Необязательное скачивание GPU frame в RAM запрещено, если backend способен принять совместимый hardware frame.

---

# 18. Владение кадрами и AVFrame interop

Основной `Frame` безопасно удерживает storage через backend-neutral ownership. Для FFmpeg
interop библиотека не изменяет ownership caller `AVFrame`, выполняет `av_frame_ref`, если
кадр должен жить асинхронно, освобождает все внутренние references и не сохраняет raw pointer
после завершения операции без удерживаемой ссылки.

---

# 19. Metadata

## 19.1 Внутренний metadata channel

Каждый encoded packet может сопровождаться:

```cpp
struct EncodedFrameMetadata {
    int64_t input_pts;
    int64_t output_pts;

    std::vector<ObjectMetadata> objects;

    AppliedROIInfo roi_info;

    uint64_t frame_index;
};
```

## 19.2 Bitstream metadata

P0 должна поддерживаться возможность backend-specific insertion:
- H.264 SEI user data unregistered;
- H.265 SEI user data unregistered;
- external sidecar metadata.

Формат payload:

```text
version        (uint8, текущая = 1)
frame_id       (uint64)
pts            (int64, в EncoderConfig::input.time_base)
object_count   (uint16)
objects[]      (сериализованные ObjectMetadata без mask data)
crc            (CRC-32C / Castagnoli, полином 0x1EDC6F41,
                вычисляется по всем предыдущим полям payload)
```

P0 serialization:

- **CBOR (RFC 8949)** — единственный нормативный формат P0; FlatBuffers допускается только как дополнительный opt-in формат P1 без изменения публичного API;
- versioned schema: несовместимые изменения увеличивают `version`; parser обязан отклонять неизвестную мажорную версию;
- bounded payload size: суммарный SEI payload на кадр не превышает 16 KiB; при превышении объекты усекаются в порядке возрастания priority (сначала теряются наименее важные), факт усечения фиксируется в telemetry;
- binary mask data в SEI payload не включается; в сериализованном объекте сохраняется только признак наличия маски.

## 19.3 Sidecar metadata

Если SEI недоступен или отключён, используется sidecar-канал:

- формат — NDJSON (одна JSON-строка на кадр) либо CBOR sequence; выбор фиксируется в `MetadataConfig` и в манифесте результата;
- каждая запись содержит те же поля, что и SEI payload (19.2), плюс `frame_index`;
- naming: `<output_basename>.svemeta.ndjson` рядом с выходным потоком, либо пользовательский sink (`std::ostream`/callback), предоставленный приложением;
- запись синхронизирована с выдачей `EncodedPacket`: запись N соответствует N-му выданному пакету и дополнительно верифицируется по PTS;
- поскольку muxing вне scope библиотеки, привязку sidecar к контейнеру выполняет приложение; библиотека гарантирует только соответствие sidecar-записей выданным пакетам.

## 19.4 Синхронизация

Ключ синхронизации:
1. PTS;
2. frame index;
3. optional UUID.

Acceptance:

```text
100% metadata-to-frame synchronization
```

на functional metadata corpus.

---

# 20. FFmpeg только как opt-in interop

FFmpeg не является encoder backend или общей прослойкой. Его использование ограничено
opt-in `sve/frame.h` adapter для интеграции с пользовательскими `AVFrame`.

Core и все encoder backend modules не должны иметь link-time или runtime dependency на
FFmpeg. `EncodedPacket` владеет собственным immutable byte buffer и не экспортирует
`AVPacket`. Кодирование software H.264/H.265 выполняют direct `X264Backend`/`X265Backend`.

FFmpeg interop adapter может ссылаться на FFmpeg и преобразовывать `AVFrame` в canonical
`Frame`/`IFrameBuffer`. После преобразования adaptive policy и native backend interface
работают только с собственными типами библиотеки.

---

# 21. Backend interface

```cpp
class IEncoderBackend {
public:
    virtual ~IEncoderBackend() = default;

    // Создаёт независимый сеанс кодирования. Состояние потока хранится в сеансе,
    // а не в объекте backend.
    virtual Result<SessionPtr> Configure(
        const EncoderConfig& config) const = 0;

    virtual EncoderCapabilities QueryCapabilities() const = 0;

    // Принимает кадр с применённой adaptive-конфигурацией.
    // Возвращает Again при заполненной внутренней очереди backend.
    virtual Status SubmitFrame(
        BackendSession& session,
        IFrameBuffer& frame,
        const BackendFrameControl& control) const = 0;

    // Возвращает готовый пакет либо Again, если пакета нет.
    // Соответствие кадр -> пакет: 1 -> 0..N (B-frames, lookahead, reordering).
    virtual Result<EncodedPacket> ReceivePacket(
        BackendSession& session) const = 0;

    virtual Status Flush(BackendSession& session) const = 0;

    virtual BackendStatistics Statistics(
        const BackendSession& session) const = 0;
};
```

`IEncoderBackend` описывает доступ к реализации backend и может совместно использоваться
несколькими потоками кодирования. Вызов `Configure` не переводит сам объект backend в
состояние конкретного кодирования, а создаёт отдельный `BackendSession`, владеющий codec,
device и queue state соответствующего потока. Один backend обязан поддерживать несколько
одновременно существующих сеансов в пределах возможностей устройства без глобальной
сериализации независимых потоков.

`SessionPtr` — владеющий smart pointer на `BackendSession`. Уничтожение последней ссылки
закрывает сеанс и освобождает связанные с ним ресурсы; оно не влияет на другие сеансы того же
backend. `SessionPtr` имеет unique ownership; вызовы методов принимают ссылку на сеанс, а не
копируют ownership. Сеанс нельзя передавать в экземпляр backend другого типа или использовать
после его закрытия. Такая ошибка возвращает `InvalidArgument`.

`Result<T>` содержит либо значение `T`, либо `Status`; одновременное наличие значения и
ошибочного статуса запрещено. Для `ReceivePacket` отсутствие готового пакета представляется
статусом `Again`, а завершённый draining — статусом `EndOfStream` без значения.

Методы backend являются `const`, поскольку изменяют только переданный сеанс либо внешние
device resources. Это не означает автоматическую thread-safety одного сеанса: для каждого
`BackendSession` действует модель single-producer / single-consumer из раздела 24. Разные
сеансы разрешено обслуживать конкурентно. Общие кэши, device handles и другие ресурсы внутри
backend должны быть синхронизированы без удержания глобальной блокировки на протяжении
операции кодирования.

`IFrameBuffer` является внутренней backend-neutral абстракцией кадра. FFmpeg interop adapter
может предоставлять её реализацию поверх `AVFrame`, но конкретный тип FFmpeg не является
обязательной частью backend interface. `BackendFrameControl` содержит применённые ROI/QP
controls, keyframe request и frame quality budget.

`BackendFrameControl` является внутренним canonical representation:

```cpp
struct BackendFrameControl {
    std::vector<ResolvedROI> rois;

    std::optional<QPMap> qp_map;

    bool force_intra = false;

    FrameQualityBudget budget;
};
```

Требования к backend adapter:

- пакеты выдаются в decode order с корректными PTS/DTS в `EncoderConfig::input.time_base`;
- при `max_b_frames > 0` adapter обязан сопоставлять metadata входного кадра выходному пакету по PTS, а не по порядку вызовов;
- `Flush(session)` переводит только переданный сеанс в draining и не влияет на другие сеансы;
- после `Flush(session)` вызов `ReceivePacket(session)` возвращает оставшиеся пакеты, затем `EndOfStream`;
- ошибка, EOS, flush или уничтожение одного сеанса не должны менять состояние других сеансов;
- capability и ограничения, общие для устройства, возвращаются backend, а фактически выбранные режимы и статистика кодирования относятся к конкретному сеансу.

---

# 22. Backend fallback matrix

| Requested feature | Backend supports | Поведение |
|---|---|---|
| ROI rectangles | yes | native ROI |
| ROI rectangles | no, QP map yes | synthesize map |
| QP map | no, ROI yes | quantize to ROI |
| Neither | no | frame-level fallback или `Unsupported` |
| CTU control | yes | native CTU map |
| Metadata SEI | no | sidecar/external metadata |

Fallback должен быть:
- deterministic;
- logged;
- доступен через statistics;
- тестируем.

Выбор между «frame-level fallback» и `Unsupported` управляется полем `AdaptiveConfig::fallback_policy` (см. Приложение C):

- `Strict` — запрошенная, но недоступная возможность возвращает `Unsupported`;
- `Permissive` (default) — применяется documented fallback с обязательной записью в telemetry.

---

# 23. Capability report

CLI/API:

```text
sve-capabilities [--json]
```

`sve-capabilities` — CLI-утилита из `tools/`, собирается при `SVE_BUILD_TOOLS=ON` и линкуется с публичной библиотекой `sve`.

Пример:

```json
{
  "backend": "nvidia-nvenc",
  "device": "...",
  "codecs": ["h264", "h265"],
  "roi_rectangles": true,
  "qp_map": false,
  "emphasis_map": true,
  "max_roi_regions": 8
}
```

---

# 24. Threading model

P0:

```text
Application thread
        |
        v
Input queue
        |
        v
Adaptive worker
        |
        v
Encoder backend
        |
        v
Output queue
```

Требования:
- bounded queues;
- configurable queue depth;
- backpressure;
- flush correctness;
- EOS correctness;
- thread-safe statistics.

Контракт thread-safety публичного API:

- модель P0 — single-producer / single-consumer: `SubmitFrame` вызывается из одного потока, `ReceivePacket` — из одного (возможно, другого) потока; такая пара безопасна без внешней синхронизации;
- конкурентные вызовы одного и того же метода из нескольких потоков не поддерживаются;
- `GetCapabilities` и `GetStatistics` безопасны из любого потока;
- backpressure выражается кодом `Again` (раздел 8.1), а не блокировкой; опциональные блокирующие варианты с timeout допускаются как отдельные методы P1 и используют `StatusCode::Timeout`.

---

# 25. Error model

Использовать:

```cpp
enum class StatusCode {
    Ok,
    Again,
    InvalidArgument,
    Unsupported,
    DeviceUnavailable,
    BackendError,
    OutOfMemory,
    Timeout,
    EndOfStream
};
```

Ошибки backend должны содержать:
- backend;
- codec;
- native error code;
- human-readable message.

---

# 26. CMake и vcpkg

## 26.1 Требования

Сборка должна использовать:
- CMake;
- vcpkg manifest mode;
- C++20.

Структура:

```text
smart-video-encoder/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── vcpkg-configuration.json
├── cmake/
├── include/
│   └── sve/
├── src/
│   ├── core/
│   ├── roi/
│   ├── temporal/
│   ├── backends/
│   │   ├── nvenc/
│   │   ├── onevpl/
│   │   ├── rockchip/
│   │   ├── x264/
│   │   └── x265/
│   ├── interop/
│   │   └── ffmpeg/
│   └── metadata/
├── tests/
├── benchmarks/
├── python/
│   ├── src/
│   ├── tests/
│   └── smart_video_encoder/
├── tools/
├── docker/
├── docs/
└── .gitlab-ci.yml
```

## 26.2 Options

```cmake
option(SVE_BUILD_BACKEND_X264 "Build direct libx264 backend module" ON)
option(SVE_BUILD_BACKEND_X265 "Build direct libx265 backend module" ON)
option(SVE_BUILD_FFMPEG_INTEROP "Build opt-in AVFrame interop component" ON)
option(SVE_BUILD_BACKEND_NVENC "Build native NVIDIA NVENC backend module" ON)
option(SVE_BUILD_BACKEND_ONEVPL "Build native Intel oneVPL backend module" ON)
option(SVE_BUILD_BACKEND_ROCKCHIP "Build native Rockchip MPP backend module" OFF)

option(SVE_BUILD_TESTS "Build tests" ON)
option(SVE_BUILD_BENCHMARKS "Build benchmarks" ON)
option(SVE_BUILD_TOOLS "Build CLI tools" ON)
option(SVE_BUILD_PYTHON "Build Python bindings" OFF)
```

Backend dependency должна быть optional, чтобы CI x86 без hardware мог собрать core и software backend.

## 26.3 Экспорт, ABI и версионирование

- установка экспортирует CMake package config: `find_package(sve CONFIG)`, target `sve::sve`;
- поставляется pkg-config файл `sve.pc`;
- core shared library имеет SONAME `libsve.so.<major>`; backend modules имеют собственные
  module ABI versions и не экспортируются как пользовательские link targets;
- публичный C++ API версионируется по semver: major — breaking changes, minor — обратно совместимые дополнения, patch — исправления;
- C++ и Python версии происходят из единого version source (см. 58.7).

## 26.4 Модульная сборка и runtime plugin ABI

Каждый backend собирается как независимый optional shared module:

```text
libsve.so.<major>
libsve_backend_x264.so
libsve_backend_x265.so
libsve_backend_nvenc.so
libsve_backend_onevpl.so
libsve_backend_rockchip.so
```

При отключённой CMake option соответствующий module не собирается, его SDK не требуется, а
registry не объявляет backend доступным. Удаление optional module из установленной системы не
должно приводить к падению core: backend получает состояние `DeviceUnavailable`/unavailable и
может участвовать в telemetry выбора.

Каждый module экспортирует ровно одну обязательную C ABI entry point:

```cpp
extern "C" SVE_BACKEND_EXPORT
const SveBackendPluginDescriptorV1* sve_backend_plugin_v1() noexcept;
```

`SveBackendPluginDescriptorV1` содержит как минимум:

- `abi_version` и `descriptor_size`;
- стабильный backend identifier и module version;
- factory/destroy callbacks;
- capability query callback;
- allocator/deallocator callbacks либо правило, что объект освобождается в создавшем module;
- optional diagnostic callback.

Исключения не должны пересекать C ABI boundary; module преобразует их в `Status`. Core до
вызова callbacks проверяет ABI version и размер descriptor. Несовместимый, повреждённый или
дублирующий module не загружается и фиксируется в telemetry. Module не выгружается, пока жив
хотя бы один созданный им backend или `BackendSession`.

Порядок поиска modules детерминирован: explicit application path, install-relative directory,
затем system-configured directory. Поиск в current working directory по умолчанию запрещён.
Разрешены только canonical paths; production profile может требовать allowlist/hash/signature.

Публичный CMake target остаётся `sve::sve`; имена backend `.so` являются packaging detail.
Core и основной `sve.pc` не объявляют FFmpeg, x264 или x265 обязательными/private dependency.
Requirements публикуются только component metadata соответствующего module/interop target.

Шаблон основного `sve.pc` обязан иметь пустое `Requires.private` относительно FFmpeg, x264 и
x265. Отдельные component metadata для `X264Backend` и `X265Backend` могут объявлять только
соответственно libx264 и libx265. FFmpeg interop component может объявлять `libavutil`, но не
`libavcodec`/`libavformat`, если они не требуются реализацией adapter. CI проверяет
сгенерированный `sve.pc`, чтобы codec и FFmpeg dependencies не стали транзитивными
зависимостями core.

## 26.5 Rockchip MPP и vcpkg

MPP отсутствует в vcpkg registry. Стратегия P0 (критический путь Milestone 4):

- x86 CI: `SVE_BUILD_BACKEND_ROCKCHIP=OFF`, backend недоступен и не блокирует сборку;
- aarch64: сборка против BSP sysroot либо system libraries target board; путь задаётся через CMake toolchain/`MPP_ROOT`;
- допускается собственный vcpkg overlay port `rockchip-mpp`, version-pinned на конкретный commit MPP;
- выбранная стратегия и версия MPP фиксируются в build manifest.

---

# 27. Docker

## 27.1 Базовые образы

Минимум:

```text
docker/Dockerfile.dev
docker/Dockerfile.runtime
docker/Dockerfile.test
docker/Dockerfile.nvidia
docker/Dockerfile.intel
docker/Dockerfile.rockchip
```

## 27.2 NVIDIA runtime

Контейнер использует NVIDIA Container Toolkit/runtime.

Требования:
- GPU должен быть доступен контейнеру;
- driver остаётся на host;
- userspace libraries должны быть совместимы с host driver/runtime.

Проверки:
- `nvidia-smi`;
- native NVENCODE API capability discovery;
- native H.264/H.265 encode/decode smoke test;

## 27.3 Intel

Для Linux hardware acceleration контейнер должен получать соответствующие DRM/render devices, обычно через `/dev/dri`, в зависимости от используемого stack и deployment.

Проверки:
- device enumeration;
- oneVPL implementation discovery;
- H.264/H.265 encode smoke test.

## 27.4 Rockchip

На target board контейнер должен получать необходимые device nodes и userspace MPP libraries согласно конкретной BSP/SoC configuration.

P0 не должен хардкодить один device path в portable API. Конкретный deployment profile документируется отдельно.

---

# 28. Observability

Каждый frame может генерировать:

```text
frame_index
pts

object_count
roi_count

logical_qp_delta
applied_qp_delta

backend_mode
fallback_mode

input_timestamp
adaptive_timestamp
encode_start
encode_end

packet_size
instantaneous_bitrate
```

Агрегированные метрики:
- FPS;
- average latency;
- P50/P95/P99 encode latency;
- bitrate;
- average ROI count;
- fallback count;
- dropped frames;
- queue depth.

---

# 29. Логирование

Использовать structured logging.

Уровни:
- ERROR;
- WARN;
- INFO;
- DEBUG;
- TRACE.

Обязательные события:
- backend selected;
- capability report;
- fallback;
- unsupported feature;
- encoder reset/reconfigure;
- frame drop;
- metadata failure.

---

# 30. Тестовые уровни

```text
L0  Static analysis / formatting (C++: clang-format, clang-tidy; Python: ruff, black --check, mypy)
L1  Unit (C++ core и Python bindings)
L2  Integration
L3  Functional codec (C++ и Python, включая P010/Main10 при capability main10)
L4  Hardware smoke
L5  Synthetic adaptive quality
L6  Natural video quality
L7  CV/task-aware
L8  Performance/stability
L9  Acceptance (включая Python wrapper acceptance, 58.10)
```

---

# 31. Unit tests

Покрыть:

## ROI
- clipping;
- normalization;
- alignment;
- minimum size;
- overlap;
- priority resolution;
- deterministic ordering.

## Temporal
- detector miss;
- object reappearance;
- timeout;
- prediction;
- smoothing;
- ROI lag calculation.

## Bitrate
- bpppf;
- rolling bitrate;
- overshoot;
- matching.

## Metadata
- serialization;
- deserialization;
- schema version;
- CRC-32C corruption detection;
- PTS mapping;
- payload size limits и truncation по priority;
- sidecar/packet соответствие;
- input limits (objects per frame, polygon points, mask size).

Минимум:
- 100% unit tests pass;
- новые production modules не принимаются без unit tests для основной логики.

---

# 32. Functional codec tests

Для каждого доступного backend:

| Test | H.264 | H.265 |
|---|---|---|
| Open/close | required | required |
| Encode 1 frame | required | required |
| Encode 300 frames | required | required |
| Flush | required | required |
| EOS | required | required |
| Monotonic PTS | required | required |
| Frame count | required | required |
| Decode output | required | required |
| Metadata | required | required |

Acceptance:
- frame count decoded = frame count input;
- PTS mapping корректен;
- битстрим декодируется reference decoder;
- memory leaks отсутствуют в software/CPU CI job.

Для backend'ов с capability `main10` таблица дополнительно выполняется для P010/Main10 (H.265): open/close, encode 300 frames, flush, EOS, monotonic PTS, decode output. Отсутствие capability — documented SKIP (правила раздела 50).

---

# 33. Synthetic benchmark corpus

Synthetic corpus обязателен и version-controlled.

Структура:

```text
benchmarks/synthetic/
├── flat/
├── gradients/
├── checkerboards/
├── sine/
├── zoneplates/
├── slanted_edges/
├── moving_roi/
├── appearing_roi/
├── disappearing_roi/
├── crossing_objects/
├── small_objects/
├── temporal_flicker/
└── mixed_frequency/
```

## 33.1 Spatial frequency patterns

Обязательны:
- uniform fields;
- horizontal/vertical gradients;
- checkerboard;
- sinusoidal gratings;
- multi-frequency sine patterns;
- zone plate;
- slanted edge;
- text/grid pattern.

Цель:
- проверить распределение ошибок по spatial frequency;
- обнаружить block artifacts;
- проверить edge preservation;
- проверить leakage качества за пределы ROI.

## 33.2 Synthetic ROI patterns

Обязательные сценарии:
1. статический ROI;
2. ROI moving horizontally;
3. ROI moving diagonally;
4. ускоряющийся ROI;
5. появление;
6. исчезновение;
7. crossing ROI;
8. partial overlap;
9. маленький объект;
10. много ROI.

---

# 34. ROI quality metrics

## 34.1 Выбор метрик по типу контента

VMAF обучен на natural content; на synthetic patterns (zone plates, checkerboards, sine gratings) его показания малоинформативны. Поэтому primary-метрика зависит от corpus:

| Corpus | Primary | Secondary |
|---|---|---|
| Synthetic (раздел 33) | ROI-PSNR-Y, ROI-SSIM | ROI-MS-SSIM, Background-PSNR-Y, Global-PSNR-Y |
| Natural video (раздел 36) | ROI-VMAF | ROI-PSNR-Y, ROI-SSIM, ROI-MS-SSIM, Background-VMAF, Global-VMAF |

## 34.2 Методика вычисления ROI-метрик

Единая нормативная процедура для всех ROI-* метрик:

1. ROI mask расширяется на 8 pixels (dilate) с clipping к границам кадра.
2. Из расширенной маски строится axis-aligned bounding rect, выровненный к чётным координатам (chroma alignment).
3. Если rect меньше 192 × 108, он симметрично расширяется до этого минимума с clipping; если после clipping размер всё ещё меньше минимума, ROI-VMAF для региона не вычисляется — используются только ROI-PSNR-Y/ROI-SSIM.
4. Идентичный rect применяется к reference и distorted кадрам: crop без scale, без letterbox, без mask-fill.
5. Метрика вычисляется на последовательности crop'ов; агрегация по кадрам — arithmetic mean.
6. Инструменты пиннингуются в манифесте: libvmaf (точная версия и модель, например `vmaf_v0.6.1`); PSNR/SSIM/MS-SSIM — из того же metrics tool той же версии.

Изменение любой ступени процедуры — новая `metrics_tool_version`; результаты разных версий несравнимы.

Для baseline operating points (natural video):

```text
ROI-VMAF in [60, 95]
```

## 34.3 Acceptance

Synthetic corpus:

```text
mean   Delta ROI-PSNR-Y >= +0.5 dB
median Delta ROI-PSNR-Y >= +0.25 dB
```

Не более 10% sequence × bitrate-point комбинаций могут иметь `ROI-PSNR-Y loss > 0.5 dB`.

Natural video:

```text
mean   Delta ROI-VMAF >= +2.0
median Delta ROI-VMAF >= +1.0
```

Не более 10% sequence × bitrate-point комбинаций могут иметь `ROI-VMAF loss > 1.0`.

Нормативные значения — в разделе 49.

---

# 35. ROI effectiveness test

Synthetic sequence:

```text
low-detail background
+
high-frequency ROI
```

При одинаковом target bitrate:

Если доступен actual QP:

```text
mean QP_ROI <= mean QP_background - 2
```

Если actual QP недоступен:

```text
ROI-PSNR-Y - Background-PSNR-Y >= +2 dB
```

на специально подготовленном synthetic ROI pattern (методика — 34.2).

---

# 36. Natural video datasets

## 36.1 MOT17

Используется для:
- multi-object tracking;
- HOTA;
- IDF1;
- DetA;
- AssA;
- MOTA;
- ID switches.

Pipeline:
1. исходное видео;
2. baseline encode/decode;
3. adaptive encode/decode;
4. одинаковый detector/tracker;
5. MOT evaluation;
6. paired comparison.

Detector и tracker пиннингуются так же строго, как ALPR (36.3) — versioned bundle:

```text
benchmarks/mot/
├── detector/
│   ├── manifest.yaml
│   └── model.onnx
├── tracker/
│   ├── manifest.yaml
│   └── params.yaml
└── pipeline/config.yaml
```

Каждый manifest содержит `name`, `version`, `git_commit`, `weights_sha256`, `container_digest`. Без совпадения hashes с manifest paired comparison невалиден (правило 50.10).

### Acceptance

```text
Delta HOTA >= -0.5 pp
Delta IDF1 >= -0.5 pp
```

При matched bitrate.

## 36.2 Critical tracks

Subset:

```text
track length >= 30 frames
median bbox area <= 5% frame area
```

Primary:
- Critical-IDF1;
- Critical-DetRe.

Acceptance:

```text
Delta Critical-IDF1 >= -0.25 pp
Delta Critical-DetRe >= -0.25 pp
```

На R1/R2 хотя бы одна метрика:

```text
Delta Critical-IDF1 >= +0.5 pp
```

или:

```text
Delta Critical-DetRe >= +0.5 pp
```

## 36.3 UFPR-ALPR

Используется для:
- plate detection;
- OCR;
- small/high-frequency ROI;
- ALPR task quality.

Versioned benchmark bundle:

```text
benchmarks/alpr/
├── detector/
│   ├── manifest.yaml
│   └── model.onnx
├── ocr/
│   ├── manifest.yaml
│   └── model.onnx
└── pipeline/config.yaml
```

Каждая модель:

```yaml
name:
version:
git_commit:
weights_sha256:
container_digest:
```

Primary:
- Exact Plate Match Rate.

Secondary:
- Character Accuracy;
- CER;
- Plate Detection Recall.

Acceptance:

```text
Delta ExactPlateMatch >= -0.25 pp
Delta PlateDetectionRecall >= -0.25 pp
```

На R1/R2 хотя бы одна:

```text
mean Delta ExactPlateMatch >= +0.5 pp
```

или:

```text
mean Delta PlateDetectionRecall >= +0.5 pp
```

---

# 37. Дополнительные datasets

P1 рекомендуется добавить:
- BDD100K — detection;
- Cityscapes / KITTI — object-centric urban scenes;
- UVG / Xiph / standard video sequences — natural video RD quality;
- собственный surveillance corpus — при наличии прав на использование.

Лицензии, версии и split должны фиксироваться в dataset manifest.

---

# 38. Baseline calibration

Перед финальным acceptance выполняется calibration run.

Для каждого:

```text
backend
codec
hardware class
resolution
dataset
R1..R4
```

создаётся baseline manifest.

Пример:

```yaml
protocol: SVENC-1.0

backend: nvenc
codec: h265

encoder:
  version: exact-runtime-version
  preset: p4
  rate_control: vbr
  target_bitrate: 6000000
  max_bitrate: 6300000
  gop_size: 60
  b_frames: 0
  lookahead: 0
  spatial_aq: false
  temporal_aq: false

input:
  width: 1920
  height: 1080
  fps: 30
```

Изменение любого поля создаёт новый profile version.

---

# 39. Operating points

| Resolution | R1 | R2 | R3 | R4 |
|---|---:|---:|---:|---:|
| 720p30 | 0.75 Mbps | 1.5 Mbps | 3 Mbps | 6 Mbps |
| 1080p30 | 1.5 Mbps | 3 Mbps | 6 Mbps | 12 Mbps |
| 1080p60 | 3 Mbps | 6 Mbps | 12 Mbps | 24 Mbps |
| 4K30 | 6 Mbps | 12 Mbps | 24 Mbps | 48 Mbps |

Дополнительно рассчитывать:

```text
bpppf = bitrate / (width * height * fps)
```

---

# 40. RD and Task-RD analysis

Обязательные curves:

```text
Bitrate vs Global-VMAF
Bitrate vs ROI-VMAF
Bitrate vs HOTA
Bitrate vs IDF1
Bitrate vs Critical-IDF1
Bitrate vs ExactPlateMatch
```

Для четырёх operating points.

Дополнительно:
- BD-Rate;
- BD-quality.

Нельзя принимать решение только по одному bitrate point.

---

# 41. Statistical protocol

Для каждого результата:

```text
mean
median
stddev
P5
P95
95% confidence interval
```

Для baseline/adaptive paired comparison:
- paired bootstrap CI.

Число прогонов и детерминизм:

- `X264Backend`/`X265Backend`: детерминированные настройки (фиксированный seed, `threads=1` либо документированные deterministic flags); один прогон;
- hardware backend: >= 3 прогонов на конфигурацию; в отчёте mean ± 95% CI;
- все seeds фиксируются в манифесте.

Уровень агрегации acceptance:

- метрики вычисляются per `sequence × bitrate-point`;
- агрегаты mean/median из 34.3 берутся по всем sequence в рамках `backend × codec × operating point`;
- вердикт PASS/FAIL выносится отдельно для каждой комбинации `backend × codec`.

Coverage gate: line coverage core-модулей (`src/core`, `src/roi`, `src/temporal`, `src/metadata`) >= 80%; порог проверяется в CI.

Каждый report обязан содержать:

```text
benchmark_id
protocol_version
git_commit
build_id

backend
encoder_version
driver_version
firmware_version
hardware_id

dataset_version

detector_model_hash
ocr_model_hash

metrics_tool_version
```

---

# 42. Performance tests

Измерять:

```text
encode FPS
end-to-end FPS

P50/P95/P99 encode latency
P50/P95/P99 end-to-end latency

CPU usage
GPU utilization where available

GPU memory
RAM

queue depth
packet rate
```

## Acceptance

Для каждого certified hardware profile задаётся отдельный performance manifest.

Нельзя использовать единый FPS threshold для NVIDIA, Intel и Rockchip.

Пример:

```yaml
profile: nvenc-a5000-h265-1080p30
minimum_realtime_factor: 1.0
target_realtime_factor: 2.0
```

---

# 43. Stability tests

## 43.1 Soak

Минимум:
- 1 час continuous encode в CI smoke profile;
- 8 часов nightly hardware profile;
- 24 часа release-candidate soak на reference hardware.

Проверки:
- memory growth;
- frame loss;
- PTS;
- deadlock;
- backend reset;
- metadata synchronization.

## 43.2 Acceptance

```text
unexpected crash = 0
deadlock = 0
frame accounting mismatch = 0
metadata mismatch = 0
unbounded memory growth = 0
```

---

# 44. GitLab CI

Pipeline:

```text
lint (C++ + Python)
  |
build ────────────► python-build (wheels: x86_64, aarch64)
  |                       |
unit                 python-test (CPython matrix, X264/X265)
  |                       |
integration          python-wheel-install-test
  |                       |
software-codec            |
  |                       |
synthetic                 |
  |                       |
dataset-quality ◄─────────┘
  |
  ├── hardware-nvidia   ┐
  ├── hardware-intel    ├── параллельные независимые стадии
  └── hardware-rockchip ┘
  |
benchmark-report
  |
acceptance
```

Стадии `hardware-nvidia`, `hardware-intel`, `hardware-rockchip` независимы и выполняются параллельно; недоступность одного runner блокирует только свою стадию, а не pipeline целиком.

## 44.1 Jobs

### lint
- clang-format;
- clang-tidy;
- cmake format;
- ruff, black --check, mypy для `python/`.

### build
Matrix:
- Debug;
- Release;
- GCC;
- Clang.

Дополнительно выполняется compile-test публичных заголовков без FFmpeg: все установочные
заголовки, кроме `sve/frame.h`, компилируются без FFmpeg include paths. Этот тест обязателен
и предотвращает попадание типов или include-директив FFmpeg в основной публичный API.

Build matrix дополнительно проверяет независимые профили:

- core без backend modules и без FFmpeg;
- core + `X264Backend` без FFmpeg и vendor hardware SDK;
- core + `X265Backend` без FFmpeg и vendor hardware SDK;
- core + FFmpeg interop без encoder backend modules;
- core + `NvencBackend` без FFmpeg;
- core + `OneVplBackend` без FFmpeg;
- core + `RockchipMppBackend` без FFmpeg;
- удаление optional module после установки без падения core.

Для encoder modules выполняется dependency scan (`readelf`/`ldd`): зависимости от
`libavcodec`, `libavformat` и других FFmpeg libraries запрещены. `X264Backend` может зависеть
только от разрешённых libx264/system libraries, `X265Backend` — от libx265/system libraries.
FFmpeg interop component не должен зависеть от libx264/libx265 или vendor hardware runtimes.
Отдельно тестируются plugin ABI version mismatch, duplicate registration и безопасный отказ
загрузки повреждённого module.

### unit
- CTest;
- JUnit XML.

### integration
- `X264Backend` и `X265Backend`;
- mocked capability layer;
- одновременная работа нескольких `BackendSession` одного backend без взаимного влияния;
- независимые submit/backpressure/flush/EOS/error paths для конкурентных сеансов;
- освобождение одного сеанса без остановки и повреждения остальных.

### python-build / python-test / python-wheel-install-test
- сборка wheels для Linux x86_64 и aarch64 (runner — раздел 47) по CPython matrix из 58.9;
- pytest suite 58.9 на `X264Backend`/`X265Backend` (Debug/Release extension);
- установка wheel в чистый virtual environment и `import smart_video_encoder` без build tree.

### synthetic
- generate corpus;
- encode baseline/adaptive;
- calculate metrics.

### hardware jobs

Tagged runners:

```text
nvidia
intel
rockchip
```

Hardware job должен:
1. собрать capability report;
2. сохранить environment manifest;
3. выполнить smoke;
4. выполнить selected quality suite.

Native hardware job обязан доказать, что выбран соответствующий native backend module, а не
одноимённый hardware encoder через FFmpeg.

---

# 45. GitLab artifacts

Сохранять:

```text
artifacts/
├── binaries/
├── junit/
├── coverage/
├── logs/
├── environment/
├── capability/
├── encoded/
├── decoded/
├── metrics/
├── plots/
├── reports/
└── manifests/
```

Reports:
- JUnit;
- coverage;
- benchmark JSON;
- Markdown summary;
- CSV metrics.

Retention policy:

- полные `encoded/`/`decoded/` видео сохраняются только для failed jobs и release/acceptance runs (retention 30 дней);
- для успешных regular jobs сохраняются metrics/manifests/logs (retention 7 дней) и по одному короткому образцу битстрима (<= 30 секунд) на `backend × codec`;
- nightly/full-dataset артефакты суммарным объёмом > 1 GiB требуют явного job-тега `keep-artifacts`.

---

# 46. Benchmark reproducibility

Каждый acceptance run должен быть воспроизводим через:

```bash
smart-video-benchmark run \
    --protocol SVENC-1.0 \
    --profile profiles/nvenc/h265_object_first_v1.yaml
```

Результат:

```text
results/<benchmark_id>/
```

Внутри:

```text
environment.json
profile.yaml
dataset_manifest.json
model_manifest.json
metrics.json
summary.md
```

---

# 47. CI runners

## x86 generic

Используется для:
- core;
- software;
- unit;
- integration;
- synthetic lightweight tests.

## NVIDIA

Используется для:
- NVENC;
- CUDA frame path;
- H.264/H.265 smoke;
- selected benchmark.

## Intel

Используется для:
- oneVPL;
- hardware encode;
- ROI capability tests.

## Rockchip

Self-hosted ARM runner:
- native build;
- MPP;
- H.264/H.265;
- ROI/CTU tests;
- smoke benchmark.

Rockchip runner используется только для native hardware/MPP-тестов и не является wheel-builder'ом.

## aarch64 builder

Отдельный aarch64 runner (или cross-build environment) используется для:
- сборки aarch64 Python wheels (58.7);
- aarch64 Python test matrix;
- проверки core, `X264Backend` и `X265Backend` на ARM без hardware-зависимостей.

---

# 48. Функциональные acceptance tests

## AT-001 Build

```text
cmake configure = PASS
build = PASS
unit tests = PASS
```

## AT-002 H.264

Каждый certified backend:
- encode;
- decode;
- frame count;
- PTS;
- EOS.

## AT-003 H.265

То же.

## AT-004 ROI normalization

Набор edge cases:
- negative;
- outside frame;
- zero area;
- tiny object;
- overlap.

Ожидаемый результат deterministic.

## AT-005 Temporal miss

ROI сохраняется:

```text
missing <= max_missing_frames
```

Преждевременное исчезновение = FAIL.

## AT-006 Metadata

100% frame mapping.

## AT-007 Capability fallback

Requested unsupported feature:
- explicit `Unsupported` либо documented fallback;
- silent degradation = FAIL.

## AT-008 Python wrapper

- установка wheel в чистый virtual environment, `import smart_video_encoder`;
- encode/receive/flush/EOS на `X264Backend` и `X265Backend`;
- эквивалентность C++/Python packets, PTS и metadata (58.10.1);
- exception mapping для всех `StatusCode`.

## AT-009 10-bit

Для каждого backend с capability `main10`:
- encode/decode P010/Main10 (H.265), 300 frames;
- frame count, monotonic PTS, EOS;
- отсутствие capability — documented SKIP с capability evidence.

---

# 49. Quality acceptance matrix

Раздел 49 — единственный нормативный источник acceptance-порогов. Разделы 14, 16, 34 описывают методику измерения и ссылаются сюда; при расхождении приоритет у настоящей таблицы.

| Category | Metric | Requirement |
|---|---|---|
| Bitrate | Average difference | <= 2% |
| Bitrate | Average overshoot | <= 5% |
| Bitrate | P95 1-second rolling | <= 120% target |
| Synthetic ROI | Mean ΔROI-PSNR-Y | >= +0.5 dB |
| Synthetic ROI | Median ΔROI-PSNR-Y | >= +0.25 dB |
| Synthetic ROI | Регрессии ROI-PSNR-Y > 0.5 dB | <= 10% |
| Natural ROI | Mean ΔROI-VMAF | >= +2.0 |
| Natural ROI | Median ΔROI-VMAF | >= +1.0 |
| Natural ROI | ROI regressions > 1 VMAF | <= 10% |
| ROI effect | QP difference | >= 2 when observable |
| 10-bit | P010/Main10 functional (AT-009) | PASS |
| MOT17 | ΔHOTA | >= -0.5 pp |
| MOT17 | ΔIDF1 | >= -0.5 pp |
| Critical tracks | ΔCritical-IDF1 | >= -0.25 pp |
| Critical tracks | ΔCritical-DetRe | >= -0.25 pp |
| MOT R1/R2 | positive critical gain | >= +0.5 pp at least one |
| Temporal ROI | P95 lag | <= 3 frames and <= 100 ms |
| Temporal ROI | P99 lag | <= 6 frames and <= 200 ms |
| ALPR | ΔExactPlateMatch | >= -0.25 pp |
| ALPR | ΔPlateDetectionRecall | >= -0.25 pp |
| ALPR R1/R2 | positive gain | >= +0.5 pp at least one |
| Python | Обязательные Python tests (58.9) | 100% pass |
| Python | Wrapper overhead P95, CPU submit (58.10.8) | <= 5% |
| Coverage | Line coverage core-модулей (41) | >= 80% |
| Functional | Frame count | exact |
| Functional | Metadata mapping | 100% |
| Functional | Unit/regression tests | 100% pass |
| Stability | Crash/deadlock | 0 |

`pp` = percentage points.

---

# 50. PASS/FAIL rules

Acceptance считается успешным только если:

1. Все L0-L4 обязательные тесты PASS.
2. Все обязательные hardware tests для заявленного backend PASS.
3. Synthetic ROI criteria PASS.
4. Temporal ROI criteria PASS.
5. Task-aware criteria для применимого dataset PASS.
6. Bitrate constraints PASS.
7. Нет silent fallback.
8. Все manifests и versions присутствуют.
9. Benchmark protocol version совпадает с утверждённой.
10. Dataset/model hashes соответствуют manifest.

Если backend не поддерживает функцию:

```text
SKIP
```

разрешён только при capability evidence.

```text
catch(error) -> continue -> PASS
```

запрещено.

---

# 51. Deliverables

Разработчик обязан предоставить:

1. Исходный код.
2. Public headers.
3. CMake project.
4. vcpkg manifest.
5. Dockerfiles.
6. GitLab CI.
7. Unit/integration/functional tests.
8. Synthetic generator.
9. Benchmark runner.
10. Dataset manifests.
11. Model manifests.
12. Quality report generator.
13. Capability CLI.
14. API documentation.
15. Backend support matrix.
16. Example applications.
17. Performance reports.
18. Acceptance report.
19. Python package, type stubs, examples, tests и wheel artifacts.

---

# 52. Example applications

```text
examples/
├── encode_cpu.cpp
├── encode_cuda.cpp
├── adaptive_boxes.cpp
├── adaptive_tracks.cpp
├── metadata_sei.cpp
└── transcode.cpp
```

Минимальный пример:

```cpp
void DrainPackets(sve::SmartVideoEncoder& encoder) {
    sve::EncodedPacket packet;
    while (encoder.ReceivePacket(&packet).ok()) {
        Consume(packet);
    }
}

sve::EncoderConfig config;
config.codec = sve::Codec::H265;
config.backend = sve::Backend::Auto;
config.backend_selection.preferred = {
    sve::Backend::NvidiaNvenc,
    sve::Backend::IntelOneVpl,
    sve::Backend::X265,
};

config.input.pixel_format = sve::PixelFormat::Yuv420P;
config.input.width = 1920;
config.input.height = 1080;
config.input.frame_rate = {30, 1};
config.input.time_base = {1, 90000};

config.target_bitrate = 6'000'000;
config.gop_size = 60;

config.adaptive.enabled = true;

auto encoder = sve::CreateVideoEncoder(config);

while (std::optional<sve::TimedFrame> frame = GetFrame()) {
    frame->metadata.objects = GetDetectorObjects();

    sve::Status s = encoder->SubmitFrame(*frame);
    while (s.code() == sve::StatusCode::Again) {   // backpressure
        DrainPackets(*encoder);                    // освобождаем output queue
        s = encoder->SubmitFrame(*frame);
    }
    if (!s.ok()) HandleError(s);

    DrainPackets(*encoder);
}

if (sve::Status s = encoder->Flush(); !s.ok()) HandleError(s);

sve::EncodedPacket packet;                          // добор до EOS
for (;;) {
    sve::Status s = encoder->ReceivePacket(&packet);
    if (s.code() == sve::StatusCode::EndOfStream) break;
    if (s.code() == sve::StatusCode::Again) continue;
    if (!s.ok()) HandleError(s);
    Consume(packet);
}
```

Вариант с `AVFrame` дополнительно подключает `sve/frame.h`, создаёт `Frame` через
`AvFrameAdapter::Ref` и далее использует тот же `TimedFrame` flow.

---

# 53. Рекомендуемая структура benchmark repository

```text
benchmarks/
├── protocol/
│   ├── SVENC-1.0.md
│   └── svenc-1.0.yaml
├── profiles/
│   ├── common/
│   ├── nvenc/
│   ├── onevpl/
│   └── rockchip/
├── synthetic/
├── datasets/
│   ├── mot17.yaml
│   ├── ufpr-alpr.yaml
│   └── natural-video.yaml
├── models/
│   ├── detector.yaml
│   └── ocr.yaml
├── scripts/
├── results/
└── reports/
```

---

# 54. Этапы реализации

## Milestone 1 — Core

- public API;
- backend-neutral `Frame`/`IFrameBuffer` и opt-in AVFrame interop;
- plugin registry и C ABI loader;
- direct `X264Backend` и `X265Backend` modules;
- pinned libx264/libx265 API integration и codec-native quant-offset conversion;
- обязательный license review software codec modules;
- ROI normalization;
- metadata core;
- Python wrapper и packaging;
- CMake/vcpkg;
- GitLab CI.

## Milestone 2 — Intel native

- native oneVPL backend module без FFmpeg/QSV dependency;
- H.264/H.265;
- ROI DeltaQP;
- capability query;
- Docker/runtime validation.

## Milestone 3 — NVIDIA native

- H.264;
- H.265;
- direct NVENCODE API;
- CUDA input и zero-copy;
- native capability detection;
- adaptive ROI path;
- hardware tests.

## Milestone 4 — Rockchip

- native MPP backend module без FFmpeg dependency;
- H.264/H.265;
- ROI;
- CTU QP where available;
- ARM GitLab runner.

## Milestone 5 — Benchmark

- synthetic corpus;
- VMAF/PSNR/SSIM/MS-SSIM;
- MOT17;
- UFPR-ALPR;
- Task-RD reports.

## Milestone 6 — Release candidate

- soak;
- regression;
- performance;
- acceptance report.

---

# 55. Definition of Done

Версия 1.0 считается завершённой, если:

- библиотека собирается на Ubuntu 24.04;
- используется CMake + vcpkg;
- GitLab CI полностью автоматизирован;
- реализованы H.264 и H.265;
- реализованы независимые direct `X264Backend`, `X265Backend`, native NVIDIA NVENC, native
  Intel oneVPL и Rockchip MPP backend modules согласно capability matrix;
- encoder backend modules не имеют зависимости от FFmpeg и проходят обязательный dependency scan;
- runtime plugin ABI, version check и безопасное отсутствие optional modules работают;
- software fallback через libx264/libx265 задаётся явно в `BackendSelection::preferred`;
- версии, commits, build/API numbers, bit depth и license mode libx264/libx265 зафиксированы в build manifest, а license review пройден до включения modules в release artifacts;
- backend-neutral `Frame` является основным входным интерфейсом, а `AVFrame` поддерживается
  через opt-in `sve/frame.h` interop;
- CPU YUV path (YUV420P/NV12/P010) работает; RGB/BGR conversion path — опционален (раздел 17) и не блокирует релиз;
- 10-bit P010/Main10 path проходит functional codec tests (AT-009) на backend'ах с capability `main10`;
- CUDA path работает на certified NVIDIA profile;
- object metadata преобразуется в ROI/QP policy;
- temporal ROI работает;
- metadata синхронизирована с frame/PTS;
- Python wrapper предоставляет основные encode, ROI/metadata, capability и statistics сценарии;
- Python wheels собираются и проходят тесты в поддерживаемой Python matrix;
- synthetic benchmark проходит acceptance;
- MOT17 и UFPR-ALPR проходят применимые task criteria;
- baseline profiles versioned;
- benchmark results reproducible;
- все обязательные GitLab CI stages PASS;
- release candidate проходит stability suite.

---

# 56. Нормативные технические источники для реализации

Проект должен опираться прежде всего на официальную документацию конкретного backend и фиксировать точную версию API/SDK в build manifest.

Основные источники:
- FFmpeg API и codec documentation;
- NVIDIA Video Codec SDK / NVENC documentation;
- Intel oneVPL API;
- Rockchip MPP documentation и исходный код;
- Netflix VMAF;
- MOTChallenge/MOT evaluation tooling;
- документация GitLab CI/CD.

При изменении версии backend API необходимо повторно выполнить capability discovery и regression benchmark.

---

# 57. Архитектурное резюме

Рекомендуемая модель:

```text
                    +----------------+
Detector / Tracker ->| Object Metadata|
                    +--------+-------+
                             |
                             v
                    +----------------+
                    | Smart Policy   |
                    | ROI / Priority |
                    | Temporal Model |
                    +--------+-------+
                             |
                             v
                    +----------------+
                    | Canonical ROI  |
                    | / Quality Map  |
                    +--------+-------+
                             |
           +------------+----------+----------+------------+
           |            |          |          |
           v            v          v          v
       x264/x265      NVENC      oneVPL       MPP
       direct SW      native     native       native
           |            |          |          |
           +------------+----------+----------+
                             |
                             v
                  EncodedPacket/SEI
                             |
                             v
                     Video + Metadata
```

Ключевое требование:

> Публичная логика object-first compression должна быть backend-independent; software encode
> выполняется direct x264/x265 modules, а различия x264/x265/NVENC/oneVPL/Rockchip изолированы
> в независимых capability-driven modules. FFmpeg используется только для opt-in interop.

Это позволяет:
- использовать один API для камер и transcoding;
- не привязывать бизнес-логику к одному GPU vendor;
- постепенно добавлять VVC/AV1;
- сравнивать backend'ы единым benchmark protocol;
- доказывать пользу adaptive compression не только через PSNR/VMAF, но и через downstream CV metrics.

---

# 58. Python wrapper

## 58.1 Обязательность и назначение

Для библиотеки должен быть реализован официальный Python package `smart-video-encoder`, импортируемый как `smart_video_encoder`. Wrapper входит в P0 и предназначен для интеграции с detector/tracker pipelines, benchmark tooling, Jupyter и Python-сервисами без дублирования encoder logic на Python.

Python layer должен быть тонким: кодирование, ROI policy, temporal state, capability mapping, metadata synchronization и backend fallback остаются в C++ core. Python wrapper отвечает за преобразование типов, lifetime, исключения, GIL и удобный Pythonic API.

## 58.2 Выбор технологии

Рассмотрены два основных варианта:

| Критерий | pybind11 | Cython |
|---|---|---|
| Интеграция с современным C++20 API | прямая, natural binding классов, enum, STL и smart pointers | требует `.pyx`/`.pxd`, C/C++ declarations и дополнительного generated layer |
| CMake integration | native CMake target | отдельный Cython generation/build step |
| Ownership и RAII | хорошо соответствует `unique_ptr`, `shared_ptr` и C++ destructors | возможно, но требует более явного управления wrapper objects |
| NumPy/buffer protocol | встроенная поддержка массивов и buffers | зрелая typed-memoryview/NumPy integration |
| Исключения | прямой mapping и custom exception translators | ручное описание и преобразование ошибок |
| GIL | явный scoped release/call guards | зрелые `nogil` sections |
| Поддерживаемость небольшого C++ API | меньше boilerplate, bindings находятся рядом с C++ типами | выгоднее при большом Python-first numeric layer или тесной обвязке C API |

**Выбор P0: pybind11.**

Причины:

1. Core является C++20 библиотекой с классами, enum, RAII и backend-independent API, а не C API.
2. Проект уже использует CMake; pybind11 естественно добавляется отдельным CMake target.
3. Binding layer должен оставаться тонким и не содержать алгоритмической логики.
4. pybind11 упрощает binding `EncoderConfig`, metadata structures, capabilities, statistics и encoder lifetime.
5. NumPy/buffer protocol и явное освобождение GIL достаточны для CPU frame path и длительных encode operations.

Cython не выбирается как основной binding generator. Он допускается в будущем только для изолированного Python-first preprocessing module, если profiling покажет измеримую пользу typed memoryviews или generated C loops. Публичный Python API не должен зависеть от выбранного внутреннего binding technology.

При выборе учитываются подходы похожих native media/computer-vision проектов: PyAV применяет Cython для глубокой обвязки FFmpeg C API, тогда как для небольшого class-oriented C++20 API целесообразнее прямой binding через pybind11. Нормативными источниками реализации должны быть официальные руководства pybind11, Cython, CPython buffer protocol, NumPy и Python packaging.

## 58.3 Python API P0

Wrapper должен предоставлять:

```python
import smart_video_encoder as sve

config = sve.EncoderConfig(
    codec=sve.Codec.H265,
    backend=sve.Backend.AUTO,
    backend_selection=sve.BackendSelection(
        preferred=[
            sve.Backend.NVIDIA_NVENC,
            sve.Backend.INTEL_ONE_VPL,
            sve.Backend.X265,
        ],
    ),
    input=sve.InputVideoFormat(
        pixel_format=sve.PixelFormat.YUV420P,
        width=1920,
        height=1080,
        frame_rate=sve.Rational(30, 1),
        time_base=sve.Rational(1, 90_000),
    ),
    target_bitrate=6_000_000,
)

with sve.SmartVideoEncoder(config) as encoder:
    encoder.submit_frame(frame, metadata)
    packets = encoder.receive_available()
    encoder.flush()
```

Обязательные bindings:

- `SmartVideoEncoder` с context manager и deterministic `close()`;
- `EncoderConfig`, `AdaptiveConfig`, `TemporalROIConfig`, `MetadataConfig`;
- `Codec`, `Backend`, `RateControl`, `ObjectPriority`, `StatusCode`;
- `BoundingBox`, `Polygon`, `ObjectMetadata`, `FrameMetadata`;
- `EncodedPacket`, `EncodedFrameMetadata`;
- `EncoderCapabilities`, `EncoderStatistics`;
- `submit_frame`, `receive_packet`/`receive_available`, `flush`, `get_capabilities`, `get_statistics`;
- module-level capability discovery без обязательного открытия encoder;
- Python exception hierarchy, сохраняющая `StatusCode`, backend, codec, native error code и исходное сообщение.

## 58.4 Frame interoperability

P0 должен поддерживать CPU frames через NumPy и Python buffer protocol:

- planar YUV420P;
- NV12;
- P010 с корректным dtype/stride;
- явное описание width, height, pixel format, PTS, plane offsets и strides;
- non-contiguous arrays только при валидных strides; иначе `ValueError`;
- read-only input, если C++ encoder не должен изменять frame memory.

Для каждого input wrapper обязан валидировать dtype, shape, strides, alignment, plane sizes и lifetime до передачи в C++.

Zero-copy разрешён только если lifetime Python owner гарантирован до завершения асинхронной C++ операции. Binding должен удерживать strong reference на owner либо создавать безопасный `AVFrame` reference с корректным release callback. Если гарантия невозможна, выполняется явная копия, отражаемая в statistics/telemetry. Скрытое использование dangling pointer запрещено.

Опциональная P1 interoperability:

- PyAV `VideoFrame` через документированный adapter, без зависимости C++ core от private PyAV ABI;
- CUDA Array Interface и/или DLPack для совместимых CUDA buffers;
- CuPy/PyTorch adapters поверх стабильного buffer/DLPack contract.

Wrapper не должен принимать произвольный integer как raw `AVFrame*` или device pointer в safe public API. Unsafe interop допускается только отдельным явно обозначенным expert API.

## 58.5 GIL, threading и callbacks

- GIL должен освобождаться на потенциально блокирующих вызовах `submit_frame`, `receive_packet`, `flush`, ожидании очередей и backend encode operations.
- GIL нельзя удерживать на протяжении hardware encode или ожидания output queue.
- Перед созданием Python objects, преобразованием exception или вызовом Python callback GIL должен быть захвачен повторно.
- C++ worker threads не должны вызывать Python callbacks после уничтожения wrapper/interpreter shutdown.
- P0 не должен зависеть от callbacks из encode hot path; предпочтительны polling/iterator/queue APIs.
- Thread-safety Python API должна соответствовать гарантиям C++ API и быть явно документирована.

## 58.6 Ownership и error mapping

- Python object владеет C++ encoder через RAII-compatible holder.
- `close()` и context manager должны быть idempotent.
- После `close()` операции возвращают предсказуемую Python exception.
- `EncodedPacket` должен владеть или reference-count безопасно удерживать packet data; view не может пережить owner.
- C++ exceptions не должны пересекать CPython ABI boundary.
- `StatusCode::Again` и `EndOfStream` должны иметь документированную Python semantics; они не должны маскироваться как неизвестный `RuntimeError`.
- Ошибки invalid shape/dtype/strides отображаются в `ValueError`/`TypeError`; backend failures — в специализированные `SmartVideoEncoderError` subclasses.

## 58.7 Packaging

Использовать:

- `pyproject.toml`;
- `scikit-build-core` как PEP 517 build backend;
- pybind11 CMake target;
- package layout `python/smart_video_encoder`;
- type hints и `.pyi` stubs либо эквивалентную typed API документацию;
- wheel build для поддерживаемых CPython versions на Linux x86_64 и Linux aarch64;
- source distribution, если она воспроизводимо собирается с документированными system dependencies.

Wheel не должен незаконно или технически небезопасно vendor'ить libx264/libx265, GPU drivers,
NVIDIA kernel components либо host-specific hardware runtime. Политика bundling
libx264/libx265, optional FFmpeg interop, oneVPL и MPP shared libraries должна быть
документирована с учётом ABI, лицензий и wheel repair. При использовании системных libraries
import-time diagnostics должны сообщать отсутствующую library и ожидаемую ABI/version.

Python и C++ package versions должны происходить из одного version source. Build manifest
Python package должен включать git commit, build ID, compiler, C++ ABI, pinned libx264/libx265
versions/build numbers, optional FFmpeg interop version, backend versions и enabled features.

## 58.8 CMake и структура

Рекомендуемая структура:

```text
python/
├── pyproject.toml
├── CMakeLists.txt
├── src/
│   ├── module.cpp
│   ├── encoder_bindings.cpp
│   ├── config_bindings.cpp
│   ├── metadata_bindings.cpp
│   ├── numpy_frame.cpp
│   └── exception_translation.cpp
├── smart_video_encoder/
│   ├── __init__.py
│   └── py.typed
├── tests/
└── examples/
```

Python extension собирается только при `SVE_BUILD_PYTHON=ON`. C++ core не должен зависеть от Python, NumPy или pybind11. Binding target линкуется с публичной C++ library и не обращается к private backend internals.

## 58.9 Тестирование и CI

Обязательные Python tests:

- import и version/build manifest;
- создание/закрытие encoder и context manager;
- config/enums/metadata round-trip;
- NumPy YUV420P/NV12/P010 shape, dtype и stride validation;
- ownership/lifetime tests, включая удаление исходного NumPy owner после submit;
- encode/receive/flush/EOS на `X264Backend` и `X265Backend`;
- H.264/H.265 decode validation и точный frame count;
- PTS и metadata mapping 100%;
- exception mapping для всех `StatusCode`;
- GIL concurrency test: другой Python thread продолжает выполняться во время blocking encode/flush;
- repeated create/destroy и leak checks;
- wheel installation test в чистом virtual environment;
- hardware smoke для NVENC/oneVPL/Rockchip wheels на tagged runners.

GitLab CI matrix должна включать все поддерживаемые CPython minor versions, Debug/Release extension build, Linux x86_64 и Linux aarch64. Минимально поддерживаемые версии Python фиксируются в release manifest; на момент релиза выбираются поддерживаемые upstream версии CPython, а не бессрочно захардкоженный диапазон.

## 58.10 Acceptance Python wrapper

Python wrapper принимается, если:

1. Основные C++ и Python сценарии дают функционально эквивалентные packets, PTS, metadata и capability reports при одинаковой конфигурации.
2. Software encode/decode H.264/H.265 проходит для всей Python CI matrix.
3. Некорректные buffer shape/dtype/strides отклоняются детерминированно без crash или memory corruption.
4. Ownership tests не обнаруживают use-after-free, double-free или утечки references.
5. Blocking native calls освобождают GIL.
6. Установка wheel в clean environment и `import smart_video_encoder` проходят без build tree.
7. Hardware-specific отсутствие runtime library или device возвращает диагностируемую Python exception, а не import crash.
8. Wrapper overhead измеряется отдельно от encode latency; для CPU frame submit без обязательной копии P95 overhead не должен превышать 5% относительно эквивалентного C++ вызова на том же профиле.
9. API reference, type hints и минимум три примера поставляются вместе с package: CPU NumPy encode, adaptive boxes и capability discovery.

---

## Приложение A. Рекомендуемые профили

### A.1 Object-first primary

```yaml
profile: object-first-primary-v1

codec: h265
rate_control: vbr

intra_period_seconds: 2
b_frames: 0

lookahead: false
spatial_aq: false
temporal_aq: false

adaptive:
  enabled: true
  policy: object-first
```

### A.2 Baseline

```yaml
profile: baseline-primary-v1

codec: h265
rate_control: vbr

intra_period_seconds: 2
b_frames: 0

lookahead: false
spatial_aq: false
temporal_aq: false

adaptive:
  enabled: false
```

---

## Приложение B. Обязательные manifest поля

```yaml
protocol_version:
benchmark_id:

library:
  git_commit:
  build_id:

backend:
  name:
  version:

hardware:
  model:
  driver:
  firmware:

encoder:
  codec:
  preset:
  rate_control:
  target_bitrate:
  max_bitrate:
  vbv:
  gop:
  b_frames:
  lookahead:
  aq:

input:
  width:
  height:
  fps:
  pixel_format:

dataset:
  name:
  version:
  manifest_sha256:

models:
  detector:
    name:
    version:
    sha256:
  ocr:
    name:
    version:
    sha256:

metrics:
  vmaf_version:
  evaluator_version:
```

---

## Приложение C. Открытые определения API

Следующие типы упоминаются в ТЗ, но их полные определения фиксируются на design review до code freeze Milestone 1: `AdaptiveConfig`, `MetadataConfig`, `PointF`, `Status`, `Result<T>`, `BackendSession`, `SessionPtr`, `IFrameBuffer`, `EncodedPacket`, `EncoderStatistics`, `BackendStatistics`, `AppliedROIInfo`, `ResolvedROI`, `QPMap`, `FrameQualityBudget`, `BackendFrameControl`, `MetadataCoordinateSystem`.

Минимальные обязательные поля, на которые ссылается текст ТЗ:

```cpp
enum class FallbackPolicy {
    Permissive,   // default: documented fallback + запись в telemetry (раздел 22)
    Strict        // Unsupported вместо деградации
};

struct AdaptiveConfig {
    bool enabled = true;
    FallbackPolicy fallback_policy = FallbackPolicy::Permissive;
    // остальные поля фиксируются на design review
};
```

Изменение открытых определений после design review проходит через minor/major версию по правилам 26.3.
