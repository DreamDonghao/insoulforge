/// @file ImageDescriptionService.cpp
/// @brief 图片与动图的视觉描述服务实现

#include <infrastructure/NumericTypes.hpp>

#include <media/ImageDescriptionService.hpp>

#include <gif_lib.h>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/http/HttpUtil.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <llm/LlmClient.hpp>
#include <media/ImageDescriptionStore.hpp>
#include <openssl/sha.h>
#include <png.h>

namespace insoulforge::ImageDescriptionService {
    namespace {
        constexpr size_t kMaxDownloadBytes = 8U * 1024U * 1024U;
        constexpr i32 kMaxGifDecodedFrames = 240;
        constexpr i32 kMaxGifSubmittedFrames = 16;
        constexpr i32 kMaxGifDimension = 1024;
        constexpr i32 kGifFrameMaxEdge = 512;
        constexpr i32 kPromptVersion = 2;

        struct DownloadedMedia {
            std::string bytes;
            std::string mimeType;
            bool isGif{false};
        };

        struct GifInput {
            const u8 *data{nullptr};
            size_t size{0};
            size_t offset{0};
        };

        struct GifFileDeleter {
            void operator()(GifFileType *file) const {
                if (!file)
                    return;
                int error = 0;
                DGifCloseFile(file, &error);
            }
        };

        /// @brief GIF 库的内存读取回调
        auto readGif(GifFileType *file, GifByteType *output, const int size) -> int {
            auto &input = *static_cast<GifInput *>(file->UserData);
            const size_t readable = std::min(static_cast<size_t>(size), input.size - input.offset);
            std::memcpy(output, input.data + input.offset, readable);
            input.offset += readable;
            return static_cast<int>(readable);
        }

        /// @brief 通过文件魔数判断媒体格式
        [[nodiscard]] auto detectMedia(const std::string &bytes) -> std::optional<std::pair<std::string, bool>> {
            if (bytes.size() >= 6 && (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a")))
                return std::pair{"image/gif", true};
            if (bytes.size() >= 8 && std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0)
                return std::pair{"image/png", false};
            if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
                static_cast<unsigned char>(bytes[1]) == 0xD8 && static_cast<unsigned char>(bytes[2]) == 0xFF)
                return std::pair{"image/jpeg", false};
            if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
                std::memcmp(bytes.data() + 8, "WEBP", 4) == 0)
                return std::pair{"image/webp", false};
            return std::nullopt;
        }

        [[nodiscard]] auto base64Encode(const std::string_view input) -> std::string {
            static constexpr std::string_view alphabet =
              "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string output;
            output.reserve((input.size() + 2) / 3 * 4);
            for (size_t i = 0; i < input.size(); i += 3) {
                const u32 value =
                  static_cast<u32>(static_cast<unsigned char>(input[i])) << 16U |
                  (i + 1 < input.size() ? static_cast<u32>(static_cast<unsigned char>(input[i + 1])) << 8U : 0U) |
                  (i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0U);
                output += alphabet[(value >> 18U) & 0x3FU];
                output += alphabet[(value >> 12U) & 0x3FU];
                output += i + 1 < input.size() ? alphabet[(value >> 6U) & 0x3FU] : '=';
                output += i + 2 < input.size() ? alphabet[value & 0x3FU] : '=';
            }
            return output;
        }

        [[nodiscard]] auto sha256(const std::string_view bytes) -> std::string {
            std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
            SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), digest.data());
            static constexpr std::string_view hex = "0123456789abcdef";
            std::string result;
            result.reserve(digest.size() * 2);
            for (const auto value: digest) {
                result += hex[value >> 4U];
                result += hex[value & 0x0FU];
            }
            return result;
        }

        /// @brief 下载 URL 的原始媒体字节，不写入 HTTP 跟踪日志以避免二进制内容进入内存日志。
        auto download(std::string sourceUrl, const u64 sessionId) -> drogon::Task<std::optional<DownloadedMedia>> {
            static const std::regex urlPattern(R"(^(https?://[^/]+)(/.*)?$)", std::regex::icase);
            std::smatch match;
            if (!std::regex_match(sourceUrl, match, urlPattern)) {
                Logger::warn(sessionId, "Media", fmt::format("拒绝非 HTTP 图片地址"));
                co_return std::nullopt;
            }
            try {
                const auto client = drogon::HttpClient::newHttpClient(match[1].str());
                const auto request = drogon::HttpRequest::newHttpRequest();
                request->setMethod(drogon::Get);
                request->setPath(match[2].matched ? match[2].str() : "/");
                const auto response = co_await client->sendRequestCoro(request, 30.0);
                if (!response || response->getStatusCode() < drogon::k200OK ||
                    response->getStatusCode() >= drogon::k300MultipleChoices) {
                    Logger::warn(sessionId, "Media",
                      fmt::format("下载失败: status={}", response ? static_cast<i32>(response->getStatusCode()) : 0));
                    co_return std::nullopt;
                }
                std::string bytes(response->body());
                if (bytes.empty() || bytes.size() > kMaxDownloadBytes) {
                    Logger::warn(sessionId, "Media", fmt::format("下载媒体大小无效: {} bytes", bytes.size()));
                    co_return std::nullopt;
                }
                const auto format = detectMedia(bytes);
                if (!format) {
                    Logger::warn(sessionId, "Media", fmt::format("不支持的媒体格式"));
                    co_return std::nullopt;
                }
                co_return DownloadedMedia{
                  .bytes = std::move(bytes), .mimeType = format->first, .isGif = format->second};
            } catch (const std::exception &error) {
                Logger::warn(sessionId, "Media", fmt::format("下载异常: {}", error.what()));
                co_return std::nullopt;
            }
        }

        [[nodiscard]] auto selectGifFrames(GifFileType &gif) -> std::vector<size_t> {
            const auto count = static_cast<size_t>(gif.ImageCount);
            if (count <= kMaxGifSubmittedFrames) {
                std::vector<size_t> all(count);
                std::iota(all.begin(), all.end(), 0);
                return all;
            }
            std::vector<i32> delays(count, 1);
            i64 totalDelay = 0;
            for (size_t i = 0; i < count; ++i) {
                GraphicsControlBlock control{};
                if (DGifSavedExtensionToGCB(&gif, static_cast<i32>(i), &control) == GIF_OK && control.DelayTime > 0)
                    delays[i] = control.DelayTime;
                totalDelay += delays[i];
            }
            std::set<size_t> selected{0, count - 1};
            for (i32 sample = 1; sample < kMaxGifSubmittedFrames - 1; ++sample) {
                const i64 target = totalDelay * sample / (kMaxGifSubmittedFrames - 1);
                i64 elapsed = 0;
                size_t index = count - 1;
                for (size_t frame = 0; frame < count; ++frame) {
                    elapsed += delays[frame];
                    if (elapsed >= target) {
                        index = frame;
                        break;
                    }
                }
                selected.insert(index);
            }
            for (size_t frame = 0; selected.size() < kMaxGifSubmittedFrames && frame < count; ++frame)
                selected.insert(frame);
            return {selected.begin(), selected.end()};
        }

        void writePng(png_structp png, png_bytep data, png_size_t length) {
            auto &output = *static_cast<std::string *>(png_get_io_ptr(png));
            output.append(reinterpret_cast<const char *>(data), length);
        }

        [[nodiscard]] auto encodePng(const std::vector<u8> &rgba, const i32 width, const i32 height)
          -> std::optional<std::string> {
            png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
            if (!png)
                return std::nullopt;
            png_infop info = png_create_info_struct(png);
            if (!info) {
                png_destroy_write_struct(&png, nullptr);
                return std::nullopt;
            }
            std::string output;
            if (setjmp(png_jmpbuf(png)) != 0) {
                png_destroy_write_struct(&png, &info);
                return std::nullopt;
            }
            png_set_write_fn(png, &output, writePng, nullptr);
            png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
              PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
            std::vector<png_bytep> rows(static_cast<size_t>(height));
            for (i32 row = 0; row < height; ++row)
                rows[static_cast<size_t>(row)] =
                  const_cast<png_bytep>(rgba.data() + static_cast<size_t>(row * width * 4));
            png_set_rows(png, info, rows.data());
            png_write_png(png, info, PNG_TRANSFORM_IDENTITY, nullptr);
            png_destroy_write_struct(&png, &info);
            return output;
        }

        [[nodiscard]] auto resizeRgba(const std::vector<u8> &input, const i32 width, const i32 height, i32 &outputWidth,
          i32 &outputHeight) -> std::vector<u8> {
            const i32 maxEdge = std::max(width, height);
            if (maxEdge <= kGifFrameMaxEdge) {
                outputWidth = width;
                outputHeight = height;
                return input;
            }
            const f64 scale = static_cast<f64>(kGifFrameMaxEdge) / maxEdge;
            outputWidth = std::max(1, static_cast<i32>(width * scale));
            outputHeight = std::max(1, static_cast<i32>(height * scale));
            std::vector<u8> output(static_cast<size_t>(outputWidth * outputHeight * 4));
            for (i32 y = 0; y < outputHeight; ++y) {
                for (i32 x = 0; x < outputWidth; ++x) {
                    const i32 sourceX = std::min(width - 1, static_cast<i32>(x / scale));
                    const i32 sourceY = std::min(height - 1, static_cast<i32>(y / scale));
                    std::copy_n(input.data() + static_cast<size_t>((sourceY * width + sourceX) * 4), 4,
                      output.data() + static_cast<size_t>((y * outputWidth + x) * 4));
                }
            }
            return output;
        }

        [[nodiscard]] auto extractGifFrames(const std::string &bytes, const u64 sessionId) -> std::vector<std::string> {
            GifInput input{.data = reinterpret_cast<const u8 *>(bytes.data()), .size = bytes.size()};
            i32 error = 0;
            std::unique_ptr<GifFileType, GifFileDeleter> gif(DGifOpen(&input, readGif, &error));
            if (!gif || gif->SWidth <= 0 || gif->SHeight <= 0 || gif->SWidth > kMaxGifDimension ||
                gif->SHeight > kMaxGifDimension || DGifSlurp(gif.get()) != GIF_OK || gif->ImageCount <= 0 ||
                gif->ImageCount > kMaxGifDecodedFrames) {
                Logger::warn(sessionId, "Media", fmt::format("GIF 解码失败或超过资源限制"));
                return {};
            }
            const i32 width = gif->SWidth;
            const i32 height = gif->SHeight;
            const auto selected = selectGifFrames(*gif);
            const std::set<size_t> selectedSet(selected.begin(), selected.end());
            std::vector<u8> canvas(static_cast<size_t>(width * height * 4), 0);
            std::vector<std::string> frames;
            for (i32 frameIndex = 0; frameIndex < gif->ImageCount; ++frameIndex) {
                const SavedImage &frame = gif->SavedImages[frameIndex];
                const GifImageDesc &desc = frame.ImageDesc;
                const ColorMapObject *colors = desc.ColorMap ? desc.ColorMap : gif->SColorMap;
                if (!colors)
                    return {};
                GraphicsControlBlock control{.TransparentColor = NO_TRANSPARENT_COLOR};
                DGifSavedExtensionToGCB(gif.get(), frameIndex, &control);
                const std::vector<u8> before = control.DisposalMode == DISPOSE_PREVIOUS ? canvas : std::vector<u8>{};
                for (i32 y = 0; y < desc.Height; ++y) {
                    for (i32 x = 0; x < desc.Width; ++x) {
                        const i32 pixelIndex = frame.RasterBits[y * desc.Width + x];
                        if (pixelIndex == control.TransparentColor || pixelIndex >= colors->ColorCount)
                            continue;
                        const i32 targetX = desc.Left + x;
                        const i32 targetY = desc.Top + y;
                        if (targetX < 0 || targetX >= width || targetY < 0 || targetY >= height)
                            continue;
                        const GifColorType color = colors->Colors[pixelIndex];
                        const size_t offset =
                          (static_cast<size_t>(targetY) * static_cast<size_t>(width) + static_cast<size_t>(targetX)) *
                          4;
                        canvas[offset] = color.Red;
                        canvas[offset + 1] = color.Green;
                        canvas[offset + 2] = color.Blue;
                        canvas[offset + 3] = 255;
                    }
                }
                if (selectedSet.contains(static_cast<size_t>(frameIndex))) {
                    i32 outputWidth = 0;
                    i32 outputHeight = 0;
                    const auto resized = resizeRgba(canvas, width, height, outputWidth, outputHeight);
                    if (const auto png = encodePng(resized, outputWidth, outputHeight))
                        frames.push_back("data:image/png;base64," + base64Encode(*png));
                }
                if (control.DisposalMode == DISPOSE_BACKGROUND) {
                    for (i32 y = 0; y < desc.Height; ++y) {
                        for (i32 x = 0; x < desc.Width; ++x) {
                            const i32 targetX = desc.Left + x;
                            const i32 targetY = desc.Top + y;
                            if (targetX >= 0 && targetX < width && targetY >= 0 && targetY < height)
                                std::fill_n(canvas.data() + static_cast<size_t>((targetY * width + targetX) * 4), 4, 0);
                        }
                    }
                } else if (control.DisposalMode == DISPOSE_PREVIOUS) {
                    canvas = before;
                }
            }
            return frames;
        }

        auto requestVision(std::vector<std::string> images, const bool isGif, const u64 sessionId)
          -> drogon::Task<std::optional<std::string>> {
            const auto &config = Config::instance();
            if (!LlmClient::isConfigured(config.image)) {
                Logger::debug(sessionId, "Media", fmt::format("图片识别模型未配置，跳过识别"));
                co_return std::nullopt;
            }
            json content = json::array();
            for (const auto &image: images)
                content.push_back({{"type", "image_url"}, {"image_url", {{"url", image}}}});
            content.push_back({{"type", "text"},
              {"text", isGif ? "这些图片按动图播放时间顺序排列。用不超过300字概括主体、动作变化、循环效果和情绪。"
                             : "用不超过300字描述这张图片"}});
            const json messages = json::array({{{"role", "user"}, {"content", std::move(content)}}});
            LLMApiConfig api = config.image;
            api.reasoningEffort.clear();
            const json body = LlmClient::buildChatRequestBody(api, config.imageParams, messages);
            const auto response = co_await HttpUtil::send(
              "[Image]", api.baseUrl, api.path, drogon::Post, body, api.apiKey, 90.0, sessionId);
            const auto parsed = response ? LlmClient::validChatJson(*response) : std::nullopt;
            if (!parsed) {
                co_return std::nullopt;
            }
            LlmClient::logUsage(*parsed, api.model, "image", sessionId);
            const std::string description =
              jsonToString(atOrNull(atOrNull((*parsed)["choices"][0], "message"), "content"));
            co_return description.empty() ? std::nullopt : std::optional{description};
        }
    } // namespace

    auto describe(std::string sourceUrl, const u64 sessionId) -> drogon::Task<std::optional<ImageDescriptionResult>> {
        const auto &config = Config::instance();
        if (!LlmClient::isConfigured(config.image)) {
            Logger::debug(sessionId, "Media", fmt::format("图片识别模型未配置，跳过识别"));
            co_return std::nullopt;
        }
        const auto media = co_await download(std::move(sourceUrl), sessionId);
        if (!media) {
            co_return std::nullopt;
        }
        const std::string hash = sha256(media->bytes);
        const std::string mediaType = media->isGif ? "gif" : "image";
        if (const auto cached = ImageDescriptionStore::find(hash, config.image.model, kPromptVersion)) {
            if (!cached->succeeded)
                co_return std::nullopt;
            co_return ImageDescriptionResult{.contentHash = hash,
              .mediaType = mediaType,
              .description = cached->description,
              .sampledFrameCount = cached->sampledFrameCount};
        }
        std::vector<std::string> images;
        if (media->isGif) {
            images = extractGifFrames(media->bytes, sessionId);
        } else {
            images.push_back("data:" + media->mimeType + ";base64," + base64Encode(media->bytes));
        }
        if (images.empty()) {
            ImageDescriptionStore::upsert(hash, config.image.model, kPromptVersion, mediaType, false, "", 0);
            co_return std::nullopt;
        }
        const auto description = co_await requestVision(images, media->isGif, sessionId);
        if (!description) {
            ImageDescriptionStore::upsert(hash, config.image.model, kPromptVersion, mediaType, false, "", 0);
            co_return std::nullopt;
        }
        ImageDescriptionStore::upsert(
          hash, config.image.model, kPromptVersion, mediaType, true, *description, static_cast<i32>(images.size()));
        co_return ImageDescriptionResult{.contentHash = hash,
          .mediaType = mediaType,
          .description = *description,
          .sampledFrameCount = static_cast<i32>(images.size())};
    }
} // namespace insoulforge::ImageDescriptionService
