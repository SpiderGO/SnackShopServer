#include "MerchantAuthController.h"

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

Json::Value makeResponseBody(int code, const std::string& message) {
    Json::Value result;
    result["code"] = code;
    result["message"] = message;
    result["data"] = Json::objectValue;
    return result;
}

HttpResponsePtr makeJsonResponse(
    const Json::Value& body,
    HttpStatusCode statusCode = k200OK
) {
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

}

void MerchantAuthController::login(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto json = req->getJsonObject();

    if (!json ||
        !json->isMember("username") ||
        !json->isMember("password")) {
        callback(makeJsonResponse(
            makeResponseBody(1, "invalid request body"),
            k400BadRequest
        ));
        return;
    }

    std::string username = (*json)["username"].asString();
    std::string password = (*json)["password"].asString();

    if (username.empty() || password.empty()) {
        callback(makeJsonResponse(
            makeResponseBody(1, "username and password are required"),
            k400BadRequest
        ));
        return;
    }

    sqlite3* db = nullptr;

    if (!openDatabase(&db)) {
        callback(makeJsonResponse(
            makeResponseBody(1, "failed to open database"),
            k500InternalServerError
        ));
        return;
    }

    const char* sql =
        "SELECT id, username, role "
        "FROM merchant_users "
        "WHERE username = ? AND password = ? AND enabled = 1 "
        "LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string error = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeResponseBody(1, "failed to prepare merchant login: " + error),
            k500InternalServerError
        ));
        return;
    }

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);

    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeResponseBody(1, "invalid username or password"),
            k401Unauthorized
        ));
        return;
    }

    int merchantId = sqlite3_column_int(stmt, 0);
    std::string merchantUsername = getTextColumn(stmt, 1);
    std::string role = getTextColumn(stmt, 2);

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    Json::Value data;
    data["merchantId"] = merchantId;
    data["username"] = merchantUsername;
    data["role"] = role;
    data["token"] = "dev-merchant-token";

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = data;

    callback(makeJsonResponse(result));
}