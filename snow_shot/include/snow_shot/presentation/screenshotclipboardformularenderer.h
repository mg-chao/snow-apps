// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDFORMULARENDERER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDFORMULARENDERER_H

#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotlatexrenderer.h"

#include <functional>
#include <optional>
#include <variant>

// Clipboard pinning only: prepare on an import worker, then convert on the GUI thread.
// Successful formulas skip text layout/rasterization; failures decode their snapshot
// on the export worker without losing formatting or changing image/file precedence.
class ScreenshotClipboardFormulaRenderer final : public QObject {
  public:
    struct DeferredFormula final {
        QString source;
        ScreenshotClipboardContentSnapshot text;
    };
    using PreparedContent = std::variant<ScreenshotClipboardContent, DeferredFormula>;
    using Completion = std::function<void(std::optional<ScreenshotClipboardContent>)>;

    explicit ScreenshotClipboardFormulaRenderer(QObject* parent = nullptr);
    ~ScreenshotClipboardFormulaRenderer() override;

    [[nodiscard]] static std::optional<QString> formulaSource(const QString& text);
    [[nodiscard]] static std::optional<PreparedContent>
    prepare(ScreenshotClipboardContentSnapshot snapshot,
            ScreenshotClipboardContentReader::CancellationCheck cancelled = {},
            ScreenshotClipboardContentReader::AllocationCheck allocate = {});
    void convert(PreparedContent content, qreal devicePixelRatio, Completion completion);
    void cancel();
    [[nodiscard]] bool active() const {
        return static_cast<bool>(m_completion);
    }

  private:
    void finish(std::optional<ScreenshotClipboardContent> content);
    ScreenshotLatexRenderer m_renderer;
    quint64 m_token = 0;
    quint64 m_generation = 0;
    std::optional<std::variant<ScreenshotClipboardContent, ScreenshotClipboardContentSnapshot>>
        m_fallback;
    ScreenshotExportJobHandle m_fallbackJob;
    Completion m_completion;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDFORMULARENDERER_H
