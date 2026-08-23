#include "snow_shot/presentation/screenshotqrrecognitionservice.h"

#include <opencv2/core.hpp>
#include <opencv2/objdetect.hpp>

#include <QCoreApplication>
#include <QDebug>
#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QSize>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <exception>
#include <utility>
#include <vector>

namespace {
constexpr qint64 kMaximumDetectorPixels = 1920LL * 1080LL;
constexpr int kMaximumDetectorEdge = 2560;

QString recognitionFailedMessage() {
    return QCoreApplication::translate("ScreenshotOcrController", "QR code recognition failed");
}

QSize boundedDetectorSize(const QSize& sourceSize) {
    if (sourceSize.isEmpty()) {
        return {};
    }

    const double width = sourceSize.width();
    const double height = sourceSize.height();
    const double pixelScale =
        std::sqrt(static_cast<double>(kMaximumDetectorPixels) / (width * height));
    const double edgeScale = static_cast<double>(kMaximumDetectorEdge) / std::max(width, height);
    const double scale = std::min({1.0, pixelScale, edgeScale});
    return QSize(std::max(1, static_cast<int>(std::floor(width * scale))),
                 std::max(1, static_cast<int>(std::floor(height * scale))));
}

ScreenshotQrRecognitionResult recognizeImage(QImage source,
                                             const std::atomic_bool& cancellation) {
    try {
        const QSize detectorSize = boundedDetectorSize(source.size());
        if (detectorSize.isEmpty() || cancellation.load(std::memory_order_relaxed)) {
            return {};
        }
        if (source.size() != detectorSize) {
            source = source.scaled(detectorSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        if (cancellation.load(std::memory_order_relaxed)) {
            return {};
        }
        if (source.format() != QImage::Format_Grayscale8) {
            source = source.convertToFormat(QImage::Format_Grayscale8);
        }
        if (source.isNull() || cancellation.load(std::memory_order_relaxed)) {
            return {};
        }

        cv::QRCodeDetector detector;
        cv::Mat input(source.height(), source.width(), CV_8UC1, source.bits(),
                      static_cast<std::size_t>(source.bytesPerLine()));
        std::vector<std::string> decoded;
        static_cast<void>(detector.detectAndDecodeMulti(input, decoded));

        ScreenshotQrRecognitionResult result;
        result.contents.reserve(static_cast<qsizetype>(decoded.size()));
        for (const std::string& value : decoded) {
            if (!value.empty()) {
                result.contents.push_back(
                    QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())));
            }
        }
        return result;
    } catch (const cv::Exception& exception) {
        qWarning() << "QR code recognition failed:" << exception.what();
    } catch (const std::exception& exception) {
        qWarning() << "QR code recognition failed:" << exception.what();
    } catch (...) {
        qWarning() << "QR code recognition failed with an unknown error";
    }
    return {{}, recognitionFailedMessage()};
}
} // namespace

class ScreenshotQrRecognitionService::Impl final {
  public:
    explicit Impl(ScreenshotQrRecognitionService* owner) : m_owner(owner) {}

    ~Impl() {
        shutdown();
    }

    RequestToken enqueue(RequestToken token, QImage image, QObject* receiver,
                         Completion completion) {
        auto request = std::make_shared<Request>();
        request->token = token;
        request->image = std::move(image);
        request->receiver = receiver;
        request->completion = std::move(completion);
        QPointer<ScreenshotQrRecognitionService> service(m_owner);
        request->receiverDestroyed = QObject::connect(
            receiver, &QObject::destroyed, m_owner, [service, token]() {
                if (service != nullptr) {
                    service->cancel(token);
                }
            });

        if (m_stopping) {
            QObject::disconnect(request->receiverDestroyed);
            return 0;
        }
        m_requests.insert(token, request);
        m_queue.push_back(request);
        startNext();
        return token;
    }

    void cancel(RequestToken token) {
        const auto request = m_requests.find(token);
        if (request == m_requests.end()) {
            return;
        }
        const RequestHandle job = request.value();
        job->cancellation->store(true, std::memory_order_release);
        m_requests.erase(request);
        QObject::disconnect(job->receiverDestroyed);
        if (job != m_runningRequest) {
            const auto queued = std::find(m_queue.begin(), m_queue.end(), job);
            if (queued != m_queue.end()) {
                m_queue.erase(queued);
            }
            startNext();
        }
    }

  private:
    using RequestToken = ScreenshotQrRecognitionPort::RequestToken;
    using Completion = ScreenshotQrRecognitionPort::Completion;

    struct Request {
        RequestToken token = 0;
        QImage image;
        QPointer<QObject> receiver;
        Completion completion;
        QMetaObject::Connection receiverDestroyed;
        std::shared_ptr<std::atomic_bool> cancellation =
            std::make_shared<std::atomic_bool>(false);
        ScreenshotQrRecognitionResult result;
    };
    using RequestHandle = std::shared_ptr<Request>;

    void startNext() {
        if (m_stopping || m_workerThread != nullptr) {
            return;
        }
        RequestHandle request;
        while (!m_queue.empty()) {
            request = std::move(m_queue.front());
            m_queue.pop_front();
            if (m_requests.contains(request->token) &&
                !request->cancellation->load(std::memory_order_acquire)) {
                break;
            }
            request.reset();
        }
        if (request == nullptr) {
            return;
        }

        QThread* const thread = QThread::create([request]() {
            if (!request->cancellation->load(std::memory_order_acquire)) {
                request->result =
                    recognizeImage(std::move(request->image), *request->cancellation);
            }
        });
        thread->setObjectName(QStringLiteral("ScreenshotQrWorker"));
        thread->setParent(m_owner);
        m_workerThread = thread;
        m_runningRequest = request;
        QPointer<ScreenshotQrRecognitionService> service(m_owner);
        QObject::connect(
            thread, &QThread::finished, m_owner,
            [this, service, request, thread]() mutable {
                if (service != nullptr) {
                    finish(request, thread);
                }
            },
            Qt::QueuedConnection);
        thread->start();
    }

    void finish(const RequestHandle& request, QThread* thread) {
        if (thread != m_workerThread) {
            return;
        }
        m_workerThread = nullptr;
        m_runningRequest.reset();
        thread->wait();
        delete thread;

        startNext();
        deliver(request);
    }

    void deliver(const RequestHandle& request) {
        const auto found = m_requests.find(request->token);
        if (found == m_requests.end()) {
            return;
        }
        m_requests.erase(found);
        QObject::disconnect(request->receiverDestroyed);
        if (request->cancellation->load(std::memory_order_acquire) ||
            request->receiver == nullptr || !request->completion) {
            return;
        }
        QPointer<QObject> receiver = request->receiver;
        Completion completion = std::move(request->completion);
        ScreenshotQrRecognitionResult result = std::move(request->result);
        if (receiver != nullptr && completion) {
            completion(std::move(result));
        }
    }

    void shutdown() {
        if (m_stopping) {
            return;
        }
        m_stopping = true;
        for (auto request = m_requests.begin(); request != m_requests.end(); ++request) {
            request.value()->cancellation->store(true, std::memory_order_release);
            QObject::disconnect(request.value()->receiverDestroyed);
        }
        m_requests.clear();
        m_queue.clear();
        QThread* const thread = m_workerThread;
        m_workerThread = nullptr;
        m_runningRequest.reset();
        if (thread != nullptr) {
            thread->wait();
            delete thread;
        }
    }

    ScreenshotQrRecognitionService* m_owner = nullptr;
    QHash<RequestToken, RequestHandle> m_requests;
    std::deque<RequestHandle> m_queue;
    QThread* m_workerThread = nullptr;
    RequestHandle m_runningRequest;
    bool m_stopping = false;
};

ScreenshotQrRecognitionService::ScreenshotQrRecognitionService(QObject* parent)
    : ScreenshotQrRecognitionPort(parent), m_impl(std::make_unique<Impl>(this)) {}

ScreenshotQrRecognitionService::~ScreenshotQrRecognitionService() = default;

ScreenshotQrRecognitionPort::RequestToken
ScreenshotQrRecognitionService::recognize(QImage image, QObject* receiver, Completion completion) {
    if (image.isNull() || receiver == nullptr || !completion || m_impl == nullptr) {
        return 0;
    }
    do {
        ++m_nextToken;
    } while (m_nextToken == 0);
    return m_impl->enqueue(m_nextToken, std::move(image), receiver, std::move(completion));
}

void ScreenshotQrRecognitionService::cancel(RequestToken token) {
    if (m_impl != nullptr && token != 0) {
        m_impl->cancel(token);
    }
}
