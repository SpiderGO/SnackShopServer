#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class PingController : public HttpController<PingController>
{
public:
    METHOD_LIST_BEGIN

    ADD_METHOD_TO(
        PingController::ping,
        "/ping",
        Get);

    METHOD_LIST_END

    void ping(
        const HttpRequestPtr& req,
        std::function<void(const HttpResponsePtr&)>&& callback);
};