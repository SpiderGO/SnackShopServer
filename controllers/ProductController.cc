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

bool isMerchantAuthorized(const HttpRequestPtr& req) {
    std::string auth = req->getHeader("Authorization");
    const std::string prefix = "Bearer ";

    if (auth.rfind(prefix, 0) != 0) {
        return false;
    }

    std::string token = auth.substr(prefix.size());

    if (token.empty()) {
        return false;
    }

    sqlite3* db = nullptr;

    if (!openDatabase(&db)) {
        return false;
    }

    const char* sql =
        "SELECT id "
        "FROM merchant_sessions "
        "WHERE token = ? "
        "AND enabled = 1 "
        "AND expires_at > datetime('now', 'localtime') "
        "LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);

    bool authorized = (rc == SQLITE_ROW);

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return authorized;
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

Json::Value queryProductVariants(sqlite3* db, int productId, bool onlyEnabled) {
    Json::Value variants(Json::arrayValue);

    const char* sqlAll =
        "SELECT id, variant_name, price, stock, image_url, enabled "
        "FROM product_variants "
        "WHERE product_id = ? "
        "ORDER BY id ASC;";

    const char* sqlEnabled =
        "SELECT id, variant_name, price, stock, image_url, enabled "
        "FROM product_variants "
        "WHERE product_id = ? AND enabled = 1 "
        "ORDER BY id ASC;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(
        db,
        onlyEnabled ? sqlEnabled : sqlAll,
        -1,
        &stmt,
        nullptr
    );

    if (rc != SQLITE_OK) {
        return variants;
    }

    sqlite3_bind_int(stmt, 1, productId);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Json::Value variant;

        variant["id"] = sqlite3_column_int(stmt, 0);
        variant["variantName"] = getTextColumn(stmt, 1);
        variant["price"] = sqlite3_column_double(stmt, 2);
        variant["stock"] = sqlite3_column_int(stmt, 3);
        variant["imageUrl"] = getTextColumn(stmt, 4);
        variant["enabled"] = sqlite3_column_int(stmt, 5) == 1;

        variants.append(variant);
    }

    sqlite3_finalize(stmt);
    return variants;
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
            "SELECT "
            "p.id, "
            "p.name, "
            "p.category, "
            "p.main_image_url, "
            "p.enabled, "
            "COALESCE(MIN(v.price), p.price) AS display_price, "
            "COALESCE(SUM(v.stock), p.stock) AS total_stock "
            "FROM products p "
            "LEFT JOIN product_variants v "
            "ON p.id = v.product_id AND v.enabled = 1 "
            "WHERE p.enabled = 1 "
            "GROUP BY p.id "
            "ORDER BY p.id ASC;";

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

        int productId = sqlite3_column_int(stmt, 0);

        product["id"] = productId;
        product["name"] = getTextColumn(stmt, 1);
        product["category"] = getTextColumn(stmt, 2);
        product["mainImageUrl"] = getTextColumn(stmt, 3);
        product["imageUrl"] = getTextColumn(stmt, 3);
        product["enabled"] = sqlite3_column_int(stmt, 4) == 1;
        product["price"] = sqlite3_column_double(stmt, 5);
        product["stock"] = sqlite3_column_int(stmt, 6);
        product["variants"] = queryProductVariants(db, productId, true);

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
    if (!isMerchantAuthorized(req)) {
        callback(makeUnauthorizedResponse());
        return;
    }

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
    if (!isMerchantAuthorized(req)) {
        callback(makeUnauthorizedResponse());
        return;
    }

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

void ProductController::listAllProductsForMerchant(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    if (!isMerchantAuthorized(req)) {
        callback(makeUnauthorizedResponse());
        return;
    }

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
        "SELECT "
        "p.id, "
        "p.name, "
        "p.category, "
        "p.main_image_url, "
        "p.enabled, "
        "COALESCE(MIN(v.price), p.price) AS display_price, "
        "COALESCE(SUM(v.stock), p.stock) AS total_stock "
        "FROM products p "
        "LEFT JOIN product_variants v "
        "ON p.id = v.product_id "
        "GROUP BY p.id "
        "ORDER BY p.id ASC;";

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

        int productId = sqlite3_column_int(stmt, 0);

        product["id"] = productId;
        product["name"] = getTextColumn(stmt, 1);
        product["category"] = getTextColumn(stmt, 2);
        product["mainImageUrl"] = getTextColumn(stmt, 3);
        product["imageUrl"] = getTextColumn(stmt, 3);
        product["enabled"] = sqlite3_column_int(stmt, 4) == 1;
        product["price"] = sqlite3_column_double(stmt, 5);
        product["stock"] = sqlite3_column_int(stmt, 6);
        product["variants"] = queryProductVariants(db, productId, false);

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

void ProductController::updateEnabled(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    int productId)
{
    if (!isMerchantAuthorized(req)) {
        callback(makeUnauthorizedResponse());
        return;
    }

    auto json = req->getJsonObject();

    if (!json || !json->isMember("enabled") || !(*json)["enabled"].isBool()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("invalid request body")
        );
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    bool enabled = (*json)["enabled"].asBool();

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
        "SET enabled = ? "
        "WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string error = sqlite3_errmsg(db);
        sqlite3_close(db);

        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("failed to prepare enabled update: " + error)
        );
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
    sqlite3_bind_int(stmt, 2, productId);

    rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        std::string error = sqlite3_errmsg(db);
        sqlite3_close(db);

        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("failed to update enabled: " + error)
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
    data["enabled"] = enabled;

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = data;

    auto resp = HttpResponse::newHttpJsonResponse(result);
    callback(resp);
}

void ProductController::updateProduct(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    int productId)
{
    if (!isMerchantAuthorized(req)) {
        callback(makeUnauthorizedResponse());
        return;
    }

    auto json = req->getJsonObject();

    if (!json ||
        !json->isMember("name") ||
        !json->isMember("category") ||
        !json->isMember("price")) {
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

    std::string mainImageUrl = "";
    if (json->isMember("mainImageUrl") && (*json)["mainImageUrl"].isString()) {
        mainImageUrl = (*json)["mainImageUrl"].asString();
    }

    if (productId <= 0 || name.empty() || category.empty() || price < 0) {
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
        "UPDATE products "
        "SET name = ?, category = ?, price = ?, main_image_url = ? "
        "WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string error = sqlite3_errmsg(db);
        sqlite3_close(db);

        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("failed to prepare product update: " + error)
        );
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, price);
    sqlite3_bind_text(stmt, 4, mainImageUrl.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, productId);

    rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        std::string error = sqlite3_errmsg(db);
        sqlite3_close(db);

        auto resp = HttpResponse::newHttpJsonResponse(
            makeErrorResponse("failed to update product: " + error)
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
    data["name"] = name;
    data["category"] = category;
    data["price"] = price;
    data["mainImageUrl"] = mainImageUrl;
    data["imageUrl"] = mainImageUrl;
    
    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = data;

    auto resp = HttpResponse::newHttpJsonResponse(result);
    callback(resp);
}

void ProductController::createVariant(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    int productId)
{
    if (!isMerchantAuthorized(req)) {
        callback(makeUnauthorizedResponse());
        return;
    }

    if (productId <= 0) {
        callback(makeJsonResponse(
            makeErrorResponse("invalid product id"),
            k400BadRequest
        ));
        return;
    }

    auto json = req->getJsonObject();

    if (!json ||
        !json->isMember("variantName") ||
        !json->isMember("price") ||
        !json->isMember("stock")) {
        callback(makeJsonResponse(
            makeErrorResponse("invalid request body"),
            k400BadRequest
        ));
        return;
    }

    std::string variantName = (*json)["variantName"].asString();
    double price = (*json)["price"].asDouble();
    int stock = (*json)["stock"].asInt();

    std::string imageUrl = "";
    if (json->isMember("imageUrl") && (*json)["imageUrl"].isString()) {
        imageUrl = (*json)["imageUrl"].asString();
    }

    if (variantName.empty()) {
        callback(makeJsonResponse(
            makeErrorResponse("variantName cannot be empty"),
            k400BadRequest
        ));
        return;
    }

    if (price < 0) {
        callback(makeJsonResponse(
            makeErrorResponse("price cannot be negative"),
            k400BadRequest
        ));
        return;
    }

    if (stock < 0) {
        callback(makeJsonResponse(
            makeErrorResponse("stock cannot be negative"),
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

    const char* checkProductSql =
        "SELECT id "
        "FROM products "
        "WHERE id = ? "
        "LIMIT 1;";

    sqlite3_stmt* checkStmt = nullptr;
    int rc = sqlite3_prepare_v2(db, checkProductSql, -1, &checkStmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to prepare product check: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    sqlite3_bind_int(checkStmt, 1, productId);

    rc = sqlite3_step(checkStmt);

    if (rc != SQLITE_ROW) {
        sqlite3_finalize(checkStmt);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("product not found"),
            k404NotFound
        ));
        return;
    }

    sqlite3_finalize(checkStmt);

    const char* insertSql =
        "INSERT INTO product_variants "
        "(product_id, variant_name, price, stock, image_url, enabled) "
        "VALUES (?, ?, ?, ?, ?, 1);";

    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to prepare variant insert: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    sqlite3_bind_int(stmt, 1, productId);
    sqlite3_bind_text(stmt, 2, variantName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, price);
    sqlite3_bind_int(stmt, 4, stock);
    sqlite3_bind_text(stmt, 5, imageUrl.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to create variant: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    long long variantId = sqlite3_last_insert_rowid(db);

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    Json::Value data;
    data["id"] = static_cast<Json::Int64>(variantId);
    data["productId"] = productId;
    data["variantName"] = variantName;
    data["price"] = price;
    data["stock"] = stock;
    data["imageUrl"] = imageUrl;
    data["enabled"] = true;

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = data;

    callback(makeJsonResponse(result));
}

void ProductController::updateVariant(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    int variantId)
{
    if (!isMerchantAuthorized(req)) {
        callback(makeUnauthorizedResponse());
        return;
    }

    if (variantId <= 0) {
        callback(makeJsonResponse(
            makeErrorResponse("invalid variant id"),
            k400BadRequest
        ));
        return;
    }

    auto json = req->getJsonObject();

    if (!json ||
        !json->isMember("variantName") ||
        !json->isMember("price") ||
        !json->isMember("stock")) {
        callback(makeJsonResponse(
            makeErrorResponse("invalid request body"),
            k400BadRequest
        ));
        return;
    }

    std::string variantName = (*json)["variantName"].asString();
    double price = (*json)["price"].asDouble();
    int stock = (*json)["stock"].asInt();

    std::string imageUrl = "";
    if (json->isMember("imageUrl") && (*json)["imageUrl"].isString()) {
        imageUrl = (*json)["imageUrl"].asString();
    }

    if (variantName.empty()) {
        callback(makeJsonResponse(
            makeErrorResponse("variantName cannot be empty"),
            k400BadRequest
        ));
        return;
    }

    if (price < 0) {
        callback(makeJsonResponse(
            makeErrorResponse("price cannot be negative"),
            k400BadRequest
        ));
        return;
    }

    if (stock < 0) {
        callback(makeJsonResponse(
            makeErrorResponse("stock cannot be negative"),
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
        "UPDATE product_variants "
        "SET variant_name = ?, price = ?, stock = ?, image_url = ? "
        "WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to prepare variant update: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    sqlite3_bind_text(stmt, 1, variantName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, price);
    sqlite3_bind_int(stmt, 3, stock);
    sqlite3_bind_text(stmt, 4, imageUrl.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, variantId);

    rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to update variant: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    sqlite3_finalize(stmt);

    int changedRows = sqlite3_changes(db);
    sqlite3_close(db);

    if (changedRows == 0) {
        callback(makeJsonResponse(
            makeErrorResponse("variant not found"),
            k404NotFound
        ));
        return;
    }

    Json::Value data;
    data["id"] = variantId;
    data["variantName"] = variantName;
    data["price"] = price;
    data["stock"] = stock;
    data["imageUrl"] = imageUrl;

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = data;

    callback(makeJsonResponse(result));
}

void ProductController::updateVariantEnabled(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    int variantId)
{
    if (!isMerchantAuthorized(req)) {
        callback(makeUnauthorizedResponse());
        return;
    }

    if (variantId <= 0) {
        callback(makeJsonResponse(
            makeErrorResponse("invalid variant id"),
            k400BadRequest
        ));
        return;
    }

    auto json = req->getJsonObject();

    if (!json || !json->isMember("enabled") || !(*json)["enabled"].isBool()) {
        callback(makeJsonResponse(
            makeErrorResponse("invalid request body"),
            k400BadRequest
        ));
        return;
    }

    bool enabled = (*json)["enabled"].asBool();

    sqlite3* db = nullptr;

    if (!openDatabase(&db)) {
        callback(makeJsonResponse(
            makeErrorResponse("failed to open database"),
            k500InternalServerError
        ));
        return;
    }

    const char* sql =
        "UPDATE product_variants "
        "SET enabled = ? "
        "WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to prepare variant enabled update: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
    sqlite3_bind_int(stmt, 2, variantId);

    rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        std::string dbError = sqlite3_errmsg(db);
        sqlite3_close(db);

        callback(makeJsonResponse(
            makeErrorResponse("failed to update variant enabled: " + dbError),
            k500InternalServerError
        ));
        return;
    }

    sqlite3_finalize(stmt);

    int changedRows = sqlite3_changes(db);
    sqlite3_close(db);

    if (changedRows == 0) {
        callback(makeJsonResponse(
            makeErrorResponse("variant not found"),
            k404NotFound
        ));
        return;
    }

    Json::Value data;
    data["id"] = variantId;
    data["enabled"] = enabled;

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = data;

    callback(makeJsonResponse(result));
}