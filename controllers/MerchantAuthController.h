#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class MerchantAuthController : public HttpController<MerchantAuthController>
{
public:
    METHOD_LIST_BEGIN

    ADD_METHOD_TO(
        MerchantAuthController::login,
        "/api/merchant/login",
        Post);

    METHOD_LIST_END

    void login(
        const HttpRequestPtr& req,
        std::function<void(const HttpResponsePtr&)>&& callback);
};