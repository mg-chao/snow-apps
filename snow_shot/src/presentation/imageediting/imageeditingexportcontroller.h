// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotrecognitionfileexport.h"
#include <QMimeData>
#include <QObject>
#include <functional>
#include <memory>
#include <variant>

class ScreenshotPrintService;
class QWidget;

namespace snow_shot::presentation {
// Owns one immutable output request and all of its dialogs, jobs and cancellation state.
// Content selection and saved checkpoints remain the editor's responsibility.
class ImageEditingExportController final : public QObject {
  public:
    enum class Operation { Copy, Save, QuickSave, Print, Upload, Pin };
    using Content = std::variant<ScreenshotExportSource, QString, ScreenshotRecognitionFileSnapshot,
                                 std::unique_ptr<QMimeData>>;
    using Completion = std::function<void(bool, QString)>;
    using Pin = std::function<bool(QImage, std::function<void(bool)>)>;

    explicit ImageEditingExportController(ScreenshotPrintService* printing, QObject* parent,
                                          Pin pin = {});
    ~ImageEditingExportController() override;
    bool busy() const;
    void start(Operation operation, Content content, QWidget* owner, QWidget* stackingOwner,
               Completion completion);
    // Retires the request before cancelling services so reentrant and late callbacks are ignored.
    void cancel();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation
