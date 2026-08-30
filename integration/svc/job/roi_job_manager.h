#pragma once

#include "sve/types.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

enum class RoiJobState {
    Running,
    Completed,
    Failed,
    Cancelled,
};

struct RoiJobStatus {
    std::string job_id;
    std::string input_path;
    std::string output_path;
    RoiJobState state = RoiJobState::Running;
    std::string message;
    std::string encoder_used;
    bool cpu_h265_target_guard = false;
    bool roi_lag_measured = false;
    int64_t roi_lag_samples = 0;
    float roi_lag_min_iou = 0.0f;
    double roi_lag_p95_frames = 0.0;
    double roi_lag_p99_frames = 0.0;
    double roi_lag_p95_ms = 0.0;
    double roi_lag_p99_ms = 0.0;
    bool roi_lag_acceptance = false;
    std::chrono::system_clock::time_point start_ts{};
    std::chrono::system_clock::time_point end_ts{};
};

struct RoiJobRequest {
    std::string input_path;
    std::string output_path;
    std::vector<sve::Box> boxes;
    sve::Settings settings;
};

class RoiJobManager {
public:
    static RoiJobManager& instance();

    RoiJobManager(const RoiJobManager&) = delete;
    RoiJobManager& operator=(const RoiJobManager&) = delete;

    std::optional<RoiJobStatus> start(RoiJobRequest req, std::string& error);
    [[nodiscard]] std::optional<RoiJobStatus> get(const std::string& job_id) const;
    bool cancel(const std::string& job_id);
    [[nodiscard]] std::vector<RoiJobStatus> list() const;
    void shutdown();

    using StatusHook = std::function<void(const RoiJobStatus&)>;
    void set_status_hook(StatusHook hook);

private:
    RoiJobManager() = default;
    ~RoiJobManager();

    struct Job {
        RoiJobStatus status;
        std::atomic_bool cancel{false};
        std::thread worker;
    };

    void run_job(const std::shared_ptr<Job>& entry, RoiJobRequest req);
    void prune_finished_locked(std::vector<std::thread>& to_join);
    void notify(const RoiJobStatus& st) const;

    static constexpr std::size_t k_max_remembered = 64;

    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<Job>> jobs_;
    StatusHook hook_ = nullptr;
};
