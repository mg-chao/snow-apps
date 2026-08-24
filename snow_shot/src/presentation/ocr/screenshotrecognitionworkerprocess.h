#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONWORKERPROCESS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONWORKERPROCESS_H

#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>

namespace snow_shot::presentation {

enum class RecognitionWorkerIoResult { Complete, Cancelled, Failed };

class RecognitionWorkerProcess final {
  public:
    static std::unique_ptr<RecognitionWorkerProcess> start(const QString& executable,
                                                           const QString& argument);

    ~RecognitionWorkerProcess();

    RecognitionWorkerProcess(const RecognitionWorkerProcess&) = delete;
    RecognitionWorkerProcess& operator=(const RecognitionWorkerProcess&) = delete;

    RecognitionWorkerIoResult writeExact(const std::atomic_bool& cancelled, const void* source,
                                         std::uint64_t length);
    RecognitionWorkerIoResult readExact(const std::atomic_bool& cancelled, void* destination,
                                        std::uint64_t length);
    void closeInput();
    RecognitionWorkerIoResult waitForFinished(const std::atomic_bool& cancelled);
    [[nodiscard]] bool exitedSuccessfullyWithoutOutput() const;
    [[nodiscard]] QString errorString() const;
    void terminate();

  private:
    class Impl;
    explicit RecognitionWorkerProcess(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONWORKERPROCESS_H
