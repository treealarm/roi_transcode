#include "job/roi_job_manager.h"

#include "roi/roi.h"

#include <format>
#include <stdexcept>
#include <vector>

namespace {
std::string make_job_id()
{
    using clock = std::chrono::system_clock;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now().time_since_epoch())
                        .count();
    static std::atomic<uint32_t> seq{0};
    return std::format("roi-{}-{}", ms, seq.fetch_add(1) + 1);
}
} // namespace

RoiJobManager& RoiJobManager::instance()
{
    static RoiJobManager m;
    return m;
}

RoiJobManager::~RoiJobManager()
{
    shutdown();
}

void RoiJobManager::shutdown()
{
    set_status_hook(nullptr);
    std::vector<std::shared_ptr<Job>> copy;
    {
        std::lock_guard lock(mutex_);
        copy.assign(jobs_.begin(), jobs_.end());
        jobs_.clear();
    }
    for (auto& j : copy) {
        j->cancel.store(true);
        if (j->worker.joinable()) {
            j->worker.join();
        }
    }
}

void RoiJobManager::set_status_hook(StatusHook hook)
{
    std::lock_guard lock(mutex_);
    hook_ = hook;
}

void RoiJobManager::notify(const RoiJobStatus& st) const
{
    StatusHook h = nullptr;
    {
        std::lock_guard lock(mutex_);
        h = hook_;
    }
    if (h) {
        h(st);
    }
}

void RoiJobManager::prune_finished_locked(std::vector<std::thread>& to_join)
{
    if (jobs_.size() <= k_max_remembered) {
        return;
    }
    std::size_t overflow = jobs_.size() - k_max_remembered;
    std::deque<std::shared_ptr<Job>> kept;
    for (auto& j : jobs_) {
        if (overflow > 0 && j->status.state != RoiJobState::Running) {
            if (j->worker.joinable()) {
                to_join.push_back(std::move(j->worker));
            }
            --overflow;
            continue;
        }
        kept.push_back(std::move(j));
    }
    jobs_ = std::move(kept);
}

std::optional<RoiJobStatus> RoiJobManager::start(
    RoiJobRequest req, std::string& error)
{
    if (req.input_path.empty() || req.output_path.empty()) {
        error = "input_path and output_path are required";
        return std::nullopt;
    }

    auto entry = std::make_shared<Job>();
    entry->status.job_id = make_job_id();
    entry->status.input_path = req.input_path;
    entry->status.output_path = req.output_path;
    entry->status.state = RoiJobState::Running;
    entry->status.start_ts = std::chrono::system_clock::now();
    entry->status.message = "running";

    {
        std::vector<std::thread> to_join;
        {
            std::lock_guard lock(mutex_);
            prune_finished_locked(to_join);
            jobs_.push_back(entry);
        }
        for (auto& t : to_join) {
            t.join();
        }
    }

    notify(entry->status);
    entry->worker = std::thread([this, entry, req = std::move(req)]() mutable {
        run_job(entry, std::move(req));
    });
    return entry->status;
}

void RoiJobManager::run_job(const std::shared_ptr<Job>& entry, RoiJobRequest req)
{
    RoiJobStatus snapshot;
    try {
        if (entry->cancel.load()) {
            throw std::runtime_error("cancelled");
        }
        const roi::Report report = roi::transcode_file(
            req.input_path, req.output_path, req.boxes, req.settings,
            &entry->cancel);
        {
            std::lock_guard lock(mutex_);
            if (entry->cancel.load()) {
                entry->status.state = RoiJobState::Cancelled;
                entry->status.message = "cancelled";
            } else {
                entry->status.state = RoiJobState::Completed;
                entry->status.message = "ok";
                // With ENCODER_AUTO the backend is only known once one opened,
                // and the difference matters enough to report rather than to
                // leave the caller reading the process log.
                entry->status.encoder_used = report.encoder_name;
            }
            entry->status.end_ts = std::chrono::system_clock::now();
            snapshot = entry->status;
        }
        notify(snapshot);
    } catch (const std::exception& ex) {
        {
            std::lock_guard lock(mutex_);
            const bool was_cancel = entry->cancel.load()
                || std::string(ex.what()) == "cancelled";
            entry->status.state =
                was_cancel ? RoiJobState::Cancelled : RoiJobState::Failed;
            entry->status.message = ex.what();
            entry->status.end_ts = std::chrono::system_clock::now();
            snapshot = entry->status;
        }
        notify(snapshot);
    }
}

std::optional<RoiJobStatus> RoiJobManager::get(const std::string& job_id) const
{
    std::lock_guard lock(mutex_);
    for (const auto& j : jobs_) {
        if (j->status.job_id == job_id) {
            return j->status;
        }
    }
    return std::nullopt;
}

bool RoiJobManager::cancel(const std::string& job_id)
{
    std::lock_guard lock(mutex_);
    for (auto& j : jobs_) {
        if (j->status.job_id == job_id
            && j->status.state == RoiJobState::Running) {
            j->cancel.store(true);
            j->status.message = "cancelling";
            return true;
        }
    }
    return false;
}

std::vector<RoiJobStatus> RoiJobManager::list() const
{
    std::lock_guard lock(mutex_);
    std::vector<RoiJobStatus> out;
    out.reserve(jobs_.size());
    for (const auto& j : jobs_) {
        out.push_back(j->status);
    }
    return out;
}
