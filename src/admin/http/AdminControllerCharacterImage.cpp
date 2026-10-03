/// @file AdminControllerCharacterImage.cpp
/// @brief 管理后台角色形象图接口

#include <drogon/MultiPart.h>

#include <admin/http/AdminController.hpp>
#include <admin/http/AdminResponse.hpp>
#include <media/CharacterImageStore.hpp>

using namespace insoulforge;
using namespace drogon;

auto AdminController::getCharacterImage(HttpRequestPtr, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto image = CharacterImageStore::load();
    if (!image) {
        auto response = jsonResponse(AdminResponse::failJson("尚未上传角色形象图"));
        response->setStatusCode(k404NotFound);
        callback(response);
        co_return;
    }
    auto response = HttpResponse::newHttpResponse();
    response->setContentTypeString(image->mimeType);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("X-Content-Type-Options", "nosniff");
    response->setBody(image->bytes);
    callback(response);
    co_return;
}

auto AdminController::saveCharacterImage(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const -> Task<> {
    if (req->body().size() > CharacterImageStore::kMaxImageBytes + 1024U * 1024U) {
        auto response = jsonResponse(AdminResponse::failJson("图片不得超过 8 MiB"));
        response->setStatusCode(k413RequestEntityTooLarge);
        callback(response);
        co_return;
    }
    MultiPartParser parser;
    if (parser.parse(req) != 0 || parser.getFiles().size() != 1 || parser.getFiles()[0].getItemName() != "image") {
        auto response = jsonResponse(AdminResponse::failJson("请以 image 字段上传一张图片"));
        response->setStatusCode(k400BadRequest);
        callback(response);
        co_return;
    }
    if (const auto error = CharacterImageStore::save(parser.getFiles()[0].fileContent())) {
        auto response = jsonResponse(AdminResponse::failJson(*error));
        response->setStatusCode(k400BadRequest);
        callback(response);
        co_return;
    }
    callback(jsonResponse(AdminResponse::okJson("角色形象图已保存")));
    co_return;
}

auto AdminController::deleteCharacterImage(HttpRequestPtr, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    if (!CharacterImageStore::remove()) {
        auto response = jsonResponse(AdminResponse::failJson("删除角色形象图失败"));
        response->setStatusCode(k500InternalServerError);
        callback(response);
        co_return;
    }
    callback(jsonResponse(AdminResponse::okJson("角色形象图已删除")));
    co_return;
}
