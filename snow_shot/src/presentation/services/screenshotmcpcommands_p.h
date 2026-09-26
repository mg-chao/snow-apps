// Private implementation fragment: ScreenshotController::Impl is defined in
// screenshotcontroller.cpp.
#include "snow_shot/app/mcp/screenshotmcpsession.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include <QSaveFile>
#include <QMimeData>
#include <QClipboard>

#include "snow_shot/app/mcp/mcpstylepatch.h"

void ScreenshotController::mcpCancelCommand() {
    auto& s = *m_impl;
    ++s.m_mcpCommandGeneration;
    if (s.m_mcpPoll) {
        s.m_mcpPoll->stop();
        s.m_mcpPoll->deleteLater();
        s.m_mcpPoll = nullptr;
    }
    s.m_mcpFileJob.cancel();
    s.m_mcpCompletion = {};
    if (s.m_scrollingCaptureController)
        s.m_scrollingCaptureController->cancelScrollOnce();
    if (s.m_ocrController)
        s.m_ocrController->cancelWorkflow();
    if (s.m_autoFilterController && s.m_autoFilterController->detecting())
        s.m_autoFilterController->resetSession();
}
void ScreenshotController::mcpDetached() {
    if (m_impl->m_scrollingCaptureController)
        m_impl->m_scrollingCaptureController->setAutoScroll(false);
}
void ScreenshotController::mcpCommand(const QString& method, const QJsonObject& params,
                                      McpCompletion completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto& s = *m_impl;
    const auto sourceEpoch = s.m_captureEpoch;
    if (s.m_captureState.captureInProgress || s.m_interaction.inactive() ||
        !s.m_selection.hasPixelSelection()) {
        completion({}, QStringLiteral("capture_not_ready"));
        return;
    }
    SnowCanvasWidget* canvas = nullptr;
    s.m_displaySession.forEachActiveOverlay(
        [&](qsizetype, const CapturedDisplayModel&, ScreenshotOverlayWindow* overlay) {
            if (!canvas && overlay)
                canvas = overlay->canvas();
        });
    const auto fail = [&] { completion({}, QStringLiteral("invalid_parameters")); };
    const auto success = [&] {
        emit mcpCanvasChanged();
        completion({}, {});
    };
    const auto action = params.value(QStringLiteral("action")).toString();
    const bool scrolling =
        s.m_scrollingCaptureController && s.m_scrollingCaptureController->active();
    if (scrolling && method != QStringLiteral("screenshot_scrolling") &&
        method != QStringLiteral("screenshot_scroll_once")) {
        completion({}, QStringLiteral("invalid_state"));
        return;
    }
    if (method == QStringLiteral("screenshot_set_selection_style")) {
        auto candidate = s.m_selection;
        for (const auto& key : {QStringLiteral("corner_radius"), QStringLiteral("shadow_width")}) {
            if (!params.contains(key))
                continue;
            const auto value = params.value(key);
            if (!value.isDouble() || value.toDouble() < 0 || value.toDouble() > 4096 ||
                std::floor(value.toDouble()) != value.toDouble()) {
                fail();
                return;
            }
        }
        if (params.contains(QStringLiteral("corner_radius")))
            static_cast<void>(
                candidate.setCornerRadius(params.value(QStringLiteral("corner_radius")).toInt()));
        if (params.contains(QStringLiteral("shadow_width")))
            static_cast<void>(
                candidate.setShadowWidth(params.value(QStringLiteral("shadow_width")).toInt()));
        if (params.contains(QStringLiteral("shadow_color"))) {
            const auto color = params.value(QStringLiteral("shadow_color")).toArray();
            if (color.size() != 4) {
                fail();
                return;
            }
            for (auto v : color)
                if (!v.isDouble() || v.toDouble() < 0 || v.toDouble() > 255 ||
                    std::floor(v.toDouble()) != v.toDouble()) {
                    fail();
                    return;
                }
            candidate.setShadowColor(
                QColor(color[0].toInt(), color[1].toInt(), color[2].toInt(), color[3].toInt()));
        }
        if (params.contains(QStringLiteral("aspect_ratio_locked"))) {
            if (!params.value(QStringLiteral("aspect_ratio_locked")).isBool()) {
                fail();
                return;
            }
            static_cast<void>(candidate.setAspectRatioLockEnabled(
                params.value(QStringLiteral("aspect_ratio_locked")).toBool(),
                snow_shot::presentation::kScreenshotSelectionMinimumSize));
        }
        s.m_selection = candidate;
        s.m_presentationServices->updateOverlayState();
        success();
        return;
    }
    if (method == QStringLiteral("screenshot_set_tool_style")) {
        if (!canvas || !snow_shot::app::mcp::mcpStylePatch(s, *canvas, params)) {
            fail();
            return;
        }
        success();
        return;
    }
    if (method == QStringLiteral("screenshot_edit_elements")) {
        if (!canvas) {
            fail();
            return;
        }
        bool ok = false;
        if (action == QStringLiteral("select")) {
            QJsonObject result;
            QString error;
            const QJsonObject operation{{QStringLiteral("type"), QStringLiteral("select")},
                                        {QStringLiteral("id"), params.value(QStringLiteral("id"))}};
            const QJsonObject batch{{QStringLiteral("version"), 1},
                                    {QStringLiteral("operations"), QJsonArray{operation}}};
            ok = mcpApplyAnnotations(QJsonDocument(batch).toJson(QJsonDocument::Compact), &result,
                                     &error);
        } else if (action == QStringLiteral("duplicate"))
            ok = canvas->duplicateSelected();
        else if (action == QStringLiteral("delete"))
            ok = canvas->deleteSelected();
        else if (action == QStringLiteral("delete_all"))
            ok = canvas->deleteAllElements();
        else if (action == QStringLiteral("create_serial_text"))
            ok = canvas->createSerialNumberText();
        else if (action == QStringLiteral("adjust_serial_numbers"))
            ok = canvas->adjustSelectedSerialNumbers(params.value(QStringLiteral("delta")).toInt());
        else if (action == QStringLiteral("opacity")) {
            const double opacity = params.value(QStringLiteral("opacity")).toDouble(-1);
            ok = std::isfinite(opacity) && opacity >= 0 && opacity <= 1 &&
                 canvas->setSelectedOpacity(opacity);
        } else if (action == QStringLiteral("order")) {
            const QStringList values{
                QStringLiteral("send_to_back"), QStringLiteral("send_backward"),
                QStringLiteral("bring_forward"), QStringLiteral("bring_to_front")};
            const int value = values.indexOf(params.value(QStringLiteral("order")).toString());
            ok =
                value >= 0 && canvas->reorderSelected(static_cast<SnowCanvasSelectionOrder>(value));
        } else if (action == QStringLiteral("align")) {
            const QStringList values{QStringLiteral("left"),
                                     QStringLiteral("center_horizontally"),
                                     QStringLiteral("right"),
                                     QStringLiteral("top"),
                                     QStringLiteral("center_vertically"),
                                     QStringLiteral("bottom"),
                                     QStringLiteral("distribute_horizontally"),
                                     QStringLiteral("distribute_vertically")};
            const int value = values.indexOf(params.value(QStringLiteral("alignment")).toString());
            ok = value >= 0 &&
                 canvas->alignSelected(static_cast<SnowCanvasSelectionAlignment>(value));
        }
        if (!ok) {
            completion({}, QStringLiteral("action_unavailable"));
            return;
        }
        success();
        return;
    }
    if (method == QStringLiteral("screenshot_draw_template")) {
        if (action == QStringLiteral("export")) {
            const auto payload = s.m_canvasRuntime.serializeSelectedDrawTemplate();
            if (payload.isEmpty()) {
                completion({}, QStringLiteral("action_unavailable"));
                return;
            }
            completion({{QStringLiteral("payload"), QString::fromUtf8(payload)}}, {});
            return;
        }
        const auto payload = params.value(QStringLiteral("payload")).toString().toUtf8();
        if (action != QStringLiteral("insert") || payload.size() > 1024 * 1024 || !canvas ||
            !canvas->insertDrawTemplate(payload, s.m_selection.normalizedSelection().center())) {
            fail();
            return;
        }
        success();
        return;
    }
    if (method == QStringLiteral("screenshot_scroll_once")) {
        if (!scrolling) {
            completion({}, QStringLiteral("scrolling_not_ready"));
            return;
        }
        s.m_scrollingCaptureController->scrollOnce(
            params.value(QStringLiteral("direction")).toString(), std::move(completion));
        return;
    }
    if (method == QStringLiteral("screenshot_scrolling")) {
        const auto axis = params.value(QStringLiteral("axis")).toString(QStringLiteral("vertical"));
        if ((action == QStringLiteral("start") || action == QStringLiteral("set_axis")) &&
            axis != QStringLiteral("vertical") && axis != QStringLiteral("horizontal")) {
            fail();
            return;
        }
        const auto mode = axis == QStringLiteral("horizontal")
                              ? ScreenshotScrollingRecognitionMode::Horizontal
                              : ScreenshotScrollingRecognitionMode::Vertical;
        if (action == QStringLiteral("start")) {
            if (scrolling) {
                completion({}, QStringLiteral("busy"));
                return;
            }
            s.startScrollingScreenshot();
            if (!s.m_scrollingCaptureController || !s.m_scrollingCaptureController->active()) {
                completion({}, QStringLiteral("capture_unavailable"));
                return;
            }
            if (mode == ScreenshotScrollingRecognitionMode::Horizontal)
                static_cast<void>(s.m_scrollingCaptureController->setRecognitionMode(mode));
        } else {
            if (!scrolling) {
                completion({}, QStringLiteral("scrolling_not_ready"));
                return;
            }
            if (action == QStringLiteral("stop")) {
                static_cast<void>(s.stopScrollingCapture(true));
                success();
                return;
            }
            if (action == QStringLiteral("set_axis")) {
                if (s.m_scrollingCaptureController->recognitionMode() != mode &&
                    !s.m_scrollingCaptureController->setRecognitionMode(mode)) {
                    fail();
                    return;
                }
            } else if (action == QStringLiteral("auto_scroll")) {
                if (!params.value(QStringLiteral("enabled")).isBool()) {
                    fail();
                    return;
                }
                const bool enabled = params.value(QStringLiteral("enabled")).toBool();
                if (enabled && !snow_shot::platform::screenshotScrollPermission()) {
                    completion({}, QStringLiteral("permission_required"));
                    return;
                }
                s.m_scrollingCaptureController->setAutoScroll(enabled);
                success();
                return;
            } else if (action == QStringLiteral("trim")) {
                if (!s.m_scrollingCaptureController->setTrimRange(
                        params.value(QStringLiteral("start")).toInt(-1),
                        params.value(QStringLiteral("end")).toInt(-1))) {
                    fail();
                    return;
                }
                success();
                return;
            } else if (action == QStringLiteral("move")) {
                const auto offset = params.value(QStringLiteral("offset")).toArray();
                if (offset.size() != 2 || !s.m_scrollingCaptureController->moveSelection(
                                              QPoint(offset[0].toInt(), offset[1].toInt()))) {
                    fail();
                    return;
                }
                success();
                return;
            } else {
                fail();
                return;
            }
        }
    } else if (method == QStringLiteral("screenshot_recapture")) {
        if (!s.canRecapture()) {
            completion({}, QStringLiteral("action_unavailable"));
            return;
        }
        s.requestRecapture();
    } else if (method == QStringLiteral("screenshot_recognize")) {
        const auto kind = params.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("text"))
            s.setOcrTool();
        else if (kind == QStringLiteral("table"))
            s.setTableTool();
        else if (kind == QStringLiteral("qr"))
            s.setQrTool();
        else if (kind == QStringLiteral("markdown"))
            s.setMarkdownTool();
        else if (kind == QStringLiteral("html"))
            s.setHtmlTool();
        else {
            fail();
            return;
        }
    } else if (method == QStringLiteral("screenshot_translate")) {
        if (!s.m_ocrController || !s.m_ocrController->hasTextResult()) {
            completion({}, QStringLiteral("recognition_required"));
            return;
        }
        s.m_ocrController->beginTextTranslation();
    } else if (method == QStringLiteral("screenshot_auto_filter")) {
        const QStringList allowed{QStringLiteral("text"),      QStringLiteral("text_in_box"),
                                  QStringLiteral("image"),     QStringLiteral("avatar"),
                                  QStringLiteral("icon"),      QStringLiteral("message_box"),
                                  QStringLiteral("text_block")};
        if (!params.value(QStringLiteral("categories")).isArray()) {
            fail();
            return;
        }
        for (auto category : params.value(QStringLiteral("categories")).toArray())
            if (!allowed.contains(category.toString())) {
                fail();
                return;
            }
        s.m_mcpOperationError.clear();
        s.setAutoFilterTool();
        if (!s.m_autoFilterController) {
            completion({}, QStringLiteral("unavailable"));
            return;
        }
        s.m_autoFilterController->refresh();
    } else if (method == QStringLiteral("screenshot_edit_recognition") ||
               method == QStringLiteral("screenshot_undo") ||
               method == QStringLiteral("screenshot_redo")) {
        auto edit = params;
        if (method != QStringLiteral("screenshot_edit_recognition"))
            edit.insert(QStringLiteral("action"), method == QStringLiteral("screenshot_undo")
                                                      ? QStringLiteral("undo")
                                                      : QStringLiteral("redo"));
        if (!s.m_ocrController || !s.m_ocrController->editWorkflow(edit)) {
            completion({}, QStringLiteral("action_unavailable"));
            return;
        }
        success();
        return;
    } else if (method == QStringLiteral("screenshot_export_recognition")) {
        if (!s.m_ocrController) {
            completion({}, QStringLiteral("recognition_required"));
            return;
        }
        const auto result = s.m_ocrController->workflowResult();
        if (result.isEmpty()) {
            completion({}, QStringLiteral("recognition_required"));
            return;
        }
        const auto output = params.value(QStringLiteral("output")).toString();
        const auto format = params.value(QStringLiteral("format")).toString(QStringLiteral("text"));
        QString source;
        if (format == QStringLiteral("json"))
            source = QString::fromUtf8(QJsonDocument(result).toJson());
        else if (format == QStringLiteral("text") || format == QStringLiteral("html") ||
                 format == QStringLiteral("markdown")) {
            if (!result.contains(format)) {
                fail();
                return;
            }
            source = result.value(format).toString();
        } else {
            fail();
            return;
        }
        if (output == QStringLiteral("return")) {
            completion(result, {});
            return;
        }
        if (output == QStringLiteral("copy")) {
            auto* mime = new QMimeData;
            mime->setText(source);
            if (format == QStringLiteral("html"))
                mime->setHtml(source);
            QApplication::clipboard()->setMimeData(mime);
            completion({{QStringLiteral("copied"), true}}, {});
            return;
        }
        QString path;
        if (output != QStringLiteral("save") ||
            !snow_shot::app::mcp::ScreenshotMcpSession::validateOutputPath(
                params.value(QStringLiteral("path")).toString(), &path)) {
            fail();
            return;
        }
        s.m_mcpFileJob = ScreenshotExportCoordinator::shared().submit(
            this, ScreenshotExportCoordinator::Priority::Foreground,
            [path, bytes = source.toUtf8()](const ScreenshotExportCancellation& cancellation) {
                QSaveFile file(path);
                if (cancellation.isCancellationRequested() || !file.open(QIODevice::WriteOnly) ||
                    file.write(bytes) != bytes.size() || cancellation.isCancellationRequested() ||
                    !file.commit())
                    return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                               QStringLiteral("output_failed"));
                ScreenshotExportTaskResult result;
                result.savedPath = path;
                return result;
            },
            [completion = std::move(completion)](ScreenshotExportTaskResult result) {
                completion({{QStringLiteral("path"), result.savedPath}},
                           result.succeeded() ? QString() : QStringLiteral("output_failed"));
            });
        return;
    } else {
        completion({}, QStringLiteral("method_not_found"));
        return;
    }
    const auto generation = ++s.m_mcpCommandGeneration;
    const auto epoch = sourceEpoch;
    s.m_mcpCompletion = std::move(completion);
    s.m_mcpPoll = new QTimer(this);
    s.m_mcpPoll->setSingleShot(true);
    s.m_mcpPoll->setInterval(300000);
    const auto settle = [this, generation, epoch, method, params] {
        auto& s = *m_impl;
        if (s.m_mcpCommandGeneration != generation || !s.m_mcpPoll || !s.m_mcpCompletion)
            return;
        QJsonObject result;
        QString error;
        bool ready = false;
        if (method == QStringLiteral("screenshot_recapture")) {
            ready = !s.m_recaptureBusy;
            if (ready && s.m_captureEpoch == epoch)
                error = QStringLiteral("capture_unavailable");
        } else if (method == QStringLiteral("screenshot_scrolling")) {
            ready = s.m_scrollingCaptureController->state().value(QStringLiteral("ready")).toBool();
            if (!s.m_scrollingCaptureController->active()) {
                ready = true;
                error = QStringLiteral("capture_unavailable");
            }
        } else if (method == QStringLiteral("screenshot_auto_filter")) {
            ready = !s.m_autoFilterController->detecting();
            if (ready &&
                (!s.m_mcpOperationError.isEmpty() || !s.m_autoFilterController->available())) {
                error = QStringLiteral("detection_failed");
                result.insert(QStringLiteral("message"), s.m_mcpOperationError);
            }
            if (ready && error.isEmpty())
                for (auto category : params.value(QStringLiteral("categories")).toArray())
                    s.m_autoFilterController->fillCategory(category.toString());
            result.insert(QStringLiteral("categories"), params.value(QStringLiteral("categories")));
        } else if (!s.m_ocrController) {
            ready = true;
            error = QStringLiteral("unavailable");
        } else {
            const auto status = s.m_ocrController->workflowState();
            ready = !status.value(QStringLiteral("busy")).toBool();
            if (ready) {
                result = s.m_ocrController->workflowResult();
                if (!status.value(QStringLiteral("error")).toString().isEmpty() ||
                    result.isEmpty()) {
                    error = QStringLiteral("recognition_failed");
                    result.insert(QStringLiteral("message"), status.value(QStringLiteral("error")));
                }
            }
        }
        if (!ready)
            return;
        s.m_mcpPoll->stop();
        s.m_mcpPoll->deleteLater();
        s.m_mcpPoll = nullptr;
        auto callback = std::exchange(s.m_mcpCompletion, {});
        emit mcpCanvasChanged();
        if (callback)
            callback(std::move(result), std::move(error));
    };
    connect(this, &ScreenshotController::mcpCanvasChanged, s.m_mcpPoll, settle,
            Qt::QueuedConnection);
    connect(this, &ScreenshotController::mcpCapturePresented, s.m_mcpPoll, settle,
            Qt::QueuedConnection);
    connect(this, &ScreenshotController::mcpCaptureTerminated, s.m_mcpPoll, settle,
            Qt::QueuedConnection);
    if (s.m_scrollingCaptureController)
        connect(s.m_scrollingCaptureController.get(),
                &ScreenshotScrollingCaptureController::stateChanged, s.m_mcpPoll, settle,
                Qt::QueuedConnection);
    if (s.m_autoFilterController) {
        connect(s.m_autoFilterController.get(),
                &ScreenshotAutoFilterController::availabilityChanged, s.m_mcpPoll, settle,
                Qt::QueuedConnection);
        connect(s.m_autoFilterController.get(), &ScreenshotAutoFilterController::detectionFailed,
                s.m_mcpPoll, settle, Qt::QueuedConnection);
    }
    if (s.m_ocrController) {
        connect(s.m_ocrController.get(), &ScreenshotOcrController::workflowStateChanged,
                s.m_mcpPoll, settle, Qt::QueuedConnection);
    }
    connect(s.m_mcpPoll, &QTimer::timeout, this, [this, generation] {
        auto& state = *m_impl;
        if (state.m_mcpCommandGeneration != generation)
            return;
        auto callback = std::exchange(state.m_mcpCompletion, {});
        mcpCancelCommand();
        if (callback)
            callback({}, QStringLiteral("timeout"));
    });
    s.m_mcpPoll->start();
    QTimer::singleShot(0, s.m_mcpPoll, settle);
}
