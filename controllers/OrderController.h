#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class OrderController : public HttpController<OrderController>
{
public:
    METHOD_LIST_BEGIN

    ADD_METHOD_TO(
        OrderController::updateOrderStatus,
        "/api/orders/{1}/status",
        Patch);

    ADD_METHOD_TO(
        OrderController::createOrder,
        "/api/orders",
        Post);

    ADD_METHOD_TO(
        OrderController::listOrders,
        "/api/orders",
        Get);

    METHOD_LIST_END

    void createOrder(
        const HttpRequestPtr& req,
        std::function<void(const HttpResponsePtr&)>&& callback);

    void listOrders(
        const HttpRequestPtr& req,
        std::function<void(const HttpResponsePtr&)>&& callback);

    void updateOrderStatus(
        const HttpRequestPtr& req,
        std::function<void(const HttpResponsePtr&)>&& callback,
        int orderId);
};