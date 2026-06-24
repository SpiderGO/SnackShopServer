#include "ProductController.h"

#include <sqlite3.h>

#include <string>
#include <vector>

namespace {

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
    result["data"] = Json::arrayValue;
    return result;
}

std::string getTextColumn(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    if (text == nullptr) {
        return "";
    }

    return reinterpret_cast<const char*>(text);
}

}

void ProductController::listProducts(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    (void)req;

    sqlite3* db = nullptr;

    if (!openDatabase(&db)) {
        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("failed to open database")
        );
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    const char* sql =
        "SELECT id, name, category, price, stock, enabled "
        "FROM products "
        "WHERE enabled = 1 "
        "ORDER BY id ASC;";

    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string error = sqlite3_errmsg(db);

        sqlite3_close(db);

        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("failed to query products: " + error)
        );
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    Json::Value products(Json::arrayValue);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Json::Value product;

        product["id"] = sqlite3_column_int(stmt, 0);
        product["name"] = getTextColumn(stmt, 1);
        product["category"] = getTextColumn(stmt, 2);
        product["price"] = sqlite3_column_double(stmt, 3);
        product["stock"] = sqlite3_column_int(stmt, 4);
        product["enabled"] = sqlite3_column_int(stmt, 5) == 1;

        products.append(product);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = products;

    auto resp = HttpResponse::newHttpJsonResponse(result);
    callback(resp);
}

void ProductController::updateStock(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    int productId)
{
    auto json = req->getJsonObject();

    if (!json || !json->isMember("stock") || !(*json)["stock"].isInt()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("invalid request body")
        );
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    int stock = (*json)["stock"].asInt();

    if (productId <= 0 || stock < 0) {
        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("invalid product id or stock")
        );
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    sqlite3* db = nullptr;

    if (!openDatabase(&db)) {
        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("failed to open database")
        );
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    const char* sql =
        "UPDATE products "
        "SET stock = ? "
        "WHERE id = ? AND enabled = 1;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string error = sqlite3_errmsg(db);
        sqlite3_close(db);

        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("failed to prepare stock update: " + error)
        );
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    sqlite3_bind_int(stmt, 1, stock);
    sqlite3_bind_int(stmt, 2, productId);

    rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        std::string error = sqlite3_errmsg(db);
        sqlite3_close(db);

        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("failed to update stock: " + error)
        );
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    sqlite3_finalize(stmt);

    int changedRows = sqlite3_changes(db);
    sqlite3_close(db);

    if (changedRows == 0) {
        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("product not found")
        );
        resp->setStatusCode(k404NotFound);
        callback(resp);
        return;
    }

    Json::Value data;
    data["id"] = productId;
    data["stock"] = stock;

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = data;

    auto resp = HttpResponse::newHttpJsonResponse(result);
    callback(resp);
}

void ProductController::createProduct(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto json = req->getJsonObject();

    if (!json ||
        !json->isMember("name") ||
        !json->isMember("category") ||
        !json->isMember("price") ||
        !json->isMember("stock")) {
        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("invalid request body")
        );
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    std::string name = (*json)["name"].asString();
    std::string category = (*json)["category"].asString();
    double price = (*json)["price"].asDouble();
    int stock = (*json)["stock"].asInt();

    if (name.empty() || category.empty() || price < 0 || stock < 0) {
        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("invalid product fields")
        );
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    sqlite3* db = nullptr;

    if (!openDatabase(&db)) {
        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("failed to open database")
        );
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    const char* sql =
        "INSERT INTO products (name, category, price, stock, enabled) "
        "VALUES (?, ?, ?, ?, 1);";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string error = sqlite3_errmsg(db);
        sqlite3_close(db);

        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("failed to prepare product insert: " + error)
        );
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, price);
    sqlite3_bind_int(stmt, 4, stock);

    rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        std::string error = sqlite3_errmsg(db);
        sqlite3_close(db);

        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("failed to insert product: " + error)
        );
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    sqlite3_finalize(stmt);

    long long productId = sqlite3_last_insert_rowid(db);
    sqlite3_close(db);

    Json::Value data;
    data["id"] = static_cast<Json::Int64>(productId);
    data["name"] = name;
    data["category"] = category;
    data["price"] = price;
    data["stock"] = stock;
    data["enabled"] = true;

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = data;

    auto resp = HttpResponse::newHttpJsonResponse(result);
    callback(resp);
}