#pragma once

#include "roi/roi_types.h"
#include "roi/transcoder.h"

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
    std::chrono::system_clock::time_point start_ts{};
    std::chrono::system_clock::time_point end_ts{};
};

struct RoiJobRequest {
    TranscodeOptions options;
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
