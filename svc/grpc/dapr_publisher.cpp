#include "grpc/dapr_publisher.h"

#include <cstdio>
#include <cstdlib>

DaprPublisher::DaprPublisher()
{
    const char* port = std::getenv("DAPR_GRPC_PORT");
    const char* pubsub = std::getenv("PUBSUB_NAME");
    if (!port || !*port || !pubsub || !*pubsub) {
        return;
    }
    pubsub_name_ = pubsub;
    channel_ = grpc::CreateChannel(
        std::string("127.0.0.1:") + port, grpc::InsecureChannelCredentials());
    stub_ = dapr::proto::runtime::v1::Dapr::NewStub(channel_);
}

bool DaprPublisher::publish_json(
    const std::string& topic, const std::string& payload) const
{
    if (!stub_) {
        return false;
    }
    dapr::proto::runtime::v1::PublishEventRequest request;
    request.set_pubsub_name(pubsub_name_);
    request.set_topic(topic);
    request.set_data(payload);
    request.set_data_content_type("application/json");

    google::protobuf::Empty response;
    grpc::ClientContext context;
    const grpc::Status status =
        stub_->PublishEvent(&context, request, &response);
    if (!status.ok()) {
        std::fprintf(stderr, "dapr publish failed topic=%s err=%s\n",
            topic.c_str(), status.error_message().c_str());
        return false;
    }
    return true;
}
