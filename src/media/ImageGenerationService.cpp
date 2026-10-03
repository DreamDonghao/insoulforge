/// @file ImageGenerationService.cpp
/// @brief 调用图片生成接口并解析图片结果

#include <stdexcept>
#include <string_view>

#include <infrastructure/config/Config.hpp>
#include <infrastructure/http/HttpUtil.hpp>
#include <media/ImageGenerationService.hpp>

namespace insoulforge::ImageGenerationService {
    namespace {
        constexpr size_t kMaxImageBase64Length = 32 * 1024 * 1024;

        auto base64ImageCode(const std::string_view base64) -> std::optional<GeneratedImage> {
            if (base64.empty() || base64.size() > kMaxImageBase64Length || base64.size() % 4 != 0 ||
                base64.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") !=
                  std::string_view::npos) {
                return std::nullopt;
            }
            std::string source = "base64://" + std::string(base64);
            return GeneratedImage{.message = "[CQ:image,file=" + source + "]", .source = std::move(source)};
        }

        auto escapeCqValue(const std::string &value) -> std::string {
            std::string escaped;
            escaped.reserve(value.size());
            for (const char ch: value) {
                switch (ch) {
                    case '&':
                        escaped += "&amp;";
                        break;
                    case '[':
                        escaped += "&#91;";
                        break;
                    case ']':
                        escaped += "&#93;";
                        break;
                    case ',':
                        escaped += "&#44;";
                        break;
                    default:
                        escaped += ch;
                        break;
                }
            }
            return escaped;
        }
    } // namespace

    auto imageCode(const json &response) -> std::optional<GeneratedImage> {
        const json &data = atOrNull(response, "data");
        if (!data.is_array() || data.empty() || !data[0].is_object()) {
            return std::nullopt;
        }

        const auto url = getStr(data[0], "url");
        if (url.starts_with("https://") || url.starts_with("http://")) {
            return GeneratedImage{.message = "[CQ:image,file=" + escapeCqValue(url) + "]", .source = url};
        }

        if (url.starts_with("data:image/")) {
            const auto marker = url.find(";base64,");
            if (marker != std::string::npos) {
                return base64ImageCode(std::string_view(url).substr(marker + 8));
            }
        }

        if (!url.empty()) {
            if (const auto image = base64ImageCode(url)) {
                return image;
            }
        }

        const auto base64 = getStr(data[0], "b64_json");
        return base64ImageCode(base64);
    }

    auto generate(std::string prompt, std::string size, const u64 sessionId,
      std::optional<std::string> referenceDataUrl) -> drogon::Task<GeneratedImage> {
        const auto config = Config::instance().imageGeneration;
        if (config.apiKey.empty() || config.baseUrl.empty() || config.path.empty() || config.model.empty()) {
            throw std::runtime_error("图片生成接口尚未配置完整");
        }

        json request = {{"model", config.model}, {"prompt", std::move(prompt)}, {"size", std::move(size)}};
        if (referenceDataUrl) {
            request["input_references"] = json::array(
              {{{"type", "image_url"}, {"image_url", {{"url", std::move(*referenceDataUrl)}}}}});
        }
        // 响应可能包含数十 MB 的 b64_json，不保存在请求调试记录中。
        const auto response = co_await HttpUtil::send("ImageGeneration", config.baseUrl, config.path, drogon::Post,
          request, config.apiKey, 180.0, sessionId, false);
        if (!response || (*response)->getStatusCode() != drogon::k200OK) {
            throw std::runtime_error("图片生成接口请求失败");
        }

        json result;
        if (!tryParseJson((*response)->body(), result)) {
            throw std::runtime_error("图片生成接口返回了无效 JSON");
        }
        const auto image = imageCode(result);
        if (!image) {
            throw std::runtime_error("图片生成接口未返回可发送的图片");
        }
        co_return *image;
    }
} // namespace insoulforge::ImageGenerationService
