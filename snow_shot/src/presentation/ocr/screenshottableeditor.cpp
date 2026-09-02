#include "snow_shot/presentation/screenshottableeditor.h"

#include "antd_icons.h"
#include "theme/theme_manager.h"
#include "widgets/context_menu.h"
#include "widgets/scroll_area.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTextDocument>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <utility>

namespace {
constexpr int kHeaderRole = Qt::UserRole;

class ReplaceTableDocumentCommand final : public QUndoCommand {
  public:
    ReplaceTableDocumentCommand(std::shared_ptr<ScreenshotTableEditingSession> session,
                                ScreenshotTableDocument before, ScreenshotTableDocument after,
                                QString label)
        : QUndoCommand(std::move(label)), m_session(std::move(session)),
          m_before(std::move(before)), m_after(std::move(after)) {}

    void undo() override {
        if (const auto session = m_session.lock()) {
            session->replaceDocument(m_before);
        }
    }

    void redo() override {
        if (const auto session = m_session.lock()) {
            session->replaceDocument(m_after);
        }
    }

  private:
    std::weak_ptr<ScreenshotTableEditingSession> m_session;
    ScreenshotTableDocument m_before;
    ScreenshotTableDocument m_after;
};

QString cssColor(const QColor& color) {
    return color.name(QColor::HexArgb);
}

QRectF tableBorderRect(const QRectF& bounds, qreal borderWidth) {
    const qreal halfWidth = std::max<qreal>(0.0, borderWidth / 2.0);
    const qreal inset = halfWidth + 0.5;
    return bounds.adjusted(inset, inset, -inset, -inset);
}

class ScreenshotTableActiveBorderLayer final : public QWidget {
  public:
    explicit ScreenshotTableActiveBorderLayer(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
        setAutoFillBackground(false);
    }

    void setStyle(const QColor& color, int width) {
        m_color = color;
        m_width = width;
        update();
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        if (!m_color.isValid() || m_width <= 0 || rect().isEmpty()) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(m_color, m_width, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        const QRectF borderRect = tableBorderRect(QRectF(rect()), m_width);
        QPainterPath borderPath;
        borderPath.addRect(borderRect);
        painter.drawPath(borderPath);
    }

  private:
    QColor m_color;
    int m_width = 0;
};

class ScreenshotTableCellEditor final : public QPlainTextEdit {
  public:
    explicit ScreenshotTableCellEditor(QWidget* parent = nullptr)
        : QPlainTextEdit(parent), m_activeBorder(new ScreenshotTableActiveBorderLayer(this)) {
        m_activeBorder->show();
        m_activeBorder->raise();
    }

    void setActiveBorderStyle(const QColor& color, int width) {
        m_activeBorder->setStyle(color, width);
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QPlainTextEdit::resizeEvent(event);
        m_activeBorder->setGeometry(rect());
        m_activeBorder->raise();
    }

  private:
    ScreenshotTableActiveBorderLayer* m_activeBorder = nullptr;
};

} // namespace

class ScreenshotTableModel final : public QAbstractTableModel {
  public:
    explicit ScreenshotTableModel(ScreenshotTableEditor* editor)
        : QAbstractTableModel(editor), m_editor(editor) {}

    void setSession(const std::shared_ptr<ScreenshotTableEditingSession>& session) {
        beginResetModel();
        m_session = session;
        endResetModel();
    }

    void resetDocument() {
        beginResetModel();
        endResetModel();
    }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() || m_session == nullptr ? 0 : m_session->document.rowCount();
    }

    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() || m_session == nullptr ? 0 : m_session->document.columnCount();
    }

    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || m_session == nullptr ||
            index.row() >= m_session->document.rowCount() ||
            index.column() >= m_session->document.columnCount()) {
            return {};
        }
        if (role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::ToolTipRole ||
            role == Qt::AccessibleTextRole) {
            return m_session->document.cellText(index.row(), index.column());
        }
        if (role == kHeaderRole) {
            const ScreenshotTableCell* cell =
                m_session->document.cellAt(index.row(), index.column());
            return cell != nullptr && cell->header;
        }
        return {};
    }

    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override {
        if (!index.isValid() || m_session == nullptr) {
            return Qt::NoItemFlags;
        }
        const QPoint anchor = m_session->document.anchorAt(index.row(), index.column());
        if (anchor.x() < 0) {
            return Qt::NoItemFlags;
        }
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
    }

    bool setData(const QModelIndex& index, const QVariant& value,
                 int role = Qt::EditRole) override {
        return role == Qt::EditRole && m_editor != nullptr &&
               m_editor->commitCellText(index, value.toString());
    }

  private:
    ScreenshotTableEditor* m_editor = nullptr;
    std::shared_ptr<ScreenshotTableEditingSession> m_session;
};

class ScreenshotTableDelegate final : public QStyledItemDelegate {
  public:
    explicit ScreenshotTableDelegate(ScreenshotTableEditor* editor)
        : QStyledItemDelegate(editor), m_editor(editor) {}

    void setTheme(const adqt::theme::ThemeMapToken& theme) {
        m_surface = theme.colorBgContainer;
        m_headerSurface = theme.colorFillAlter;
        m_hoverSurface = theme.colorFillTertiary;
        m_selectionSurface = theme.colorPrimaryBg;
        m_grid = theme.colorBorderSecondary;
        m_focus = theme.colorPrimary;
        m_selectionText = theme.colorWhite;
        m_text = theme.colorText;
        m_font = theme.appFont;
        m_padding = std::max(6, qRound(theme.sizeXS));
        m_lineWidth = std::max(1, qRound(theme.lineWidth));
    }

    [[nodiscard]] int cellInset() const {
        return m_padding + m_lineWidth;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        if (painter == nullptr || !index.isValid()) {
            return;
        }
        const bool header = index.data(kHeaderRole).toBool();
        QColor background = header ? m_headerSurface : m_surface;
        if ((option.state & QStyle::State_Selected) != 0) {
            background = m_selectionSurface;
        } else if ((option.state & QStyle::State_MouseOver) != 0) {
            background = m_hoverSurface;
        }

        painter->save();
        painter->fillRect(option.rect, background);
        // Shared cell edges are drawn once on the bottom and right. The first row and
        // column also provide the table's outer top and left edges.
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(m_grid, m_lineWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        QPainterPath edgePath;
        if (index.row() == 0) {
            edgePath.moveTo(option.rect.topLeft());
            edgePath.lineTo(option.rect.topRight());
        }
        if (index.column() == 0) {
            edgePath.moveTo(option.rect.topLeft());
            edgePath.lineTo(option.rect.bottomLeft());
        }
        edgePath.moveTo(option.rect.bottomLeft());
        edgePath.lineTo(option.rect.bottomRight());
        edgePath.lineTo(option.rect.topRight());
        painter->drawPath(edgePath);

        QFont font = m_font.family().isEmpty() ? option.font : m_font;
        font.setBold(header);
        painter->setFont(font);
        painter->setPen(m_text);
        const int inset = cellInset();
        const QRect textRect = option.rect.adjusted(inset, inset, -inset, -inset);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                          index.data(Qt::DisplayRole).toString());

        if ((option.state & QStyle::State_HasFocus) != 0) {
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(m_focus, m_lineWidth, Qt::SolidLine, Qt::SquareCap,
                                 Qt::MiterJoin));
            const QRectF focusRect = tableBorderRect(QRectF(option.rect), m_lineWidth);
            QPainterPath focusPath;
            focusPath.addRect(focusRect);
            painter->drawPath(focusPath);
        }
        painter->restore();
    }

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                          const QModelIndex& index) const override {
        auto* editor = new ScreenshotTableCellEditor(parent);
        editor->setObjectName(QStringLiteral("snowShotTableCellEditor"));
        editor->setFrameShape(QFrame::NoFrame);
        editor->setTabChangesFocus(true);
        editor->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        editor->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        editor->setVerticalScrollBar(new adqt::widgets::AdScrollBar(Qt::Vertical, editor));
        editor->setContentsMargins(0, 0, 0, 0);
        editor->document()->setDocumentMargin(0.0);
        editor->setProperty("snowShotTableRow", index.row());
        editor->setProperty("snowShotTableColumn", index.column());
        editor->setFont(m_font);
        editor->setActiveBorderStyle(m_focus, m_lineWidth);
        editor->setStyleSheet(QStringLiteral("QPlainTextEdit#snowShotTableCellEditor {"
                                             " padding: %1px; border: none;"
                                             " border-radius: 0; background: %2; color: %3;"
                                             " selection-background-color: %4; selection-color: %5; }")
                                  .arg(m_padding)
                                  .arg(cssColor(m_surface))
                                  .arg(cssColor(m_text))
                                  .arg(cssColor(m_focus))
                                  .arg(cssColor(m_selectionText)));
        return editor;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        auto* textEdit = qobject_cast<QPlainTextEdit*>(editor);
        if (textEdit == nullptr) {
            return;
        }
        textEdit->setPlainText(index.data(Qt::EditRole).toString());
        textEdit->selectAll();
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override {
        auto* textEdit = qobject_cast<QPlainTextEdit*>(editor);
        if (textEdit != nullptr && model != nullptr) {
            model->setData(index, textEdit->toPlainText(), Qt::EditRole);
        }
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* editor = qobject_cast<QPlainTextEdit*>(watched);
        if (editor != nullptr && event != nullptr && event->type() == QEvent::KeyPress) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Tab || keyEvent->key() == Qt::Key_Backtab) {
                const int row = editor->property("snowShotTableRow").toInt();
                const int column = editor->property("snowShotTableColumn").toInt();
                const int columnDelta = keyEvent->key() == Qt::Key_Backtab ? -1 : 1;
                if (m_editor != nullptr) {
                    m_editor->continueEditingAfter(editor, row, column, 0, columnDelta);
                }
                emit commitData(editor);
                emit closeEditor(editor, QAbstractItemDelegate::NoHint);
                return true;
            }
            if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) &&
                !keyEvent->modifiers().testFlag(Qt::AltModifier)) {
                const int row = editor->property("snowShotTableRow").toInt();
                const int column = editor->property("snowShotTableColumn").toInt();
                const int rowDelta = keyEvent->modifiers().testFlag(Qt::ShiftModifier) ? -1 : 1;
                if (m_editor != nullptr) {
                    m_editor->continueEditingAfter(editor, row, column, rowDelta, 0);
                }
                emit commitData(editor);
                emit closeEditor(editor, QAbstractItemDelegate::NoHint);
                return true;
            }
            if (keyEvent->key() == Qt::Key_Escape) {
                emit closeEditor(editor, QAbstractItemDelegate::RevertModelCache);
                return true;
            }
        }
        return QStyledItemDelegate::eventFilter(watched, event);
    }

  private:
    QColor m_surface;
    QColor m_headerSurface;
    QColor m_hoverSurface;
    QColor m_selectionSurface;
    QColor m_grid;
    QColor m_focus;
    QColor m_selectionText;
    QColor m_text;
    QFont m_font;
    int m_padding = 8;
    int m_lineWidth = 1;
    ScreenshotTableEditor* m_editor = nullptr;
};

ScreenshotTableEditingSession::ScreenshotTableEditingSession(
    ScreenshotTableDocument recognizedDocument)
    : baseline(recognizedDocument), document(std::move(recognizedDocument)) {
    undoStack.setUndoLimit(100);
}

void ScreenshotTableEditingSession::replaceDocument(const ScreenshotTableDocument& replacement) {
    if (document == replacement) {
        return;
    }
    document = replacement;
    if (documentChanged) {
        documentChanged();
    }
}

ScreenshotTableEditor::ScreenshotTableEditor(QWidget* parent)
    : QTableView(parent), m_model(new ScreenshotTableModel(this)),
      m_delegate(new ScreenshotTableDelegate(this)) {
    setObjectName(QStringLiteral("snowShotRecognizedTable"));
    setModel(m_model);
    setItemDelegate(m_delegate);
    setFrameShape(QFrame::NoFrame);
    setShowGrid(false);
    setAlternatingRowColors(false);
    setSelectionBehavior(QAbstractItemView::SelectItems);
    setSelectionMode(QAbstractItemView::ContiguousSelection);
    setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    setWordWrap(true);
    setTextElideMode(Qt::ElideNone);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBar(new adqt::widgets::AdScrollBar(Qt::Vertical, this));
    setHorizontalScrollBar(new adqt::widgets::AdScrollBar(Qt::Horizontal, this));
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    horizontalHeader()->hide();
    verticalHeader()->hide();
    horizontalHeader()->setStretchLastSection(false);
    setContextMenuPolicy(Qt::DefaultContextMenu);
    setAccessibleName(tr("Recognized table editor"));

    connect(selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this]() { normalizeSelection(); });
    connect(selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this]() { refreshCommandState(); });
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
            [this]() { applyTheme(); });
    applyTheme();
}

ScreenshotTableEditor::~ScreenshotTableEditor() {
    clearSession();
}

void ScreenshotTableEditor::setSession(std::shared_ptr<ScreenshotTableEditingSession> session) {
    if (m_session == session) {
        restoreSessionViewState();
        return;
    }
    clearSession();
    m_session = std::move(session);
    m_model->setSession(m_session);
    if (m_session != nullptr) {
        const QPointer<ScreenshotTableEditor> guard(this);
        m_session->documentChanged = [guard]() {
            if (guard != nullptr) {
                guard->handleDocumentChanged();
            }
        };
        connect(&m_session->undoStack, &QUndoStack::canUndoChanged, this,
                [this]() { refreshCommandState(); });
        connect(&m_session->undoStack, &QUndoStack::canRedoChanged, this,
                [this]() { refreshCommandState(); });
    }
    synchronizeSpans();
    updateTableMetrics();
    restoreSessionViewState();
    refreshCommandState();
}

void ScreenshotTableEditor::clearSession() {
    if (m_session != nullptr) {
        saveSessionViewState();
        m_session->documentChanged = {};
        disconnect(&m_session->undoStack, nullptr, this, nullptr);
    }
    m_session.reset();
    clearSpans();
    m_model->setSession(nullptr);
    refreshCommandState();
}

ScreenshotTableCommandState ScreenshotTableEditor::commandState() const {
    return m_commandState;
}

ScreenshotTableRange ScreenshotTableEditor::selectedRange() const {
    if (m_session == nullptr || selectionModel() == nullptr) {
        return {};
    }
    const QModelIndexList selected = selectionModel()->selectedIndexes();
    if (selected.isEmpty()) {
        return {};
    }
    ScreenshotTableRange range{selected.constFirst().row(), selected.constFirst().column(),
                               selected.constFirst().row(), selected.constFirst().column()};
    for (const QModelIndex& index : selected) {
        range.top = std::min(range.top, index.row());
        range.left = std::min(range.left, index.column());
        range.bottom = std::max(range.bottom, index.row());
        range.right = std::max(range.right, index.column());
    }
    return m_session->document.expandedRange(range);
}

bool ScreenshotTableEditor::isEditingCell() const {
    return state() == QAbstractItemView::EditingState;
}

bool ScreenshotTableEditor::commitActiveEdit() {
    bool committed = false;
    if (isEditingCell()) {
        auto* editor = findChild<QPlainTextEdit*>(QStringLiteral("snowShotTableCellEditor"));
        if (editor != nullptr) {
            committed = true;
            commitData(editor);
            closeEditor(editor, QAbstractItemDelegate::NoHint);
        }
    }
    return flushPendingCellEdit() || committed;
}

bool ScreenshotTableEditor::flushPendingCellEdit(quint64 expectedId) {
    if (!m_pendingCellEdit.has_value() ||
        (expectedId != 0 && m_pendingCellEdit->id != expectedId)) {
        return false;
    }
    PendingCellEdit pending = std::move(*m_pendingCellEdit);
    m_pendingCellEdit.reset();
    if (m_session == pending.session && pending.session->document != pending.replacement) {
        pushDocumentChange(pending.replacement, pending.label);
    }
    return true;
}

bool ScreenshotTableEditor::cancelActiveEdit() {
    if (!isEditingCell()) {
        return false;
    }
    if (auto* editor = findChild<QPlainTextEdit*>(QStringLiteral("snowShotTableCellEditor"))) {
        closeEditor(editor, QAbstractItemDelegate::RevertModelCache);
        return true;
    }
    return false;
}

void ScreenshotTableEditor::mergeSelection() {
    if (m_session == nullptr) {
        return;
    }
    const ScreenshotTableRange range = selectedRange();
    ScreenshotTableDocument replacement = m_session->document;
    if (!replacement.merge(range)) {
        return;
    }
    pushDocumentChange(replacement, tr("Merge cells"));
    selectRange(replacement.expandedRange(range));
}

void ScreenshotTableEditor::splitSelection() {
    if (m_session == nullptr) {
        return;
    }
    const ScreenshotTableRange range = selectedRange();
    ScreenshotTableDocument replacement = m_session->document;
    if (!replacement.split(range)) {
        return;
    }
    pushDocumentChange(replacement, tr("Split cells"));
    selectRange(range);
}

void ScreenshotTableEditor::resetDocument() {
    if (m_session != nullptr && m_session->document != m_session->baseline) {
        pushDocumentChange(m_session->baseline, tr("Reset table"));
    }
}

void ScreenshotTableEditor::undoEdit() {
    if (m_session != nullptr) {
        m_session->undoStack.undo();
    }
}

void ScreenshotTableEditor::redoEdit() {
    if (m_session != nullptr) {
        m_session->undoStack.redo();
    }
}

void ScreenshotTableEditor::copySelection() {
    if (copySelectionToClipboard()) {
        emit copyCompleted();
    }
}

bool ScreenshotTableEditor::copySelectionToClipboard() {
    if (m_session == nullptr || QApplication::clipboard() == nullptr) {
        return false;
    }
    // A current cell is navigation state, not a text selection. Copy the
    // complete recognized table when the selection model has no indexes.
    const ScreenshotTableRange range = selectionModel()->selectedIndexes().isEmpty()
                                           ? ScreenshotTableRange{
                                                 0, 0, m_session->document.rowCount() - 1,
                                                 m_session->document.columnCount() - 1}
                                           : selectedRange();
    if (!range.isValid()) {
        return false;
    }
    auto* mimeData = new QMimeData;
    mimeData->setHtml(m_session->document.toHtml(range));
    mimeData->setText(m_session->document.toPlainText(range));
    QApplication::clipboard()->setMimeData(mimeData);
    return true;
}

void ScreenshotTableEditor::pasteSelection() {
    if (m_session == nullptr || QApplication::clipboard() == nullptr) {
        return;
    }
    const QMimeData* mimeData = QApplication::clipboard()->mimeData();
    if (mimeData == nullptr) {
        return;
    }
    ScreenshotTableDocument source;
    if (mimeData->hasHtml()) {
        source = ScreenshotTableDocument::fromHtml(mimeData->html());
    }
    if (source.empty() && mimeData->hasText()) {
        source = ScreenshotTableDocument::fromPlainText(mimeData->text());
    }
    if (!source.empty()) {
        static_cast<void>(pasteDocument(source));
    }
}

void ScreenshotTableEditor::clearSelectionContents() {
    if (m_session == nullptr) {
        return;
    }
    ScreenshotTableDocument replacement = m_session->document;
    if (replacement.clear(selectedRange())) {
        pushDocumentChange(replacement, tr("Clear cell contents"));
    }
}

void ScreenshotTableEditor::mousePressEvent(QMouseEvent* event) {
    m_pointerDragged = false;
    m_pressPosition = event != nullptr ? event->position().toPoint() : QPoint();
    m_pressedIndex = event != nullptr ? anchorIndex(indexAt(m_pressPosition)) : QModelIndex();
    QTableView::mousePressEvent(event);
}

void ScreenshotTableEditor::mouseMoveEvent(QMouseEvent* event) {
    if (event != nullptr && (event->position().toPoint() - m_pressPosition).manhattanLength() >=
                                QApplication::startDragDistance()) {
        m_pointerDragged = true;
    }
    QTableView::mouseMoveEvent(event);
}

void ScreenshotTableEditor::mouseReleaseEvent(QMouseEvent* event) {
    QTableView::mouseReleaseEvent(event);
    if (event == nullptr || event->button() != Qt::LeftButton || m_pointerDragged ||
        event->modifiers() != Qt::NoModifier || !m_pressedIndex.isValid()) {
        return;
    }
    const QModelIndex released = anchorIndex(indexAt(event->position().toPoint()));
    if (released == m_pressedIndex) {
        setCurrentIndex(released);
        edit(released);
    }
}

void ScreenshotTableEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    const QModelIndex index =
        event != nullptr ? anchorIndex(indexAt(event->position().toPoint())) : QModelIndex();
    if (index.isValid()) {
        setCurrentIndex(index);
        edit(index);
        event->accept();
        return;
    }
    QTableView::mouseDoubleClickEvent(event);
}

void ScreenshotTableEditor::keyPressEvent(QKeyEvent* event) {
    if (event == nullptr) {
        return;
    }
    const bool command = event->modifiers().testFlag(Qt::ControlModifier) ||
                         event->modifiers().testFlag(Qt::MetaModifier);
    if (command && event->key() == Qt::Key_A) {
        selectAll();
        event->accept();
        return;
    }
    if (command && event->key() == Qt::Key_C) {
        copySelection();
        event->accept();
        return;
    }
    if (command && event->key() == Qt::Key_V) {
        pasteSelection();
        event->accept();
        return;
    }
    if (command && event->key() == Qt::Key_Z) {
        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
            redoEdit();
        } else {
            undoEdit();
        }
        event->accept();
        return;
    }
    if (command && event->key() == Qt::Key_Y) {
        redoEdit();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        clearSelectionContents();
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_F2 || event->key() == Qt::Key_Return ||
         event->key() == Qt::Key_Enter) &&
        currentIndex().isValid()) {
        edit(anchorIndex(currentIndex()));
        event->accept();
        return;
    }
    QTableView::keyPressEvent(event);
}

void ScreenshotTableEditor::contextMenuEvent(QContextMenuEvent* event) {
    if (event == nullptr) {
        return;
    }
    const QModelIndex clicked = anchorIndex(indexAt(event->pos()));
    if (clicked.isValid() && !selectionModel()->isSelected(clicked)) {
        setCurrentIndex(clicked);
        selectRange(m_session->document.spanRangeAt(clicked.row(), clicked.column()));
    }

    adqt::widgets::AdContextMenu menu(this);
    QAction* editAction = menu.addItem(tr("Edit cell"), adqt::icons::antd::outlined::Edit(),
                                       QKeySequence(Qt::Key_F2));
    QAction* copyAction =
        menu.addItem(tr("Copy"), adqt::icons::antd::outlined::Copy(), QKeySequence::Copy);
    QAction* pasteAction =
        menu.addItem(tr("Paste"), adqt::icons::antd::outlined::Snippets(), QKeySequence::Paste);
    QAction* clearAction =
        menu.addItem(tr("Clear contents"), adqt::icons::antd::outlined::IconDelete());
    menu.addSeparator();
    QAction* mergeAction =
        menu.addItem(tr("Merge cells"), adqt::icons::antd::outlined::MergeCells());
    QAction* splitAction =
        menu.addItem(tr("Split cells"), adqt::icons::antd::outlined::SplitCells());
    const ScreenshotTableCommandState state = commandState();
    editAction->setEnabled(clicked.isValid());
    copyAction->setEnabled(m_session != nullptr && !m_session->document.empty());
    pasteAction->setEnabled(QApplication::clipboard() != nullptr &&
                            QApplication::clipboard()->mimeData() != nullptr);
    clearAction->setEnabled(state.hasSelection);
    mergeAction->setEnabled(state.canMerge);
    splitAction->setEnabled(state.canSplit);

    connect(editAction, &QAction::triggered, this, [this, clicked]() {
        if (clicked.isValid()) {
            edit(clicked);
        }
    });
    connect(copyAction, &QAction::triggered, this, &ScreenshotTableEditor::copySelection);
    connect(pasteAction, &QAction::triggered, this, &ScreenshotTableEditor::pasteSelection);
    connect(clearAction, &QAction::triggered, this, &ScreenshotTableEditor::clearSelectionContents);
    connect(mergeAction, &QAction::triggered, this, &ScreenshotTableEditor::mergeSelection);
    connect(splitAction, &QAction::triggered, this, &ScreenshotTableEditor::splitSelection);
    menu.exec(event->globalPos());
}

void ScreenshotTableEditor::wheelEvent(QWheelEvent* event) {
    if (event == nullptr) {
        return;
    }

    if (event->modifiers().testFlag(Qt::ShiftModifier) && horizontalScrollBar() != nullptr &&
        horizontalScrollBar()->maximum() > horizontalScrollBar()->minimum()) {
        const int delta = !event->pixelDelta().isNull() ? event->pixelDelta().y()
                                                        : event->angleDelta().y();
        if (delta != 0) {
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta);
            event->accept();
            return;
        }
    }

    QTableView::wheelEvent(event);
}

void ScreenshotTableEditor::resizeEvent(QResizeEvent* event) {
    QTableView::resizeEvent(event);
    updateTableMetrics();
}

void ScreenshotTableEditor::updateGeometries() {
    QTableView::updateGeometries();
    if (m_tablePadding > 0) {
        QAbstractScrollArea::setViewportMargins(m_tablePadding, m_tablePadding, m_tablePadding,
                                                m_tablePadding);
    }
}

bool ScreenshotTableEditor::commitCellText(const QModelIndex& sourceIndex, const QString& text) {
    if (m_session == nullptr) {
        return false;
    }
    static_cast<void>(flushPendingCellEdit());
    const QModelIndex index = anchorIndex(sourceIndex);
    ScreenshotTableDocument replacement = m_session->document;
    if (!index.isValid() || !replacement.setCellText(index.row(), index.column(), text)) {
        return false;
    }
    const auto session = m_session;
    const QPointer<ScreenshotTableEditor> guard(this);
    const quint64 pendingId = ++m_nextCellEditId;
    m_pendingCellEdit =
        PendingCellEdit{session, std::move(replacement), tr("Edit cell"), pendingId};
    QTimer::singleShot(0, this, [guard, pendingId]() {
        if (guard != nullptr) {
            static_cast<void>(guard->flushPendingCellEdit(pendingId));
        }
    });
    return true;
}

void ScreenshotTableEditor::pushDocumentChange(const ScreenshotTableDocument& replacement,
                                               const QString& label) {
    if (m_session == nullptr || replacement == m_session->document) {
        return;
    }
    m_session->undoStack.push(
        new ReplaceTableDocumentCommand(m_session, m_session->document, replacement, label));
}

void ScreenshotTableEditor::continueEditingAfter(QWidget* closingEditor, int row, int column,
                                                 int rowDelta, int columnDelta) {
    if (closingEditor == nullptr || m_session == nullptr || (rowDelta == 0 && columnDelta == 0)) {
        return;
    }
    const QPointer<ScreenshotTableEditor> guard(this);
    connect(
        closingEditor, &QObject::destroyed, this,
        [guard, row, column, rowDelta, columnDelta]() {
            if (guard != nullptr) {
                guard->continueEditingFrom(row, column, rowDelta, columnDelta);
            }
        },
        Qt::SingleShotConnection);
}

void ScreenshotTableEditor::continueEditingFrom(int row, int column, int rowDelta,
                                                int columnDelta) {
    if (m_session == nullptr || (rowDelta == 0 && columnDelta == 0)) {
        return;
    }
    int targetRow = row + rowDelta;
    int targetColumn = column + columnDelta;
    if (targetColumn >= m_session->document.columnCount()) {
        targetColumn = 0;
        ++targetRow;
    } else if (targetColumn < 0) {
        targetColumn = m_session->document.columnCount() - 1;
        --targetRow;
    }
    targetRow = std::clamp(targetRow, 0, m_session->document.rowCount() - 1);
    const QPointer<ScreenshotTableEditor> guard(this);
    QTimer::singleShot(0, this, [guard, targetRow, targetColumn]() {
        if (guard == nullptr || guard->m_session == nullptr) {
            return;
        }
        const QModelIndex target =
            guard->anchorIndex(guard->m_model->index(targetRow, targetColumn));
        if (!target.isValid()) {
            return;
        }
        guard->setCurrentIndex(target);
        guard->selectRange(guard->m_session->document.spanRangeAt(target.row(), target.column()));
        guard->setFocus(Qt::TabFocusReason);
        static_cast<void>(guard->edit(target, QAbstractItemView::AllEditTriggers, nullptr));
    });
}

void ScreenshotTableEditor::handleDocumentChanged() {
    ScreenshotTableRange range = selectedRange();
    if (!range.isValid() && m_session != nullptr) {
        range = m_session->selection;
    }
    const QModelIndex current = anchorIndex(currentIndex());
    QPoint currentCell =
        current.isValid() ? QPoint(current.column(), current.row()) : QPoint(-1, -1);
    if (currentCell.x() < 0 && range.isValid()) {
        currentCell = QPoint(range.left, range.top);
    }
    if (m_session != nullptr) {
        m_session->selection = range;
        m_session->currentCell = currentCell;
    }
    m_model->resetDocument();
    synchronizeSpans();
    if (m_session != nullptr && currentCell.x() >= 0 && currentCell.y() >= 0) {
        setCurrentIndex(anchorIndex(m_model->index(currentCell.y(), currentCell.x())));
    }
    if (range.isValid()) {
        selectRange(range);
    } else {
        normalizeSelection();
    }
    emit documentChanged();
    refreshCommandState();
}

void ScreenshotTableEditor::refreshCommandState() {
    ScreenshotTableCommandState state;
    if (m_session != nullptr) {
        const ScreenshotTableRange range = selectedRange();
        state.canUndo = m_session->undoStack.canUndo();
        state.canRedo = m_session->undoStack.canRedo();
        state.hasSelection = range.isValid();
        state.canMerge = m_session->document.canMerge(range);
        state.canSplit = m_session->document.canSplit(range);
        state.canReset = m_session->document != m_session->baseline;
    }
    if (state != m_commandState) {
        m_commandState = state;
        emit commandStateChanged(m_commandState);
    }
}

void ScreenshotTableEditor::normalizeSelection() {
    if (m_normalizingSelection || m_session == nullptr || selectionModel() == nullptr) {
        return;
    }
    const ScreenshotTableRange range = selectedRange();
    if (!range.isValid()) {
        refreshCommandState();
        return;
    }
    m_normalizingSelection = true;
    selectRange(range);
    m_normalizingSelection = false;
    refreshCommandState();
}

void ScreenshotTableEditor::restoreSessionViewState() {
    if (m_session == nullptr || m_session->document.empty()) {
        return;
    }
    QModelIndex current;
    if (m_session->currentCell.x() >= 0 && m_session->currentCell.y() >= 0) {
        current = m_model->index(m_session->currentCell.y(), m_session->currentCell.x());
    }
    if (!current.isValid()) {
        current = m_model->index(0, 0);
    }
    setCurrentIndex(anchorIndex(current));
    if (m_session->selection.isValid()) {
        selectRange(m_session->selection);
    } else {
        const QSignalBlocker blocker(selectionModel());
        selectionModel()->clearSelection();
        refreshCommandState();
    }
    horizontalScrollBar()->setValue(m_session->horizontalScrollValue);
    verticalScrollBar()->setValue(m_session->verticalScrollValue);
}

void ScreenshotTableEditor::saveSessionViewState() {
    if (m_session == nullptr) {
        return;
    }
    m_session->selection = selectedRange();
    const QModelIndex current = anchorIndex(currentIndex());
    m_session->currentCell =
        current.isValid() ? QPoint(current.column(), current.row()) : QPoint(-1, -1);
    m_session->horizontalScrollValue = horizontalScrollBar()->value();
    m_session->verticalScrollValue = verticalScrollBar()->value();
}

void ScreenshotTableEditor::synchronizeSpans() {
    clearSpans();
    if (m_session == nullptr) {
        return;
    }
    const ScreenshotTableDocument& document = m_session->document;
    for (int row = 0; row < document.rowCount(); ++row) {
        for (int column = 0; column < document.columnCount(); ++column) {
            const ScreenshotTableCell* cell = document.anchorCellAt(row, column);
            if (cell != nullptr && (cell->rowSpan > 1 || cell->columnSpan > 1)) {
                setSpan(row, column, cell->rowSpan, cell->columnSpan);
            }
        }
    }
}

void ScreenshotTableEditor::updateTableMetrics() {
    if (m_session == nullptr || viewport()->width() <= 0) {
        return;
    }
    const adqt::theme::ThemeMapToken theme =
        adqt::theme::ThemeManager::instance().resolveTheme(this);
    const QFontMetrics metrics(theme.appFont.family().isEmpty() ? font() : theme.appFont);
    const int minimumWidth = std::max(72, qRound(theme.controlHeight * 2.75));
    const int maximumWidth = std::max(minimumWidth, qRound(theme.controlHeight * 11.25));
    const int cellInset = m_delegate->cellInset();
    const int horizontalPadding = cellInset * 2;
    const int sampleRows = std::min(m_session->document.rowCount(), 200);
    QVector<int> widths(m_session->document.columnCount(), minimumWidth);
    int totalWidth = 0;
    for (int column = 0; column < widths.size(); ++column) {
        for (int row = 0; row < sampleRows; ++row) {
            if (!m_session->document.isAnchor(row, column)) {
                continue;
            }
            const QStringList lines = m_session->document.cellText(row, column)
                                          .split(QLatin1Char('\n'), Qt::KeepEmptyParts);
            for (const QString& line : lines) {
                widths[column] =
                    std::max(widths[column], metrics.horizontalAdvance(line) + horizontalPadding);
            }
        }
        widths[column] = std::min(widths[column], maximumWidth);
        totalWidth += widths[column];
    }
    const int availableWidth = viewport()->width();
    if (totalWidth > 0 && totalWidth < availableWidth) {
        int remaining = availableWidth - totalWidth;
        for (int column = 0; column < widths.size() && remaining > 0; ++column) {
            const int columnsRemaining = static_cast<int>(widths.size()) - column;
            const int share = std::max(1, remaining / columnsRemaining);
            const int growth = std::min(share, maximumWidth - widths[column]);
            widths[column] += growth;
            remaining -= growth;
        }
    }
    for (int column = 0; column < widths.size(); ++column) {
        setColumnWidth(column, widths.at(column));
    }

    const int baseHeight = std::max(24, qRound(theme.controlHeight));
    verticalHeader()->setDefaultSectionSize(baseHeight);
    const int sampledRows = std::min(m_session->document.rowCount(), 200);
    for (int row = 0; row < sampledRows; ++row) {
        int visibleLines = 1;
        for (int column = 0; column < m_session->document.columnCount(); ++column) {
            if (m_session->document.isAnchor(row, column)) {
                const int lineCount =
                    static_cast<int>(
                        m_session->document.cellText(row, column).count(QLatin1Char('\n'))) +
                    1;
                visibleLines = std::max(visibleLines, std::min(3, lineCount));
            }
        }
        setRowHeight(row,
                     std::max(baseHeight, visibleLines * metrics.lineSpacing() + cellInset * 2));
    }
}

void ScreenshotTableEditor::applyTheme() {
    const adqt::theme::ThemeMapToken theme =
        adqt::theme::ThemeManager::instance().resolveTheme(this);
    m_delegate->setTheme(theme);
    if (!theme.appFont.family().isEmpty()) {
        setFont(theme.appFont);
    }
    m_tablePadding = std::max(0, qRound(theme.sizeSM));
    updateGeometries();
    QPalette tablePalette = palette();
    tablePalette.setColor(QPalette::Base, theme.colorBgContainer);
    tablePalette.setColor(QPalette::AlternateBase, theme.colorFillAlter);
    tablePalette.setColor(QPalette::Text, theme.colorText);
    tablePalette.setColor(QPalette::Window, theme.colorBgContainer);
    tablePalette.setColor(QPalette::Highlight, theme.colorPrimaryBg);
    tablePalette.setColor(QPalette::HighlightedText, theme.colorText);
    setPalette(tablePalette);
    setStyleSheet(
        QStringLiteral("QTableView#snowShotRecognizedTable { background: %1; border: none; }")
            .arg(cssColor(theme.colorBgContainer)));
    updateTableMetrics();
    viewport()->update();
}

QModelIndex ScreenshotTableEditor::anchorIndex(const QModelIndex& index) const {
    if (!index.isValid() || m_session == nullptr) {
        return {};
    }
    const QPoint anchor = m_session->document.anchorAt(index.row(), index.column());
    return anchor.x() >= 0 ? m_model->index(anchor.y(), anchor.x()) : QModelIndex();
}

bool ScreenshotTableEditor::pasteDocument(const ScreenshotTableDocument& source) {
    if (m_session == nullptr || source.empty()) {
        return false;
    }
    const QModelIndex current = anchorIndex(currentIndex());
    if (!current.isValid()) {
        return false;
    }
    const ScreenshotTableRange destination{current.row(), current.column(),
                                           current.row() + source.rowCount() - 1,
                                           current.column() + source.columnCount() - 1};
    if (destination.bottom >= m_session->document.rowCount() ||
        destination.right >= m_session->document.columnCount()) {
        emit operationRejected(tr("The pasted cells do not fit inside the recognized table"));
        return false;
    }
    for (int row = destination.top; row <= destination.bottom; ++row) {
        for (int column = destination.left; column <= destination.right; ++column) {
            const ScreenshotTableRange span = m_session->document.spanRangeAt(row, column);
            if (span.top < destination.top || span.left < destination.left ||
                span.bottom > destination.bottom || span.right > destination.right) {
                emit operationRejected(
                    tr("Select the complete merged cell before pasting into this area"));
                return false;
            }
        }
    }

    ScreenshotTableDocument replacement = m_session->document;
    static_cast<void>(replacement.split(destination));
    for (int row = 0; row < source.rowCount(); ++row) {
        for (int column = 0; column < source.columnCount(); ++column) {
            const ScreenshotTableCell* cell = source.anchorCellAt(row, column);
            if (cell == nullptr) {
                continue;
            }
            const ScreenshotTableRange target{destination.top + row, destination.left + column,
                                              destination.top + row + cell->rowSpan - 1,
                                              destination.left + column + cell->columnSpan - 1};
            if (cell->rowSpan > 1 || cell->columnSpan > 1) {
                static_cast<void>(replacement.merge(target));
            }
            static_cast<void>(replacement.setCellText(target.top, target.left, cell->text));
        }
    }
    pushDocumentChange(replacement, tr("Paste cells"));
    selectRange(destination);
    return true;
}

void ScreenshotTableEditor::selectRange(const ScreenshotTableRange& source) {
    if (m_session == nullptr || selectionModel() == nullptr) {
        return;
    }
    const ScreenshotTableRange range = m_session->document.expandedRange(source);
    if (!range.isValid()) {
        return;
    }
    const QItemSelection selection(m_model->index(range.top, range.left),
                                   m_model->index(range.bottom, range.right));
    {
        const QSignalBlocker blocker(selectionModel());
        selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    }
    m_session->selection = range;
    refreshCommandState();
}
