#include "UploadController.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <random>
#include <sstream>
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

std::string getFileExtension(const std::string& filename) {
    auto pos = filename.find_last_of('.');

    if (pos == std::string::npos) {
        return ".jpg";
    }

    std::string ext = filename.substr(pos);

    if (ext == ".jpg" ||
        ext == ".jpeg" ||
        ext == ".png" ||
        ext == ".webp") {
        return ext;
    }

    return ".jpg";
}

std::string generateImageFileName(const std::string& originalName) {
    auto now = std::chrono::system_clock::now()
                   .time_since_epoch()
                   .count();

    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<unsigned long long> dist;

    std::ostringstream oss;
    oss << "product_"
        << now
        << "_"
        << std::hex
        << dist(gen)
        << getFileExtension(originalName);

    return oss.str();
}

}  // namespace

void UploadController::uploadProductImage(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    if (!isMerchantAuthorized(req)) {
        callback(makeUnauthorizedResponse());
        return;
    }

    MultiPartParser parser;
    int parseResult = parser.parse(req);

    if (parseResult != 0) {
        callback(makeJsonResponse(
            makeErrorResponse("failed to parse multipart form"),
            k400BadRequest
        ));
        return;
    }

    const auto& files = parser.getFiles();

    if (files.empty()) {
        callback(makeJsonResponse(
            makeErrorResponse("image file is required"),
            k400BadRequest
        ));
        return;
    }

    const auto& file = files[0];

    std::string originalName = file.getFileName();

    if (originalName.empty()) {
        originalName = "product.jpg";
    }

    std::filesystem::path uploadDir = "../uploads/products";

    if (!std::filesystem::exists(uploadDir)) {
        std::filesystem::create_directories(uploadDir);
    }

    std::string savedName = generateImageFileName(originalName);
    std::filesystem::path savedPath = uploadDir / savedName;

    try {
        file.saveAs(savedPath.string());
    } catch (const std::exception& e) {
        callback(makeJsonResponse(
            makeErrorResponse(std::string("failed to save image: ") + e.what()),
            k500InternalServerError
        ));
        return;
    }

    Json::Value data;
    data["filename"] = savedName;
    data["url"] = "/uploads/products/" + savedName;
    data["fullUrl"] = "http://localhost:5555/uploads/products/" + savedName;

    Json::Value result;
    result["code"] = 0;
    result["message"] = "success";
    result["data"] = data;

    callback(makeJsonResponse(result));
}

void UploadController::getProductImage(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& filename)
{
    if (filename.find("..") != std::string::npos ||
        filename.find("/") != std::string::npos ||
        filename.find("\\") != std::string::npos) {
        callback(makeJsonResponse(
            makeErrorResponse("invalid filename"),
            k400BadRequest
        ));
        return;
    }

    std::filesystem::path filePath = "../uploads/products";
    filePath /= filename;

    if (!std::filesystem::exists(filePath)) {
        callback(makeJsonResponse(
            makeErrorResponse("file not found"),
            k404NotFound
        ));
        return;
    }

    auto resp = HttpResponse::newFileResponse(filePath.string());
    callback(resp);
}