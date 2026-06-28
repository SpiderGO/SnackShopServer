#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class ProductController : public HttpController<ProductController>
{
    public:
        METHOD_LIST_BEGIN

        ADD_METHOD_TO(
            ProductController::listProducts,
            "/api/products",
            Get);
        
        ADD_METHOD_TO(
            ProductController::listAllProductsForMerchant,
            "/api/merchant/products",
            Get);
        
        ADD_METHOD_TO(
            ProductController::updateEnabled,
            "/api/products/{1}/enabled",
            Patch);

        ADD_METHOD_TO(
            ProductController::createProduct,
            "/api/products",
            Post);
        
        ADD_METHOD_TO(
            ProductController::updateStock,
            "/api/products/{1}/stock",
            Patch);
        
        ADD_METHOD_TO(
            ProductController::updateProduct,
            "/api/products/{1}",
            Patch);
        
        ADD_METHOD_TO(
            ProductController::createVariant,
            "/api/products/{1}/variants",
            Post);

        METHOD_LIST_END

        void listProducts(
            const HttpRequestPtr& req,
            std::function<void(const HttpResponsePtr&)>&& callback);
        
        void updateStock(
            const HttpRequestPtr& req,
            std::function<void(const HttpResponsePtr&)>&& callback,
            int productId);
        
        void createProduct(
            const HttpRequestPtr& req,
            std::function<void(const HttpResponsePtr&)>&& callback);

        void listAllProductsForMerchant(
            const HttpRequestPtr& req,
            std::function<void(const HttpResponsePtr&)>&& callback);

        void updateEnabled(
            const HttpRequestPtr& req,
            std::function<void(const HttpResponsePtr&)>&& callback,
            int productId);   
        
        void updateProduct(
            const HttpRequestPtr& req,
            std::function<void(const HttpResponsePtr&)>&& callback,
            int productId);
        
        void createVariant(
            const HttpRequestPtr& req,
            std::function<void(const HttpResponsePtr&)>&& callback,
            int productId);
};