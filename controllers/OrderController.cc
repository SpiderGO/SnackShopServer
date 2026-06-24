#include "OrderController.h"

#include <sqlite3.h>

#include <string>
#include <vector>

namespace {

struct ProductSnapshot {
    int productId;
    std::string name;
    double price;
    int stock;
    int quantity;
};

bool openDatabase(sqlite3** db) {
    const std::vector<std::string> paths = {
        "../data/snackshop.db",
        "data/snackshop.db"
    };

    for (const auto& path : paths) {
        int rc = sqlite3_open_v2(
            path.c_str(),
            db,
            SQLITE_OPEN_READWRITE,
            nullptr
        );

        if (rc == SQLITE_OK) {
            return true;
        }

        if (*db != nullptr) {
            sqlite3_close(*db);
            *db = nullptr;
        }
    }

    return false;
}

Json::Value makeErrorResponse(const std::string& message) {
    Json::Value result;
    result["code"] = 1;
    result["message"] = message;
    result["data"] = Json::objectValue;
    return result;
}

HttpResponsePtr makeJsonResponse(const Json::Value& body, HttpStatusCode statusCode = k200OK) {
    auto resp = HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(statusCode);
    return resp;
}

std::string getTextColumn(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    if (text == nullptr) {
        return "";
    }

    return reinterpret_cast<const char*>(text);
}

bool execSql(sqlite3* db, const char* sql, std::string& error) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK) {
        if (errMsg != nullptr) {
            error = errMsg;
            sqlite3_free(errMsg);
        } else {
            error = sqlite3_errmsg(db);
        }

        return false;
    }

    return true;
}

bool isMerchantAuthorized(const HttpRequestPtr& req) {
    std::string auth = req->getHeader("Authorization");
    return auth == "Bearer dev-merchant-token";
}

HttpResponsePtr makeUnauthorizedResponse() {
    Json::Value result;
    result["code"] = 1;
    result["message"] = "unauthorized";
    result["data"] = Json::objectValue;

    auto resp = HttpResponse::newHttpJsonResponse(result);
    resp->setStatusCode(k401Unauthorized);
    return resp;
}

}

void OrderController::createOrder(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto json = req->getJsonObject();

    if (!json || !json->isMember("items") || !(*json)["items"].isArray()) {
        callback(makeJsonResponse(
            makeErrorResponse("invalid request body"),
            k400BadRequest
        ));
        return;
    }

    const Json::Value& items = (*json)["items"];

    if (items.empty()) {
        callback(makeJsonResponse(
            makeErrorResponse("order items cannot be empty"),
            k400BadRequest
        ));
        return;
    }

    if (!json->isMember("customerId") || !(*json)["customerId"].isString()) {
        callback(makeJsonResponse(
            makeErrorResponse("customerId is required"),
            k400BadRequest
        ));
        return;
    }

    std::string customerId = (*json)["customerId"].asString();

    if (customerId.empty()) {
        callback(makeJsonResponse(
            makeErrorResponse("customerId cannot be empty"),
            k400BadRequest
        ));
        return;
    }

    std::string pickupType = "到店自提";
    if (json->isMember("pickupType") && (*json)["pickupType"].isString()) {
        pickupType = (*json)["pickupType"].asString();
    }

    sqlite3* db = nullptr;

    if (!openDatabase(&db)) {
        callback(makeJsonResponse(
            makeErrorResponse("failed to open database"),
            k500InternalServerError
        ));
        return;
    }

    std::string error;
    if (!execSql(db, "BEGIN TRANSACTION;", error)) {
        sqlite3_close(db);
        callback(makeJsonResponse(
            makeErrorResponse("failed to begin transaction: " + error),
            k500InternalServerError
        ));
        return;
    }

    const char* queryProductSql =
        "SELECT name, price, stock "
        "FROM products "
        "WHERE id = ? AND enabled = 1;";

    sqlite3_stmt* queryStmt = nullptr;
    int rc = sqlite3_prepare_v2(db, queryProductSql, -1, &queryStmt, nullptr);

    if (rc != SQLITE_OK) {
        execSql(db, "ROLLBACK;", error);
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to prepare product query: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    std::vector<ProductSnapshot> snapshots;
    double totalAmount = 0.0;

    for (const auto& item : items) {
        int productId = 0;

        if (item.isMember("productId")) {
            productId = item["productId"].asInt();
        } else if (item.isMember("id")) {
            productId = item["id"].asInt();
        }

        int quantity = item.isMember("quantity") ? item["quantity"].asInt() : 0;

        if (productId <= 0 || quantity <= 0) {
            sqlite3_finalize(queryStmt);
            execSql(db, "ROLLBACK;", error);
            sqlite3_close(db);

            callback(makeJsonResponse(
                makeErrorResponse("invalid product id or quantity"),
                k400BadRequest
            ));
            return;
        }

        sqlite3_reset(queryStmt);
        sqlite3_clear_bindings(queryStmt);
        sqlite3_bind_int(queryStmt, 1, productId);

        rc = sqlite3_step(queryStmt);

        if (rc != SQLITE_ROW) {
            sqlite3_finalize(queryStmt);
            execSql(db, "ROLLBACK;", error);
            sqlite3_close(db);

            callback(makeJsonResponse(
                makeErrorResponse("product not found or disabled"),
                k400BadRequest
            ));
            return;
        }

        std::string name = getTextColumn(queryStmt, 0);
        double price = sqlite3_column_double(queryStmt, 1);
        int stock = sqlite3_column_int(queryStmt, 2);

        if (quantity > stock) {
            sqlite3_finalize(queryStmt);
            execSql(db, "ROLLBACK;", error);
            sqlite3_close(db);

            callback(makeJsonResponse(
                makeErrorResponse("insufficient stock for product: " + name),
                k400BadRequest
            ));
            return;
        }

        snapshots.push_back(ProductSnapshot{
            productId,
            name,
            price,
            stock,
            quantity
        });

        totalAmount += price * quantity;
    }

    sqlite3_finalize(queryStmt);

    const char* insertOrderSql =
        "INSERT INTO orders (customer_id, total_amount, status, pickup_type, created_at) "
        "VALUES (?, ?, '待商家确认', ?, datetime('now', 'localtime'));";

    sqlite3_stmt* insertOrderStmt = nullptr;
    rc = sqlite3_prepare_v2(db, insertOrderSql, -1, &insertOrderStmt, nullptr);

    if (rc != SQLITE_OK) {
        execSql(db, "ROLLBACK;", error);
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to prepare order insert: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    sqlite3_bind_text(insertOrderStmt, 1, customerId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(insertOrderStmt, 2, totalAmount);
    sqlite3_bind_text(insertOrderStmt, 3, pickupType.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(insertOrderStmt);

    if (rc != SQLITE_DONE) {
        sqlite3_finalize(insertOrderStmt);
        execSql(db, "ROLLBACK;", error);
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to insert order: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    sqlite3_finalize(insertOrderStmt);

    long long orderId = sqlite3_last_insert_rowid(db);

    const char* insertItemSql =
        "INSERT INTO order_items "
        "(order_id, product_id, product_name, price, quantity) "
        "VALUES (?, ?, ?, ?, ?);";

    const char* updateStockSql =
        "UPDATE products "
        "SET stock = stock - ? "
        "WHERE id = ?;";

    sqlite3_stmt* insertItemStmt = nullptr;
    sqlite3_stmt* updateStockStmt = nullptr;

    rc = sqlite3_prepare_v2(db, insertItemSql, -1, &insertItemStmt, nullptr);
    if (rc != SQLITE_OK) {
        execSql(db, "ROLLBACK;", error);
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to prepare item insert: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    rc = sqlite3_prepare_v2(db, updateStockSql, -1, &updateStockStmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(insertItemStmt);
        execSql(db, "ROLLBACK;", error);
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to prepare stock update: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    for (const auto& product : snapshots) {
        sqlite3_reset(insertItemStmt);
        sqlite3_clear_bindings(insertItemStmt);

        sqlite3_bind_int64(insertItemStmt, 1, orderId);
        sqlite3_bind_int(insertItemStmt, 2, product.productId);
        sqlite3_bind_text(insertItemStmt, 3, product.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(insertItemStmt, 4, product.price);
        sqlite3_bind_int(insertItemStmt, 5, product.quantity);

        rc = sqlite3_step(insertItemStmt);

        if (rc != SQLITE_DONE) {
            sqlite3_finalize(insertItemStmt);
            sqlite3_finalize(updateStockStmt);
            execSql(db, "ROLLBACK;", error);
            std::string dbError = sqlite3_errmsg(db);
            sqlite3_close(db);

            callback(makeJsonResponse(
                makeErrorResponse("failed to insert order item: " + dbError),
                k500InternalServerError
            ));
            return;
        }

        sqlite3_reset(updateStockStmt);
        sqlite3_clear_bindings(updateStockStmt);

        sqlite3_bind_int(updateStockStmt, 1, product.quantity);
        sqlite3_bind_int(updateStockStmt, 2, product.productId);

        rc = sqlite3_step(updateStockStmt);

        if (rc != SQLITE_DONE) {
            sqlite3_finalize(insertItemStmt);
            sqlite3_finalize(updateStockStmt);
            execSql(db, "ROLLBACK;", error);
            std::string dbError = sqlite3_errmsg(db);
            sqlite3_close(db);

            callback(makeJsonResponse(
                makeErrorResponse("failed to update stock: " + dbError),
                k500InternalServerError
            ));
            return;
        }
    }

    sqlite3_finalize(insertItemStmt);
    sqlite3_finalize(updateStockStmt);

    if (!execSql(db, "COMMIT;", error)) {
        execSql(db, "ROLLBACK;", error);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to commit order: " + error),
            k500InternalServerError
        ));
        return;
    }

    sqlite3_close(db);

    Json::Value data;
    data["id"] = static_cast<Json::Int64>(orderId);
    data["totalAmount"] = totalAmount;
    data["status"] = "待商家确认";
    data["pickupType"] = pickupType;

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = data;

    callback(makeJsonResponse(result));
}

void OrderController::listOrders(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    std::string customerId = req->getParameter("customerId");

    if (customerId.empty()) {
        callback(makeJsonResponse(
            makeErrorResponse("customerId is required"),
            k400BadRequest
        ));
        return;
    }

    (void)req;

    sqlite3* db = nullptr;

    if (!openDatabase(&db)) {
        callback(makeJsonResponse(
            makeErrorResponse("failed to open database"),
            k500InternalServerError
        ));
        return;
    }

    const char* orderSql =
        "SELECT id, total_amount, status, pickup_type, created_at "
        "FROM orders "
        "WHERE customer_id = ? "
        "ORDER BY id DESC;";

    sqlite3_stmt* orderStmt = nullptr;
    int rc = sqlite3_prepare_v2(db, orderSql, -1, &orderStmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to query orders: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    Json::Value orders(Json::arrayValue);

    const char* itemSql =
        "SELECT product_id, product_name, price, quantity "
        "FROM order_items "
        "WHERE order_id = ? "
        "ORDER BY id ASC;";

    sqlite3_stmt* itemStmt = nullptr;
    rc = sqlite3_prepare_v2(db, itemSql, -1, &itemStmt, nullptr);

    if (rc != SQLITE_OK) {
        sqlite3_finalize(orderStmt);
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to prepare order item query: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    sqlite3_bind_text(orderStmt, 1, customerId.c_str(), -1, SQLITE_TRANSIENT);

    while ((rc = sqlite3_step(orderStmt)) == SQLITE_ROW) {
        long long orderId = sqlite3_column_int64(orderStmt, 0);

        Json::Value order;
        order["id"] = static_cast<Json::Int64>(orderId);
        order["totalAmount"] = sqlite3_column_double(orderStmt, 1);
        order["status"] = getTextColumn(orderStmt, 2);
        order["pickupType"] = getTextColumn(orderStmt, 3);
        order["createdAt"] = getTextColumn(orderStmt, 4);

        Json::Value orderItems(Json::arrayValue);

        sqlite3_reset(itemStmt);
        sqlite3_clear_bindings(itemStmt);
        sqlite3_bind_int64(itemStmt, 1, orderId);

        while (sqlite3_step(itemStmt) == SQLITE_ROW) {
            Json::Value product;
            product["id"] = sqlite3_column_int(itemStmt, 0);
            product["name"] = getTextColumn(itemStmt, 1);
            product["price"] = sqlite3_column_double(itemStmt, 2);
            product["quantity"] = sqlite3_column_int(itemStmt, 3);

            orderItems.append(product);
        }

        order["items"] = orderItems;

        orders.append(order);
    }

    sqlite3_finalize(itemStmt);
    sqlite3_finalize(orderStmt);
    sqlite3_close(db);

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = orders;

    callback(makeJsonResponse(result));
}

void OrderController::updateOrderStatus(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    int orderId)
{
    if (!isMerchantAuthorized(req)) {
        callback(makeUnauthorizedResponse());
        return;
    }

    auto json = req->getJsonObject();

    if (!json || !json->isMember("status") || !(*json)["status"].isString()) {
        callback(makeJsonResponse(
            makeErrorResponse("invalid request body"),
            k400BadRequest
        ));
        return;
    }

    std::string status = (*json)["status"].asString();

    if (status != "待商家确认" &&
        status != "已接单" &&
        status != "已完成") {
        callback(makeJsonResponse(
            makeErrorResponse("invalid order status"),
            k400BadRequest
        ));
        return;
    }

    sqlite3* db = nullptr;

    if (!openDatabase(&db)) {
        callback(makeJsonResponse(
            makeErrorResponse("failed to open database"),
            k500InternalServerError
        ));
        return;
    }

    const char* sql =
        "UPDATE orders "
        "SET status = ? "
        "WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to prepare status update: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, orderId);

    rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to update order status: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    sqlite3_finalize(stmt);

    int changedRows = sqlite3_changes(db);
    sqlite3_close(db);

    if (changedRows == 0) {
        callback(makeJsonResponse(
            makeErrorResponse("order not found"),
            k404NotFound
        ));
        return;
    }

    Json::Value data;
    data["id"] = orderId;
    data["status"] = status;

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = data;

    callback(makeJsonResponse(result));
}

void OrderController::listMerchantOrders(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    if (!isMerchantAuthorized(req)) {
        callback(makeUnauthorizedResponse());
        return;
    }

    sqlite3* db = nullptr;

    if (!openDatabase(&db)) {
        callback(makeJsonResponse(
            makeErrorResponse("failed to open database"),
            k500InternalServerError
        ));
        return;
    }

    const char* orderSql =
        "SELECT id, total_amount, status, pickup_type, created_at "
        "FROM orders "
        "ORDER BY id DESC;";

    sqlite3_stmt* orderStmt = nullptr;
    int rc = sqlite3_prepare_v2(db, orderSql, -1, &orderStmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to query merchant orders: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    const char* itemSql =
        "SELECT product_id, product_name, price, quantity "
        "FROM order_items "
        "WHERE order_id = ? "
        "ORDER BY id ASC;";

    sqlite3_stmt* itemStmt = nullptr;
    rc = sqlite3_prepare_v2(db, itemSql, -1, &itemStmt, nullptr);

    if (rc != SQLITE_OK) {
        sqlite3_finalize(orderStmt);
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to prepare merchant order item query: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    Json::Value orders(Json::arrayValue);

    while ((rc = sqlite3_step(orderStmt)) == SQLITE_ROW) {
        long long orderId = sqlite3_column_int64(orderStmt, 0);

        Json::Value order;
        order["id"] = static_cast<Json::Int64>(orderId);
        order["totalAmount"] = sqlite3_column_double(orderStmt, 1);
        order["status"] = getTextColumn(orderStmt, 2);
        order["pickupType"] = getTextColumn(orderStmt, 3);
        order["createdAt"] = getTextColumn(orderStmt, 4);

        Json::Value orderItems(Json::arrayValue);

        sqlite3_reset(itemStmt);
        sqlite3_clear_bindings(itemStmt);
        sqlite3_bind_int64(itemStmt, 1, orderId);

        while (sqlite3_step(itemStmt) == SQLITE_ROW) {
            Json::Value product;
            product["id"] = sqlite3_column_int(itemStmt, 0);
            product["name"] = getTextColumn(itemStmt, 1);
            product["price"] = sqlite3_column_double(itemStmt, 2);
            product["quantity"] = sqlite3_column_int(itemStmt, 3);

            orderItems.append(product);
        }

        order["items"] = orderItems;
        orders.append(order);
    }

    sqlite3_finalize(itemStmt);
    sqlite3_finalize(orderStmt);
    sqlite3_close(db);

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = orders;

    callback(makeJsonResponse(result));
}