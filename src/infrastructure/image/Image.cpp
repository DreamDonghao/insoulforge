/// @file Image.cpp
/// @brief 图片缓存的获取、内容寻址、读取和清理
#include <algorithm>
#include <array>
#include <fstream>
#include <cstdint>
#include <stdexcept>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <cstring>
#include <drogon/HttpClient.h>
#include <drogon/utils/Utilities.h>
#include <openssl/sha.h>
#include <regex>

#include <infrastructure/image/Image.hpp>

namespace insoulforge {
    namespace {
        constexpr size_t kMaxBytes = 24 * 1024 * 1024;
        const std::filesystem::path kDirectory{"cache/images"};
        std::recursive_mutex cacheMutex;

        auto validId(const std::string_view id) -> bool {
            return id.size() == 64 && id.find_first_not_of("0123456789abcdef") == std::string_view::npos;
        }

        auto validShortId(std::string_view id) -> bool {
            return id.size() == 12 && id.starts_with("img_") &&
              id.substr(4).find_first_not_of("0123456789abcdef") == std::string_view::npos;
        }

        using Mapping = std::unordered_map<std::string, std::string>;
        // Read on demand: disk is the source of truth, including after a restart.
        auto readMapping() -> Mapping {
            Mapping mapping;
            const auto path = kDirectory / "ids";
            if (!std::filesystem::exists(path))
                return mapping;
            std::ifstream input(path);
            if (!input)
                throw std::runtime_error("无法读取图片 ID 映射");
            std::string id, hash;
            while (input >> id) {
                if (!(input >> hash) || !validShortId(id) || !validId(hash) || !mapping.emplace(id, hash).second)
                    throw std::runtime_error("图片 ID 映射已损坏");
            }
            if (!input.eof())
                throw std::runtime_error("读取图片 ID 映射失败");
            return mapping;
        }

        void writeMapping(const Mapping &mapping) {
            std::filesystem::create_directories(kDirectory);
            const auto temporary = kDirectory / "ids.tmp";
            std::ofstream output(temporary, std::ios::trunc);
            for (const auto &[id, hash]: mapping)
                output << id << ' ' << hash << '\n';
            output.close();
            if (!output)
                throw std::runtime_error("写入图片 ID 映射失败");
            std::filesystem::rename(temporary, kDirectory / "ids");
        }

        auto shortId(const std::string &hash) -> std::string {
            auto mapping = readMapping();
            for (const auto &[id, content]: mapping)
                if (content == hash)
                    return id;
            // Probe alternate eight-hex candidates rather than aliasing colliding content.
            for (uint64_t attempt = 0; attempt <= 0xffffffffULL; ++attempt) {
                const auto id = "img_" + (attempt == 0 ? hash : Image::hash(hash + ":" + std::to_string(attempt))).substr(0, 8);
                if (mapping.contains(id))
                    continue;
                mapping.emplace(id, hash);
                writeMapping(mapping);
                return id;
            }
            throw std::runtime_error("图片 ID 空间已满");
        }

        std::unordered_map<std::string, std::weak_ptr<const std::string>> handles;
    } // namespace

    Image::Image(std::string id, std::string contentHash)
      : m_id(std::move(id)), m_contentHash(std::move(contentHash)) {
        std::lock_guard lock(cacheMutex);
        auto &handle = handles[m_contentHash];
        m_handle = handle.lock();
        if (!m_handle) {
            m_handle = std::make_shared<const std::string>(m_contentHash);
            handle = m_handle;
        }
    }

    auto Image::mimeType(std::string_view bytes) -> std::optional<std::string_view> {
        if (bytes.size() >= 14 && (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a")) && bytes.back() == ';')
            return "image/gif";
        if (bytes.size() >= 45 && bytes.starts_with("\x89PNG\r\n\x1a\n") &&
            std::memcmp(bytes.data() + bytes.size() - 12, "\0\0\0\0IEND", 8) == 0)
            return "image/png";
        if (bytes.size() >= 4 && bytes.starts_with("\xff\xd8\xff") &&
            static_cast<unsigned char>(bytes[bytes.size() - 2]) == 255 &&
            static_cast<unsigned char>(bytes.back()) == 217)
            return "image/jpeg";
        if (bytes.size() >= 20 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP") {
            size_t length = 0;
            for (size_t i = 0; i < 4; ++i)
                length |= size_t(static_cast<unsigned char>(bytes[4 + i])) << (8 * i);
            if (length == bytes.size() - 8)
                return "image/webp";
        }
        return std::nullopt;
    }

    auto Image::encode(std::string_view bytes) -> std::string { return drogon::utils::base64Encode(bytes); }

    auto Image::hash(std::string_view bytes) -> std::string {
        std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
        SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), digest.data());
        std::string id;
        for (const auto byte: digest) {
            id += "0123456789abcdef"[byte >> 4];
            id += "0123456789abcdef"[byte & 15];
        }
        return id;
    }

    auto Image::load(std::string source) -> drogon::Task<std::expected<Image, std::string>> {
        if (validId(source) || validShortId(source))
            co_return find(source);
        if (source.starts_with("http://") || source.starts_with("https://"))
            co_return co_await fromUrl(std::move(source));
        co_return fromBase64(source);
    }

    auto Image::path() const -> std::filesystem::path { return std::filesystem::absolute(kDirectory / m_contentHash); }

    auto Image::fromBytes(const std::string_view bytes) -> std::expected<Image, std::string> {
        if (bytes.empty() || bytes.size() > kMaxBytes)
            return std::unexpected("图片不能为空且不得超过 24 MiB");
        if (!mimeType(bytes))
            return std::unexpected("不支持或不完整的图片格式");
        if (auto cleaned = cleanup(); !cleaned)
            return std::unexpected(cleaned.error());
        std::lock_guard lock(cacheMutex);
        try {
            std::filesystem::create_directories(kDirectory);
            const auto contentHash = hash(bytes);
            Image image{shortId(contentHash), contentHash};
            const auto target = image.path();
            if (!std::filesystem::is_symlink(target) && std::filesystem::is_regular_file(target)) {
                std::filesystem::last_write_time(target, std::filesystem::file_time_type::clock::now());
                return image;
            }
            const auto temporary = target.string() + ".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                output.close();
                if (!output) {
                    std::error_code ignored;
                    std::filesystem::remove(temporary, ignored);
                    return std::unexpected("写入图片缓存失败");
                }
            }
            std::filesystem::rename(temporary, target);
            if (auto cleaned = cleanup(); !cleaned)
                return std::unexpected(cleaned.error());
            return image;
        } catch (const std::exception &error) {
            return std::unexpected(std::string("保存图片缓存失败: ") + error.what());
        }
    }

    auto Image::fromBase64(std::string_view encoded) -> std::expected<Image, std::string> {
        if (encoded.starts_with("base64://"))
            encoded.remove_prefix(9);
        else if (encoded.starts_with("data:image/")) {
            const auto marker = encoded.find(";base64,");
            if (marker == std::string_view::npos)
                return std::unexpected("无效的图片 Data URL");
            encoded.remove_prefix(marker + 8);
        }
        if (const auto padding = encoded.ends_with("==")  ? 2U
                                 : encoded.ends_with('=') ? 1U
                                                          : 0U;
          encoded.empty() || encoded.size() > (kMaxBytes + 2) / 3 * 4 || encoded.size() % 4 != 0 ||
          encoded.substr(0, encoded.size() - padding)
              .find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") !=
            std::string_view::npos)
            return std::unexpected("无效或过大的 Base64 图片");
        const auto decoded = drogon::utils::base64Decode(std::string(encoded));
        if (drogon::utils::base64Encode(decoded) != encoded)
            return std::unexpected("无效的 Base64 编码");
        return fromBytes(decoded);
    }

    auto Image::fromUrl(const std::string url) -> drogon::Task<std::expected<Image, std::string>> {
        static const std::regex pattern(R"(^(https?://[^/?#]+)([^#]*)$)", std::regex::icase);
        std::smatch match;
        if (!std::regex_match(url, match, pattern))
            co_return std::unexpected("图片地址必须是 HTTP/HTTPS URL");
        try {
            const auto client = drogon::HttpClient::newHttpClient(match[1].str());
            const auto request = drogon::HttpRequest::newHttpRequest();
            auto resource = match[2].str();
            if (resource.empty() || resource.front() == '?')
                resource.insert(0, "/");
            request->setPath(resource);
            const auto response = co_await client->sendRequestCoro(request, 30.0);
            if (!response || response->getStatusCode() != drogon::k200OK)
                co_return std::unexpected("图片下载失败或服务器未返回 HTTP 200");
            co_return fromBytes(response->body());
        } catch (const std::exception &error) {
            co_return std::unexpected(std::string("图片下载失败: ") + error.what());
        }
    }

    auto Image::find(const std::string_view id) -> std::expected<Image, std::string> {
        if (!validId(id) && !validShortId(id))
            return std::unexpected("无效的图片 ID");
        std::lock_guard lock(cacheMutex);
        try {
            std::string contentHash(id);
            if (validShortId(id)) {
                const auto mapping = readMapping();
                const auto it = mapping.find(std::string(id));
                if (it == mapping.end())
                    return std::unexpected("图片缓存已过期或不存在");
                contentHash = it->second;
            }
            // Verify legacy files before assigning a new short ID.
            Image image{std::string(id), contentHash};
            if (auto result = image.bytes(); !result)
                return std::unexpected(result.error());
            image.m_id = shortId(contentHash);
            return image;
        } catch (const std::exception &error) {
            return std::unexpected(std::string("读取图片 ID 映射失败: ") + error.what());
        }
    }

    auto Image::bytes() const -> std::expected<std::string, std::string> {
        std::lock_guard lock(cacheMutex);
        try {
            if (std::filesystem::is_symlink(path()))
                return std::unexpected("图片缓存不能是符号链接");
            std::ifstream input(path(), std::ios::binary | std::ios::ate);
            if (!input)
                return std::unexpected("图片缓存已过期或不存在");
            const auto size = input.tellg();
            if (size <= 0 || size > static_cast<std::streamoff>(kMaxBytes))
                return std::unexpected("图片缓存大小无效");
            std::string result(size, '\0');
            input.seekg(0);
            if (!input.read(result.data(), size))
                return std::unexpected("读取图片缓存失败");
            if (!mimeType(result) || hash(result) != m_contentHash)
                return std::unexpected("图片缓存内容已损坏");
            std::filesystem::last_write_time(path(), std::filesystem::file_time_type::clock::now());
            return result;
        } catch (const std::exception &error) {
            return std::unexpected(std::string("读取图片缓存失败: ") + error.what());
        }
    }

    auto Image::base64() const -> std::expected<std::string, std::string> {
        auto content = bytes();
        if (!content)
            return std::unexpected(content.error());
        return drogon::utils::base64Encode(*content);
    }

    auto Image::dataUrl() const -> std::expected<std::string, std::string> {
        auto content = bytes();
        if (!content)
            return std::unexpected(content.error());
        const auto mime = mimeType(*content);
        if (!mime)
            return std::unexpected("图片缓存格式无效");
        return "data:" + std::string(*mime) + ";base64," + drogon::utils::base64Encode(*content);
    }

    auto Image::cleanup(const std::chrono::seconds maxAge, const size_t maxBytes)
      -> std::expected<size_t, std::string> {
        if (maxAge.count() < 0)
            return std::unexpected("缓存保留时间不能为负数");
        std::lock_guard lock(cacheMutex);
        try {
            if (!std::filesystem::exists(kDirectory))
                return 0;
            auto mapping = readMapping();
            bool mappingChanged = false;
            const auto cutoff = std::filesystem::file_time_type::clock::now() - maxAge;
            size_t removed = 0;
            size_t total = 0;
            std::vector<std::filesystem::directory_entry> entries;
            for (const auto &entry: std::filesystem::directory_iterator(kDirectory)) {
                if (entry.is_symlink() || !entry.is_regular_file() || !validId(entry.path().filename().string()))
                    continue;
                total += entry.file_size();
                entries.push_back(entry);
            }
            std::ranges::sort(entries, {}, [](const auto &entry) { return entry.last_write_time(); });
            for (const auto &entry: entries) {
                const auto id = entry.path().filename().string();
                if (const auto it = handles.find(id); it != handles.end() && !it->second.expired())
                    continue;
                if (entry.last_write_time() <= cutoff || total > maxBytes) {
                    const auto size = entry.file_size();
                    if (std::filesystem::remove(entry.path())) {
                        total -= size;
                        ++removed;
                        mappingChanged |= std::erase_if(mapping, [&](const auto &item) { return item.second == id; }) != 0;
                    }
                }
            }
            if (mappingChanged)
                writeMapping(mapping);
            std::erase_if(handles, [](const auto &entry) { return entry.second.expired(); });
            return removed;
        } catch (const std::exception &error) {
            return std::unexpected(std::string("清理图片缓存失败: ") + error.what());
        }
    }
} // namespace insoulforge
