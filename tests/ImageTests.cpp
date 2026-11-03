/// @file ImageTests.cpp
/// @brief Cache retention, active handles, content integrity and session-visible IDs.
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>

#include <infrastructure/image/Image.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/image/ImageReferences.hpp>
#include <workflow/message/MessageRecord.hpp>

using insoulforge::Image;
using insoulforge::json;

int main(int argc, char **argv) {
    if (argc == 3) {
        const auto image = Image::find(argv[1]);
        return image && image->contentHash() == argv[2] && image->id() == argv[1] ? 0 : 1;
    }
    const auto executable = std::filesystem::absolute(argv[0]);
    const auto directory = std::filesystem::temp_directory_path() / ("insoulforge-image-" + drogon::utils::getUuid());
    const auto previous = std::filesystem::current_path();
    std::filesystem::create_directories(directory);
    std::filesystem::current_path(directory);
    int status = 0;
    try {
        const auto require = [](const bool condition, const char *message) {
            if (!condition)
                throw std::runtime_error(message);
        };
        const std::string encoded = "R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";
        require(Image::encode(std::string("\0\xff", 2)) == "AP8=", "binary Base64 preserves embedded NUL");
        std::string id;
        {
            const auto image = Image::fromBase64(encoded);
            require(image.has_value(), "GIF creation");
            id = image->id();
            require(id.size() == 12 && id.starts_with("img_"), "short image ID");
            require(image->contentHash() == Image::hash(*image->bytes()), "full content hash");
            require(Image::find(image->contentHash())->id() == id, "legacy full hash lookup");
            require(drogon::sync_wait(Image::load(id))->contentHash() == image->contentHash(), "load short ID");
            require(drogon::sync_wait(Image::load(image->contentHash()))->id() == id, "load legacy full hash");
            const auto command = "\"" + executable.string() + "\" " + id + " " + image->contentHash();
            require(std::system(command.c_str()) == 0, "mapping survives a fresh process");
            require(image->base64() == encoded, "Base64 roundtrip");
            require(image->dataUrl() == "data:image/gif;base64," + encoded, "actual MIME");
            const auto copy = *image;
            require(Image::fromBytes(*image->bytes())->id() == id, "content deduplication");
            require(Image::fromBase64("base64://" + encoded)->id() == id, "OneBot Base64");
            require(Image::fromBase64("data:image/png;base64," + encoded)->id() == id, "bytes determine MIME");
            const auto removed = Image::cleanup(std::chrono::seconds(0), 0);
            require(removed && *removed == 0 && copy.bytes().has_value(), "live handles survive age and size eviction");
            const auto record = insoulforge::MessageRecord::createAssistantGeneratedImageRecord("bot", 1, "image", id);
            const insoulforge::ToolCallContext context{
              .sessionId = insoulforge::SessionId::group("1"), .messageSnapshot = json::array({record})};
            const auto references =
              insoulforge::ImageReferences::resolve(json::array({id, json{{"image_id", id}}}), context);
            require(
              references && references->size() == 1 && references->front() == id, "visible generated IDs deduplicate");
            require(!insoulforge::ImageReferences::resolve(
                      json::array({id}), {.sessionId = insoulforge::SessionId::group("2")}),
              "cached image inaccessible without visible snapshot");
            const auto projected = insoulforge::MessageRecord::projectForAgent(record, false, false);
            require(projected.dump().contains(id) && !record.dump().contains("base64://"),
              "records and projections preserve IDs only");
            auto received = record;
            received["assets"]["images"][0]["recognition_status"] = "failed";
            require(insoulforge::ImageReferences::resolve(json::array({json{{"message_id", "1"}, {"image_index", 0}}}),
                      {.sessionId = insoulforge::SessionId::group("1"), .messageSnapshot = json::array({received})})
                      .has_value(),
              "legacy references resolve cached IDs without URLs");
        }
        require(!Image::find("../outside"), "ID path traversal rejected");
        require(!Image::fromBase64("====") && !Image::fromBase64("aGVsbG8="), "invalid image payload rejected");
        require(!Image::fromBytes("not an image"), "unsupported format rejected");
        require(!Image::cleanup(std::chrono::seconds(-1)), "negative retention rejected");
        const auto removed = Image::cleanup(std::chrono::seconds(0));
        require(removed && *removed == 1 && !Image::find(id), "released handles permit expiry");

        {
            std::ifstream mapping("cache/images/ids");
            const std::string contents((std::istreambuf_iterator<char>(mapping)), {});
            require(contents.find(id) == std::string::npos, "eviction removes persisted mapping");
        }
        // Occupy the preferred prefix to force the collision probe deterministically.
        {
            const auto bytes = drogon::utils::base64Decode(encoded);
            const auto hash = Image::hash(bytes);
            const auto occupied = "img_" + hash.substr(0, 8);
            const auto otherHash = Image::hash("other content");
            { std::ofstream mapping("cache/images/ids"); mapping << occupied << ' ' << otherHash << '\n'; }
            const auto image = Image::fromBytes(bytes);
            require(image && image->id() != occupied && image->contentHash() == hash, "collision gets distinct ID");
            require(Image::fromBytes(bytes)->id() == image->id(), "collision result deduplicates");
            require(Image::find(image->id())->contentHash() == hash, "collision lookup preserves content");
            const auto command = "\"" + executable.string() + "\" " + image->id() + " " + hash;
            require(std::system(command.c_str()) == 0, "collision mapping survives restart");
        }
        require(Image::cleanup(std::chrono::seconds(0)).value() == 1, "collision image eviction");
        // A legacy hash-named file acquires a short mapping on first lookup.
        {
            const auto bytes = drogon::utils::base64Decode(encoded);
            const auto hash = Image::hash(bytes);
            { std::ofstream output(std::filesystem::path("cache/images") / hash, std::ios::binary); output << bytes; }
            const auto image = Image::find(hash);
            require(image && image->id().starts_with("img_") && Image::find(image->id())->bytes() == bytes,
              "legacy file migration");
        }
        require(Image::cleanup(std::chrono::seconds(0)).value() == 1, "legacy image eviction");

        const json expiredRecord = {{"message_id", "9"},
          {"assets", {{"images", {{{"image_id", id}, {"source", {{"url", "https://example.com/legacy.png"}}}}}}}}};
        const insoulforge::ToolCallContext expiredContext{
          .sessionId = insoulforge::SessionId::group("1"), .messageSnapshot = json::array({expiredRecord})};
        require(!insoulforge::ImageReferences::resolve(json::array({id}), expiredContext), "explicit expired ID fails");
        const auto fallback = insoulforge::ImageReferences::resolve(
          json::array({json{{"message_id", "9"}, {"image_index", 0}}}), expiredContext);
        require(fallback && fallback->front() == "https://example.com/legacy.png",
          "legacy references fall back to URL after cache expiry");

        // Small capacity override verifies oldest-first eviction without allocating a GiB.
        auto first = Image::fromBase64(encoded);
        const auto firstPath = first->path();
        first = std::unexpected("release");
        auto bytes = drogon::utils::base64Decode(encoded);
        bytes[6] = 2; // distinct valid GIF header
        auto second = Image::fromBytes(bytes);
        const auto secondPath = second->path();
        second = std::unexpected("release");
        std::filesystem::last_write_time(
          firstPath, std::filesystem::file_time_type::clock::now() - std::chrono::hours(2));
        require(Image::cleanup(std::chrono::hours(72), bytes.size()).value() == 1 &&
                  !std::filesystem::exists(firstPath) && std::filesystem::exists(secondPath),
          "size limit evicts least recently used");
        std::filesystem::last_write_time(
          secondPath, std::filesystem::file_time_type::clock::now() - std::chrono::hours(73));
        require(Image::cleanup().value() == 1, "default three day expiry");

        // A write must enforce the real default 1 GiB limit; sparse files keep the test cheap.
        const auto old = std::filesystem::path("cache/images") / std::string(64, 'a');
        const auto newer = std::filesystem::path("cache/images") / std::string(64, 'b');
        for (const auto &path: {old, newer}) {
            std::ofstream(path).close();
            std::filesystem::resize_file(path, 600ULL * 1024 * 1024);
        }
        std::filesystem::last_write_time(old, std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
        auto written = Image::fromBase64(encoded);
        require(written && !std::filesystem::exists(old) && std::filesystem::exists(newer),
          "writes enforce default GiB limit");
        {
            std::ofstream corrupt(written->path(), std::ios::binary | std::ios::trunc);
            corrupt << "corrupted cache";
        }
        require(!written->bytes(), "corrupted cache rejected");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(directory);
    return status;
}
