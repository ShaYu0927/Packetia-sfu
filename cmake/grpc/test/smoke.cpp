#include "smoke.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <iostream>

namespace
{
class EchoService final : public packetia::grpc_test::Echo::Service
{
    grpc::Status Call(grpc::ServerContext*, const packetia::grpc_test::EchoMessage* request,
                      packetia::grpc_test::EchoMessage* response) override
    {
        response->set_text(request->text());
        return grpc::Status::OK;
    }
};
}

int main()
{
    EchoService service;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server || !port)
    {
        std::cerr << "Could not start local gRPC server\n";
        return 1;
    }

    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                       grpc::InsecureChannelCredentials());
    auto client = packetia::grpc_test::Echo::NewStub(channel);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    packetia::grpc_test::EchoMessage request, response;
    request.set_text("Packetia gRPC source/package smoke test");
    const auto status = client->Call(&context, request, &response);
    server->Shutdown();
    server->Wait();
    if (!status.ok() || response.text() != request.text())
    {
        std::cerr << "gRPC round trip failed: " << status.error_message() << '\n';
        return 1;
    }
    std::cout << response.text() << '\n';
    return 0;
}
