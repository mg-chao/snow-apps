#include <QAbstractItemModel>
#include <QColor>
#include <QCoreApplication>
#include <QListView>
#include <QLineEdit>
#include <QLayout>
#include <QToolButton>
#include <QStringList>
#include <QTest>
#include <QWidget>

#include "widgets/select.h"
#include "widgets/control_scale.h"

using adqt::widgets::AdSelect;

namespace {

AdSelect::Option makeOption(const QString& value, const QString& label) {
  AdSelect::Option option;
  option.value = value;
  option.label = label;
  return option;
}

QStringList popupLabels(AdSelect& select) {
  const QListView* view = select.view();
  const QAbstractItemModel* model = view ? view->model() : nullptr;
  QStringList labels;
  if (!model) {
    return labels;
  }

  labels.reserve(model->rowCount());
  for (int row = 0; row < model->rowCount(); ++row) {
    labels.append(model->index(row, 0).data(Qt::DisplayRole).toString());
  }
  return labels;
}

QWidget* popupSurface(AdSelect& select) {
  QWidget* candidate = select.view();
  while (candidate && candidate->objectName() != QStringLiteral("adselect-popup")) {
    candidate = candidate->parentWidget();
  }
  return candidate;
}

int horizontalCenterIn(const QWidget* widget, const QWidget* ancestor) {
  return widget->mapTo(ancestor, widget->rect().center()).x();
}

}  // namespace

class SelectTest final : public QObject {
  Q_OBJECT

 private slots:
  void popupCanBeDestroyedBeforeItsSelectDuringHostTeardown() {
    auto* host = new QWidget;
    auto* toolbar = new QWidget(host);
    auto* select = new AdSelect(toolbar);
    select->setOptions({makeOption(QStringLiteral("mosaic"), QStringLiteral("Mosaic"))});
    select->setPopupFooterWidget(new QWidget);
    QWidget* popup = popupSurface(*select);
    QVERIFY(popup);
    QCOMPARE(popup->parentWidget(), host);
    // Stacking changes reorder QObject children: the host can delete the popup first.
    popup->lower();
    QPointer<AdSelect> guardedSelect(select);
    QPointer<QWidget> guardedPopup(popup);
    delete host;
    QVERIFY(guardedSelect.isNull());
    QVERIFY(guardedPopup.isNull());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }

  void controlScaleAppliesToContentAndSurvivesStyleRefresh() {
    for (const auto size : {AdSelect::ControlSize::Small, AdSelect::ControlSize::Middle,
                            AdSelect::ControlSize::Large}) {
      AdSelect select;
      select.setControlSize(size);
      select.setVariant(AdSelect::Variant::Borderless);
      select.setOptions({makeOption(QStringLiteral("a"), QStringLiteral("Alpha"))});
      select.setCurrentValue(QStringLiteral("a"));
      select.show();
      QCoreApplication::processEvents();
      auto* input = select.findChild<QLineEdit*>(QStringLiteral("adselect-input"));
      auto* suffix = select.findChild<QToolButton*>(QStringLiteral("adselect-suffix"));
      QVERIFY(input);
      QVERIFY(suffix);
      const int fontSize = input->font().pixelSize();
      const int height = select.height();
      const int iconSize = suffix->iconSize().height();
      const int padding = select.layout()->contentsMargins().left();
      const int rowHeight = select.view()->sizeHintForRow(0);
      adqt::widgets::AdControlScaleScope scope(&select);
      for (const qreal scale : {0.8, 0.64, 0.8 / 1.5, 1.25, 0.8, 1.0}) {
        scope.publishScale(
            adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(1, 1, scale));
        // Hover, selection and theme changes all refresh the resolved style.
        select.setStatus(AdSelect::Status::Warning);
        select.setStatus(AdSelect::Status::None);
        QCoreApplication::processEvents();
        QCOMPARE(input->font().pixelSize(), qRound(fontSize * scale));
        QCOMPARE(select.height(), qRound(height * scale));
        QCOMPARE(select.sizeHint().height(), select.height());
        QCOMPARE(select.minimumSizeHint().height(), select.height());
        QCOMPARE(suffix->iconSize().height(), qRound(iconSize * scale));
        QVERIFY(qAbs(select.layout()->contentsMargins().left() - qRound(padding * scale)) <= 1);
        QCOMPARE(select.view()->sizeHintForRow(0), qRound(rowHeight * scale));
      }
    }
  }

  void scaledMultipleModesKeepTagsAndHintsConsistent() {
    for (const auto mode : {AdSelect::Mode::Multiple, AdSelect::Mode::Tags}) {
      AdSelect select;
      select.setMode(mode);
      select.setOptions({makeOption(QStringLiteral("a"), QStringLiteral("Alpha"))});
      select.setCurrentValues({QStringLiteral("a")});
      select.resize(300, select.sizeHint().height());
      select.show();
      QCoreApplication::processEvents();
      const int height = select.height();
      auto* input = select.findChild<QLineEdit*>(QStringLiteral("adselect-input"));
      QVERIFY(input);
      const int fontSize = input->font().pixelSize();
      adqt::widgets::AdControlScaleScope scope(&select);
      for (const qreal scale : {0.8, 0.64, 1.0}) {
        scope.publishScale(
            adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(1, 1, scale));
        QCoreApplication::processEvents();
        QCOMPARE(input->font().pixelSize(), qRound(fontSize * scale));
        QCOMPARE(select.height(), qRound(height * scale));
        QCOMPARE(select.sizeHint().height(), select.height());
      }
    }
  }

  void defaultPopupOrderMatchesInputOrder() {
    AdSelect select;
    select.setOptions({makeOption(QStringLiteral("z"), QStringLiteral("Zulu")),
                       makeOption(QStringLiteral("a"), QStringLiteral("Alpha")),
                       makeOption(QStringLiteral("m"), QStringLiteral("Middle"))});

    QCOMPARE(popupLabels(select), QStringList({QStringLiteral("Zulu"), QStringLiteral("Alpha"),
                                               QStringLiteral("Middle")}));
  }

  void filteringPreservesInputOrderWithoutSortComparator() {
    AdSelect select;
    select.setOptions({makeOption(QStringLiteral("g"), QStringLiteral("Gamma")),
                       makeOption(QStringLiteral("a"), QStringLiteral("Alpha")),
                       makeOption(QStringLiteral("b"), QStringLiteral("Beta")),
                       makeOption(QStringLiteral("z"), QStringLiteral("Zulu"))});
    select.setSearchEnabled(true);
    select.setSearchText(QStringLiteral("a"));

    QCOMPARE(popupLabels(select), QStringList({QStringLiteral("Gamma"), QStringLiteral("Alpha"),
                                               QStringLiteral("Beta")}));
  }

  void customSortComparatorStillControlsPopupOrder() {
    AdSelect select;
    select.setOptions({makeOption(QStringLiteral("z"), QStringLiteral("Zulu")),
                       makeOption(QStringLiteral("a"), QStringLiteral("Alpha")),
                       makeOption(QStringLiteral("m"), QStringLiteral("Middle"))});
    select.setSortComparator([](const AdSelect::Option& lhs, const AdSelect::Option& rhs) {
      return lhs.label < rhs.label;
    });

    QCOMPARE(popupLabels(select), QStringList({QStringLiteral("Alpha"), QStringLiteral("Middle"),
                                               QStringLiteral("Zulu")}));

    select.setSortComparator(AdSelect::SortComparator{});
    QCOMPARE(popupLabels(select), QStringList({QStringLiteral("Zulu"), QStringLiteral("Alpha"),
                                               QStringLiteral("Middle")}));
  }

  void groupedOptionHeadersUseDescriptionColor() {
    AdSelect select;
    AdSelect::Option general = makeOption(QStringLiteral("general"), QStringLiteral("General"));
    general.group = QStringLiteral("General Models");
    AdSelect::Option translation =
        makeOption(QStringLiteral("translation"), QStringLiteral("Translation"));
    translation.group = QStringLiteral("Translation Models");
    select.setOptions({general, translation});

    const QAbstractItemModel* model = select.view()->model();
    QVERIFY(model != nullptr);
    QCOMPARE(model->index(0, 0).data(Qt::DisplayRole).toString(), QStringLiteral("General Models"));
    const QColor headerColor = model->index(0, 0).data(Qt::ForegroundRole).value<QColor>();
    const QColor optionColor = model->index(1, 0).data(Qt::ForegroundRole).value<QColor>();
    QVERIFY(headerColor.isValid());
    QVERIFY(optionColor.isValid());
    QVERIFY(headerColor != optionColor);
  }

  void centeredPlacementSurvivesPopupGeometryRefreshes() {
    QWidget host;
    host.resize(800, 480);

    AdSelect select(&host);
    select.setGeometry(220, 80, 360, 32);
    select.setSearchEnabled(true);
    select.setPopupMatchSelectWidth(false);
    select.setPopupWidth(390);
    select.setPlacement(AdSelect::Placement::BottomCenter);
    select.setOptions({makeOption(QStringLiteral("alpha"), QStringLiteral("Alpha")),
                       makeOption(QStringLiteral("beta"), QStringLiteral("Beta"))});

    host.show();
    select.showPopup();
    QCoreApplication::processEvents();

    QWidget* popup = popupSurface(select);
    QVERIFY(popup != nullptr);
    QCOMPARE(select.placement(), AdSelect::Placement::BottomCenter);
    QCOMPARE(horizontalCenterIn(popup, &host), horizontalCenterIn(&select, &host));

    select.setSearchText(QStringLiteral("a"));
    QCoreApplication::processEvents();
    QCOMPARE(horizontalCenterIn(popup, &host), horizontalCenterIn(&select, &host));

    host.resize(900, 480);
    QCoreApplication::processEvents();
    QCOMPARE(horizontalCenterIn(popup, &host), horizontalCenterIn(&select, &host));
  }
};

QTEST_MAIN(SelectTest)

#include "select_tests.moc"
