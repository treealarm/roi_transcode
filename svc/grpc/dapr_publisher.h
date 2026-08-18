#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "dapr/dapr_runtime.grpc.pb.h"

class DaprPublisher {
public:
    // No-op if DAPR_GRPC_PORT / PUBSUB_NAME unset.
    DaprPublisher();

    [[nodiscard]] bool enabled() const { return stub_ != nullptr; }
    bool publish_json(const std::string& topic, const std::string& payload) const;

private:
    std::string pubsub_name_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<dapr::proto::runtime::v1::Dapr::Stub> stub_;
};
