#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class UploadController : public HttpController<UploadController> {
public:
    METHOD_LIST_BEGIN

    ADD_METHOD_TO(
        UploadController::uploadProductImage,
        "/api/upload/product-image",
        Post);

    ADD_METHOD_TO(
        UploadController::getProductImage,
        "/uploads/products/{1}",
        Get);

    METHOD_LIST_END

    void uploadProductImage(
        const HttpRequestPtr& req,
        std::function<void(const HttpResponsePtr&)>&& callback);

    void getProductImage(
        const HttpRequestPtr& req,
        std::function<void(const HttpResponsePtr&)>&& callback,
        const std::string& filename);
};