// Snow Shot integration for MicroTeX 0e3707f6. This file is compiled into the private dependency.
#ifndef SNOW_MICROTEX_PREVIEW_H
#define SNOW_MICROTEX_PREVIEW_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>

#include <QFile>
#include <tinyxml2.h>

namespace tex {

struct SnowPreviewLimit final : std::runtime_error {
    SnowPreviewLimit() : std::runtime_error("Formula preview limit exceeded") {}
};

struct SnowPreviewCanceled final : std::runtime_error {
    SnowPreviewCanceled() : std::runtime_error("Formula preview canceled") {}
};

struct SnowPreviewResource final : std::runtime_error {
    SnowPreviewResource() : std::runtime_error("Formula preview resource unavailable") {}
};

class SnowPreviewBudget final {
  public:
    explicit SnowPreviewBudget(const std::atomic_bool& canceled);
    ~SnowPreviewBudget();
    static void check();
    // Parsing checks amortize clock reads. Phase boundaries always check elapsed time.
    static void checkpoint();
    static void length(std::size_t value);
    static void replacement(std::size_t size, std::size_t removed, std::size_t inserted);
    static void matrix(int rows, int columns);
    class Depth final {
      public:
        Depth();
        ~Depth();
    };

  private:
    static thread_local SnowPreviewBudget* current;
    const std::atomic_bool& canceled;
    std::chrono::steady_clock::time_point deadline;
    std::size_t operations = 0;
    unsigned depth = 0;
};

struct SnowSessionState final {
    static void reset();
};

void snowRebuildCommands();
int snowCheckedDimension(double value);

// QFile supports Qt resources and Unicode paths on every platform. TinyXML's FILE API does not.
inline tinyxml2::XMLError snowLoadXml(tinyxml2::XMLDocument& document, const std::string& path) {
    QFile file(QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())));
    if (!file.open(QIODevice::ReadOnly))
        return tinyxml2::XML_ERROR_FILE_NOT_FOUND;
    const auto bytes = file.readAll();
    return document.Parse(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

} // namespace tex

#endif
