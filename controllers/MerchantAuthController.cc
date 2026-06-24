#include "MerchantAuthController.h"

void MerchantAuthController::login(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto json = req->getJsonObject();

    if (!json ||
        !json->isMember("username") ||
        !json->isMember("password")) {
        Json::Value result;
        result["code"] = 1;
        result["message"] = "invalid request body";
        result["data"] = Json::objectValue;

        auto resp = HttpResponse::newHttpJsonResponse(result);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    std::string username = (*json)["username"].asString();
    std::string password = (*json)["password"].asString();

    if (username != "merchant" || password != "123456") {
        Json::Value result;
        result["code"] = 1;
        result["message"] = "invalid username or password";
        result["data"] = Json::objectValue;

        auto resp = HttpResponse::newHttpJsonResponse(result);
        resp->setStatusCode(k401Unauthorized);
        callback(resp);
        return;
    }

    Json::Value data;
    data["token"] = "dev-merchant-token";
    data["role"] = "merchant";

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = data;

    auto resp = HttpResponse::newHttpJsonResponse(result);
    callback(resp);
}