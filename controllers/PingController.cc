#include "PingController.h"

void PingController::ping(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto resp = HttpResponse::newHttpResponse();

    resp->setBody("SnackShop Server Running");

    callback(resp);
}