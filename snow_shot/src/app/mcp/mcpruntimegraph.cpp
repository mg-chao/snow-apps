// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/app/mcp/mcpruntimegraph.h"
#include "snow_shot/app/mcp/mcpjobregistry.h"
#include "snow_shot/app/mcp/mcpmediaservice.h"
#include "snow_shot/presentation/screenshotcontroller.h"

#include <QThread>

namespace snow_shot::app::mcp {
class McpRuntimeGraph::Impl {
  public:
    explicit Impl(McpRuntimeGraph& owner) : q(owner) {}

    McpRuntimeLifecycle::StartOutcome start(Ports ports) {
        Q_ASSERT(QThread::currentThread() == q.thread());
        if (server && server->isRunning())
            return {true, {}};
        Q_ASSERT(!sources && !jobs && !application && !documents && !media && !session);
        Q_ASSERT(ports.screenshots && ports.groups);

        sources = std::make_unique<McpSourceResolver>(std::move(ports.sources), &q);
        sources->setSuspended(sourcesSuspended);
        jobs = std::make_unique<McpJobRegistry>(&q);
        auto artifactWriter = [this](quint64 owner, QByteArray bytes, const QString& mime) {
            return documents ? documents->storeArtifact(owner, std::move(bytes), mime)
                             : QJsonObject();
        };
        ports.application.jobs = jobs.get();
        ports.application.artifactWriter = artifactWriter;
        // The callback is installed before construction but invoked only after the complete
        // graph is available. Keeping its parent here avoids leaking graph-owned native work
        // into the containing application's longer lifetime.
        if (ports.selectedText) {
            ports.application.selectedText = [this, selectedText =
                                                        std::move(ports.selectedText)](auto done) {
                return selectedText(application.get(), std::move(done));
            };
        }
        application = std::make_unique<McpApplicationService>(std::move(ports.application), &q);
        ports.documents.jobs = jobs.get();
        ports.documents.cancelSource = [this](quint64 owner, const QString& requestId) {
            sources->cancel(owner, requestId);
        };
        ports.documents.resolveSource = [this](const ScreenshotMcpRequest& request, auto done,
                                               auto budget) {
            sources->resolve(request, std::move(done), std::move(budget));
        };
        documents = std::make_unique<McpDocumentService>(std::move(ports.documents), &q);
        media = std::make_unique<McpMediaService>(*ports.screenshots, *ports.groups, &q);
        jobs->setArtifactPublisher(artifactWriter);
        media->setArtifactWriter(artifactWriter);
        media->setFileArtifactWriter([this](quint64 owner, QString path, QString mime, auto done) {
            if (documents)
                documents->storeFileArtifact(owner, std::move(path), std::move(mime),
                                             std::move(done));
            else
                done(QJsonObject(), QStringLiteral("unavailable"));
        });
        QObject::connect(jobs.get(), &McpJobRegistry::changed, &q,
                         [this](quint64 owner, const QString& id) {
                             publishChanged(owner, QStringLiteral("snow-shot://jobs/") + id);
                         });
        QObject::connect(documents.get(), &McpDocumentService::changed, &q,
                         [this](quint64 owner, const QString& id) {
                             publishChanged(owner, QStringLiteral("snow-shot://documents/") + id);
                         });
        QObject::connect(documents.get(), &McpDocumentService::artifactChanged, &q,
                         [this](quint64 owner, const QString& id) {
                             publishChanged(owner, QStringLiteral("snow-shot://artifacts/") + id);
                         });

        session = std::make_unique<ScreenshotMcpSession>(std::move(ports.session), &q);
        QObject::connect(ports.screenshots, &ScreenshotController::mcpCapturePresented,
                         session.get(), &ScreenshotMcpSession::capturePresented);
        QObject::connect(ports.screenshots, &ScreenshotController::mcpCaptureTerminated,
                         session.get(), &ScreenshotMcpSession::captureTerminated);
        QObject::connect(ports.screenshots, &ScreenshotController::mcpCanvasChanged, session.get(),
                         &ScreenshotMcpSession::observe);
        if (!server) {
            server = std::make_unique<ScreenshotMcpServer>(&q);
            server->setRequestHandler([this](const auto& request, auto done) {
                if (application && application->handles(request.method))
                    application->request(request, std::move(done));
                else if (documents && documents->handles(request.method))
                    documents->request(request, std::move(done));
                else if (media && media->handles(request.method))
                    media->request(request, std::move(done));
                else
                    session->request(request, std::move(done));
            });
            server->setClientDisconnectedHandler([this](quint64 owner) {
                if (sources)
                    sources->disconnected(owner);
                if (session)
                    session->disconnected(owner);
                if (application)
                    application->disconnected(owner);
                if (documents)
                    documents->disconnected(owner);
                if (media)
                    media->disconnected(owner);
                if (jobs)
                    jobs->disconnected(owner);
            });
            server->setRequestCancellationHandler([this](quint64 owner, const QString& id) {
                bool accepted = sources && sources->cancel(owner, id);
                if (documents)
                    accepted |= documents->cancelRequest(owner, id);
                if (application)
                    accepted |= application->cancelRequest(owner, id);
                if (media)
                    accepted |= media->cancelRequest(owner, id);
                if (session)
                    accepted |= session->cancelRequest(owner, id);
                return accepted;
            });
        }
        QString error;
        const bool available = server->start(&error);
        return {available, std::move(error)};
    }

    void publishChanged(quint64 owner, const QString& uri) {
        if (server)
            server->publishEvent(owner,
                                 {{QStringLiteral("event"), QStringLiteral("resource_changed")},
                                  {QStringLiteral("uri"), uri}});
    }

    void stop() {
        Q_ASSERT(QThread::currentThread() == q.thread());
        // Close source admission before cancellations and native callbacks can reenter. The
        // actual workers, not cancellation delivery, determine when borrowed ports are free.
        if (sources)
            sources->stopAndDrain();
        if (session)
            session->shutdown();
        if (server)
            server->stop();
        if (documents)
            documents->shutdown();
        if (media)
            media->shutdown();
        if (application)
            application->shutdown();
        if (jobs)
            jobs->shutdown();

        session.reset();
        documents.reset();
        sources.reset();
        media.reset();
        application.reset();
        jobs.reset();
    }

    McpRuntimeGraph& q;
    std::unique_ptr<ScreenshotMcpServer> server;
    std::unique_ptr<ScreenshotMcpSession> session;
    std::unique_ptr<McpJobRegistry> jobs;
    std::unique_ptr<McpApplicationService> application;
    std::unique_ptr<McpDocumentService> documents;
    std::unique_ptr<McpMediaService> media;
    std::unique_ptr<McpSourceResolver> sources;
    bool sourcesSuspended = false;
};

McpRuntimeGraph::McpRuntimeGraph(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {}
McpRuntimeGraph::~McpRuntimeGraph() {
    m_impl->stop();
}
McpRuntimeLifecycle::StartOutcome McpRuntimeGraph::start(Ports ports) {
    return m_impl->start(std::move(ports));
}
void McpRuntimeGraph::drain(std::function<void()> done) {
    if (m_impl->server)
        m_impl->server->drainAndStop(std::move(done));
    else
        done();
}
void McpRuntimeGraph::stop() {
    m_impl->stop();
}
void McpRuntimeGraph::setSourcesSuspended(bool suspended) {
    m_impl->sourcesSuspended = suspended;
    if (m_impl->sources)
        m_impl->sources->setSuspended(suspended);
}
bool McpRuntimeGraph::busy() const {
    return (m_impl->sources && m_impl->sources->workerCount() != 0) ||
           (m_impl->jobs && m_impl->jobs->hasRunningJobs());
}
McpStartupError McpRuntimeGraph::startupError() const {
    return m_impl->server ? m_impl->server->startupError() : McpStartupError::None;
}
} // namespace snow_shot::app::mcp
