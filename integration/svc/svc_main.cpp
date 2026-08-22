#include "grpc/dapr_publisher.h"
#include "grpc/path_guard.h"
#include "grpc/roi_transcode_service.h"
#include "job/roi_job_manager.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <grpcpp/grpcpp.h>

namespace {
std::atomic_bool g_stop{false};

void on_signal(int)
{
    g_stop = true;
}

// A missing required variable is a misconfiguration, and it has to be visible at
// startup rather than through surprising behavior later.
std::string require_env(const char* key)
{
    const char* v = std::getenv(key);
    if (!v || !*v) {
        throw std::runtime_error(
            std::string("Required environment variable '") + key + "' is not set");
    }
    return std::string(v);
}

const char* state_name(RoiJobState s)
{
    switch (s) {
    case RoiJobState::Running:
        return "running";
    case RoiJobState::Completed:
        return "completed";
    case RoiJobState::Failed:
        return "failed";
    case RoiJobState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

std::string json_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                out += std::format("\\u{:04x}", c);
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

void on_job_status(const RoiJobStatus& st, DaprPublisher& publisher)
{
    std::fprintf(stderr, "job %s state=%s msg=%s out=%s\n", st.job_id.c_str(),
        state_name(st.state), st.message.c_str(), st.output_path.c_str());
    if (!publisher.enabled()) {
        return;
    }
    const auto ms = [](std::chrono::system_clock::time_point tp) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            tp.time_since_epoch())
            .count();
    };
    const std::string json = std::format(
        R"({{"job_id":"{}","input_path":"{}","output_path":"{}","state":"{}","message":"{}","start_unix_ms":{},"end_unix_ms":{}}})",
        json_escape(st.job_id), json_escape(st.input_path),
        json_escape(st.output_path), state_name(st.state),
        json_escape(st.message), ms(st.start_ts),
        st.end_ts.time_since_epoch().count() == 0 ? 0 : ms(st.end_ts));
    publisher.publish_json("roi-transcode-job-changed", json);
}
} // namespace

int run()
{
    const std::string app_id = require_env("APP_ID");
    const std::string bind = require_env("GRPC_ROI_BIND");
    const std::string port = require_env("GRPC_ROI_PORT");
    const std::string listen = bind + ":" + port;

    // Resolve the sandbox now. Left to the first request, a bad ROI_ALLOWED_ROOTS
    // would let the process come up looking healthy.
    const auto& roots = init_allowed_roots();

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    auto publisher = std::make_shared<DaprPublisher>();
    RoiJobManager::instance().set_status_hook(
        [publisher](const RoiJobStatus& st) { on_job_status(st, *publisher); });

    // Two services on one port. They are separate contracts, not separate
    // deployments: a caller picks one by name, and both reach the same GPU.
    RoiFileTranscodeServiceImpl file_service;
    RoiStreamTranscodeServiceImpl stream_service;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
    builder.RegisterService(&file_service);
    builder.RegisterService(&stream_service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::fprintf(stderr, "failed to listen on %s\n", listen.c_str());
        return 1;
    }
    std::string root_list;
    for (const auto& root : roots) {
        root_list += (root_list.empty() ? "" : ",") + root;
    }
    std::fprintf(stderr,
        "roi-transcode-svc app_id=%s listening on %s dapr_publish=%s roots=%s\n",
        app_id.c_str(), listen.c_str(),
        publisher->enabled() ? "on" : "off", root_list.c_str());

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    server->Shutdown();
    RoiJobManager::instance().shutdown();
    return 0;
}

int main(int /*argc*/, char** /*argv*/)
{
    try {
        return run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "roi-transcode-svc: %s\n", e.what());
        return 1;
    }
}
