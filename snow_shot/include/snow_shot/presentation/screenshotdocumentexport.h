#pragma once
#include "snow_shot/presentation/screenshotrecognizeddocument.h"
#include <functional>
#include <QMap>
[[nodiscard]] QString screenshotDocumentImageDirectoryName(const QString& primaryName);
[[nodiscard]] QMap<QString, QByteArray>
screenshotDocumentFiles(const ScreenshotRecognizedDocument& document, const QString& primaryName,
                        const QString& extension, const std::function<bool()>& cancelled = {});
