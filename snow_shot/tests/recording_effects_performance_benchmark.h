#pragma once
#include <QApplication>
#include <QElapsedTimer>
#include <functional>

class RecordingEffectsBenchmarkApplication : public QApplication {
  public:
    using QApplication::QApplication;
    std::function<void(QObject*, QEvent*, qint64)> afterEvent;
    bool notify(QObject* receiver, QEvent* event) override {
        if (!afterEvent) {
            return QApplication::notify(receiver, event);
        }
        QElapsedTimer elapsed;
        elapsed.start();
        const bool result = QApplication::notify(receiver, event);
        if (afterEvent) {
            afterEvent(receiver, event, elapsed.nsecsElapsed());
        }
        return result;
    }
};
int runRecordingEffectsPerformanceBenchmark(RecordingEffectsBenchmarkApplication& app);
