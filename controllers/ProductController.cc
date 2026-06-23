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