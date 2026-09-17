#include "color_picker_value_model.h"

#include <QRegularExpression>

#include <algorithm>

namespace adqt::widgets::detail {

namespace {

bool modeListContains(const QVector<ColorPickerValueModel::Mode>& modes,
                      ColorPickerValueModel::Mode value) {
  return std::find(modes.cbegin(), modes.cend(), value) != modes.cend();
}

QString formatPercent(double value) {
  QString text = QString::number(value, 'f', 3);
  while (text.contains(QLatin1Char('.')) &&
         (text.endsWith(QLatin1Char('0')) || text.endsWith(QLatin1Char('.')))) {
    text.chop(1);
    if (text.endsWith(QLatin1Char('.'))) {
      text.chop(1);
      break;
    }
  }
  return text.isEmpty() ? QStringLiteral("0") : text;
}

QString colorToHexRgbLower(const QColor& color) { return color.name(QColor::HexRgb).toLower(); }

QString colorToHexRgbaLower(const QColor& color) {
  return QStringLiteral("#%1%2%3%4")
      .arg(color.red(), 2, 16, QChar('0'))
      .arg(color.green(), 2, 16, QChar('0'))
      .arg(color.blue(), 2, 16, QChar('0'))
      .arg(color.alpha(), 2, 16, QChar('0'))
      .toLower();
}

bool parseCssHexColor(const QString& input, QColor* out) {
  if (!out) {
    return false;
  }

  const QString trimmed = input.trimmed();
  if (!trimmed.startsWith(QLatin1Char('#'))) {
    return false;
  }

  const QString hex = trimmed.mid(1);
  const qsizetype length = hex.size();
  if (length != 3 && length != 4 && length != 6 && length != 8) {
    return false;
  }

  static const QRegularExpression kHexPattern(QStringLiteral("^[0-9a-fA-F]+$"));
  if (!kHexPattern.match(hex).hasMatch()) {
    return false;
  }

  bool ok = false;
  int red = 0;
  int green = 0;
  int blue = 0;
  int alpha = 255;

  if (length == 3 || length == 4) {
    red = QString(hex.at(0)).repeated(2).toInt(&ok, 16);
    if (!ok) {
      return false;
    }
    green = QString(hex.at(1)).repeated(2).toInt(&ok, 16);
    if (!ok) {
      return false;
    }
    blue = QString(hex.at(2)).repeated(2).toInt(&ok, 16);
    if (!ok) {
      return false;
    }
    if (length == 4) {
      alpha = QString(hex.at(3)).repeated(2).toInt(&ok, 16);
      if (!ok) {
        return false;
      }
    }
  } else {
    red = hex.mid(0, 2).toInt(&ok, 16);
    if (!ok) {
      return false;
    }
    green = hex.mid(2, 2).toInt(&ok, 16);
    if (!ok) {
      return false;
    }
    blue = hex.mid(4, 2).toInt(&ok, 16);
    if (!ok) {
      return false;
    }
    if (length == 8) {
      alpha = hex.mid(6, 2).toInt(&ok, 16);
      if (!ok) {
        return false;
      }
    }
  }

  const QColor parsed(red, green, blue, alpha);
  if (!parsed.isValid()) {
    return false;
  }
  *out = parsed;
  return true;
}

QColor fallbackEditableColor() { return QColor(QStringLiteral("#1677ff")); }

QColor transparentBaseColor(const QColor& source) {
  QColor color = source.isValid() ? source : QColor(0, 0, 0, 255);
  color.setAlpha(0);
  return color;
}

}  // namespace

QVector<ColorPickerValueModel::Mode> ColorPickerValueModel::normalizeModeOptions(
    const QVector<Mode>& options) {
  QVector<Mode> normalized;
  normalized.reserve(options.size());
  for (Mode value : options) {
    if (modeListContains(normalized, value)) {
      continue;
    }
    normalized.append(value);
  }
  if (normalized.isEmpty()) {
    normalized.append(Mode::Solid);
  }
  return normalized;
}

QVector<AdColorGradientStop> ColorPickerValueModel::normalizeStops(
    const QVector<AdColorGradientStop>& stops, const QColor& fallbackColor) {
  QVector<AdColorGradientStop> normalized;
  normalized.reserve(stops.size());
  for (const AdColorGradientStop& stop : stops) {
    if (!stop.color.isValid()) {
      continue;
    }
    normalized.append(AdColorGradientStop{stop.color, std::clamp(stop.percent, 0, 100)});
  }

  std::stable_sort(normalized.begin(), normalized.end(),
                   [](const AdColorGradientStop& lhs, const AdColorGradientStop& rhs) {
                     return lhs.percent < rhs.percent;
                   });

  if (normalized.isEmpty()) {
    const QColor color = fallbackColor.isValid() ? fallbackColor : fallbackEditableColor();
    normalized = {
        AdColorGradientStop{color, 0},
        AdColorGradientStop{color, 100},
    };
  }

  return normalized;
}

ColorPickerValueModel::State ColorPickerValueModel::normalizedState(const State& state) {
  State out = state;
  out.modeOptions = normalizeModeOptions(out.modeOptions);
  if (!modeListContains(out.modeOptions, out.mode)) {
    out.mode = out.modeOptions.constFirst();
  }

  if (out.selection.isEmpty()) {
    out.selection.solidColor = transparentBaseColor(out.selection.solidColor);
    out.activeStopIndex = 0;
    if (modeListContains(out.modeOptions, Mode::Solid)) {
      out.mode = Mode::Solid;
    }
    return out;
  }

  if (out.selection.isGradient()) {
    out.selection.gradientStops =
        normalizeStops(out.selection.gradientStops, out.selection.solidColor);
    if (!modeListContains(out.modeOptions, Mode::Gradient)) {
      const int index = std::clamp(out.activeStopIndex, 0,
                                   static_cast<int>(out.selection.gradientStops.size()) - 1);
      out.selection = AdColorSelection::solid(out.selection.gradientStops.at(index).color);
      out.mode = modeListContains(out.modeOptions, Mode::Solid) ? Mode::Solid
                                                                : out.modeOptions.constFirst();
      out.activeStopIndex = 0;
      return out;
    }

    out.mode = Mode::Gradient;
    out.activeStopIndex = std::clamp(out.activeStopIndex, 0,
                                     static_cast<int>(out.selection.gradientStops.size()) - 1);
    out.selection.solidColor = out.selection.gradientStops.at(out.activeStopIndex).color;
    return out;
  }

  QColor solid = out.selection.solidColor;
  if (!solid.isValid()) {
    solid = fallbackEditableColor();
  }
  out.selection = AdColorSelection::solid(solid);
  if (out.mode == Mode::Gradient && modeListContains(out.modeOptions, Mode::Gradient)) {
    out.selection.kind = AdColorSelectionKind::Gradient;
    out.selection.gradientStops = normalizeStops({}, solid);
    out.activeStopIndex = 0;
  } else {
    out.mode =
        modeListContains(out.modeOptions, Mode::Solid) ? Mode::Solid : out.modeOptions.constFirst();
    out.activeStopIndex = 0;
  }
  return out;
}

ColorPickerValueModel::State ColorPickerValueModel::stateFromSelection(
    const AdColorSelection& selection, const QVector<Mode>& modeOptions, Mode preferredMode,
    int activeStopIndex) {
  State state;
  state.selection = selection;
  state.modeOptions = modeOptions;
  state.mode = preferredMode;
  state.activeStopIndex = activeStopIndex;
  if (selection.isGradient()) {
    state.mode = Mode::Gradient;
  } else if (selection.isSolid()) {
    state.mode = Mode::Solid;
  }
  return normalizedState(state);
}

ColorPickerValueModel::State ColorPickerValueModel::stateFromCssValue(
    const QString& cssValue, const QVector<Mode>& modeOptions, Mode preferredMode,
    int activeStopIndex, bool* ok) {
  const AdColorSelection selection = parseCssValue(cssValue, ok);
  if (ok && !*ok) {
    return State{};
  }
  return stateFromSelection(selection, modeOptions, preferredMode, activeStopIndex);
}

ColorPickerValueModel::State ColorPickerValueModel::withMode(const State& state, Mode mode) {
  State next = state;
  next.mode = mode;
  if (next.selection.isEmpty()) {
    return normalizedState(next);
  }

  if (mode == Mode::Gradient) {
    const QColor color = editableColor(state);
    next.selection = AdColorSelection::gradient(normalizeStops({}, color));
    next.selection.solidColor = color;
    next.activeStopIndex = 0;
  } else {
    next.selection = AdColorSelection::solid(editableColor(state));
    next.activeStopIndex = 0;
  }
  return normalizedState(next);
}

ColorPickerValueModel::State ColorPickerValueModel::withModeOptions(
    const State& state, const QVector<Mode>& modeOptions) {
  State next = state;
  next.modeOptions = modeOptions;
  return normalizedState(next);
}

ColorPickerValueModel::State ColorPickerValueModel::withEditableColor(const State& state,
                                                                      const QColor& color) {
  if (!color.isValid()) {
    return normalizedState(state);
  }

  State next = state;
  if (next.mode == Mode::Gradient) {
    QVector<AdColorGradientStop> stops = next.selection.isGradient()
                                             ? normalizeStops(next.selection.gradientStops, color)
                                             : normalizeStops({}, color);
    const int index = std::clamp(next.activeStopIndex, 0, static_cast<int>(stops.size()) - 1);
    stops[index].color = color;
    next.selection = AdColorSelection::gradient(stops);
    next.selection.solidColor = color;
    next.activeStopIndex = index;
  } else {
    next.selection = AdColorSelection::solid(color);
    next.activeStopIndex = 0;
  }
  return normalizedState(next);
}

ColorPickerValueModel::State ColorPickerValueModel::withGradientStopPositions(
    const State& state, const QList<double>& values) {
  if (values.isEmpty()) {
    return normalizedState(state);
  }

  QVector<int> percents;
  percents.reserve(values.size());
  for (double value : values) {
    percents.append(std::clamp(qRound(value), 0, 100));
  }
  std::sort(percents.begin(), percents.end());

  State next = normalizedState(withMode(state, Mode::Gradient));
  const QVector<AdColorGradientStop> current =
      normalizeStops(next.selection.gradientStops, editableColor(next));
  auto mixColor = [](const QColor& lhs, const QColor& rhs, double ratio) {
    const double t = std::clamp(ratio, 0.0, 1.0);
    return QColor(std::clamp(qRound(lhs.red() + (rhs.red() - lhs.red()) * t), 0, 255),
                  std::clamp(qRound(lhs.green() + (rhs.green() - lhs.green()) * t), 0, 255),
                  std::clamp(qRound(lhs.blue() + (rhs.blue() - lhs.blue()) * t), 0, 255),
                  std::clamp(qRound(lhs.alpha() + (rhs.alpha() - lhs.alpha()) * t), 0, 255));
  };
  auto sampleColorAtPercent = [&](int percent) -> QColor {
    if (current.isEmpty()) {
      return editableColor(next);
    }
    const int target = std::clamp(percent, 0, 100);
    if (target <= current.constFirst().percent) {
      return current.constFirst().color;
    }
    if (target >= current.constLast().percent) {
      return current.constLast().color;
    }
    for (int i = 0; i + 1 < current.size(); ++i) {
      const AdColorGradientStop& lhs = current.at(i);
      const AdColorGradientStop& rhs = current.at(i + 1);
      if (target < lhs.percent || target > rhs.percent) {
        continue;
      }
      const int span = rhs.percent - lhs.percent;
      if (span <= 0) {
        return rhs.color;
      }
      return mixColor(lhs.color, rhs.color, static_cast<double>(target - lhs.percent) / span);
    }
    return current.constLast().color;
  };

  QVector<int> oldToNew(current.size(), -1);
  QVector<int> newToOld(percents.size(), -1);
  QVector<bool> oldUsed(current.size(), false);
  for (int newIndex = 0; newIndex < percents.size(); ++newIndex) {
    const int percent = percents.at(newIndex);
    for (int oldIndex = 0; oldIndex < current.size(); ++oldIndex) {
      if (oldUsed.at(oldIndex) || current.at(oldIndex).percent != percent) {
        continue;
      }
      oldUsed[oldIndex] = true;
      oldToNew[oldIndex] = newIndex;
      newToOld[newIndex] = oldIndex;
      break;
    }
  }

  QVector<int> unmatchedOld;
  QVector<int> unmatchedNew;
  for (int oldIndex = 0; oldIndex < current.size(); ++oldIndex) {
    if (oldToNew.at(oldIndex) < 0) {
      unmatchedOld.append(oldIndex);
    }
  }
  for (int newIndex = 0; newIndex < percents.size(); ++newIndex) {
    if (newToOld.at(newIndex) < 0) {
      unmatchedNew.append(newIndex);
    }
  }

  int addedIndex = -1;
  int removedOldIndex = -1;
  if (unmatchedOld.size() == 1 && unmatchedNew.size() == 1) {
    oldToNew[unmatchedOld.constFirst()] = unmatchedNew.constFirst();
    newToOld[unmatchedNew.constFirst()] = unmatchedOld.constFirst();
  } else if (unmatchedOld.isEmpty() && unmatchedNew.size() == 1) {
    addedIndex = unmatchedNew.constFirst();
  } else if (unmatchedOld.size() == 1 && unmatchedNew.isEmpty()) {
    removedOldIndex = unmatchedOld.constFirst();
  } else {
    const qsizetype pairCount = std::min(unmatchedOld.size(), unmatchedNew.size());
    for (qsizetype i = 0; i < pairCount; ++i) {
      const int oldIndex = unmatchedOld.at(i);
      const int newIndex = unmatchedNew.at(i);
      oldToNew[oldIndex] = newIndex;
      newToOld[newIndex] = oldIndex;
    }
  }

  QVector<AdColorGradientStop> result;
  result.reserve(percents.size());
  const QColor fallbackColor = editableColor(next);
  for (int newIndex = 0; newIndex < percents.size(); ++newIndex) {
    AdColorGradientStop stop;
    stop.percent = percents.at(newIndex);
    const int mappedOldIndex = newToOld.at(newIndex);
    if (mappedOldIndex >= 0 && mappedOldIndex < current.size()) {
      stop.color = current.at(mappedOldIndex).color;
    } else {
      stop.color = sampleColorAtPercent(stop.percent);
    }
    if (!stop.color.isValid()) {
      stop.color = fallbackColor;
    }
    result.append(stop);
  }

  next.selection = AdColorSelection::gradient(normalizeStops(result, fallbackColor));
  int nextActiveIndex = next.activeStopIndex;
  if (addedIndex >= 0) {
    nextActiveIndex = addedIndex;
  } else if (next.activeStopIndex >= 0 && next.activeStopIndex < oldToNew.size() &&
             oldToNew.at(next.activeStopIndex) >= 0) {
    nextActiveIndex = oldToNew.at(next.activeStopIndex);
  } else if (removedOldIndex >= 0 && next.activeStopIndex >= 0) {
    if (next.activeStopIndex > removedOldIndex) {
      nextActiveIndex = next.activeStopIndex - 1;
    } else if (next.activeStopIndex == removedOldIndex) {
      nextActiveIndex = removedOldIndex;
    }
  }
  next.activeStopIndex =
      std::clamp(nextActiveIndex, 0, static_cast<int>(next.selection.gradientStops.size()) - 1);
  next.selection.solidColor = next.selection.gradientStops.at(next.activeStopIndex).color;
  next.mode = Mode::Gradient;
  return normalizedState(next);
}

ColorPickerValueModel::State ColorPickerValueModel::clearedState(const State& state) {
  State next = state;
  next.selection = AdColorSelection::empty(editableColor(state));
  next.activeStopIndex = 0;
  return normalizedState(next);
}

QColor ColorPickerValueModel::editableColor(const State& state) {
  if (state.mode == Mode::Gradient && !state.selection.gradientStops.isEmpty()) {
    const int index = std::clamp(state.activeStopIndex, 0,
                                 static_cast<int>(state.selection.gradientStops.size()) - 1);
    return state.selection.gradientStops.at(index).color;
  }
  if (state.selection.solidColor.isValid()) {
    return state.selection.solidColor;
  }
  return fallbackEditableColor();
}

QString ColorPickerValueModel::cssValue(const State& state) { return cssValue(state.selection); }

QString ColorPickerValueModel::cssValue(const AdColorSelection& selection) {
  if (selection.isEmpty()) {
    return colorToCss(transparentBaseColor(selection.solidColor));
  }
  if (selection.isGradient()) {
    QStringList parts;
    parts.reserve(selection.gradientStops.size());
    for (const AdColorGradientStop& stop :
         normalizeStops(selection.gradientStops, selection.solidColor)) {
      parts.append(QStringLiteral("%1 %2%").arg(colorToCss(stop.color)).arg(stop.percent));
    }
    return QStringLiteral("linear-gradient(90deg, %1)").arg(parts.join(QStringLiteral(", ")));
  }
  return colorToCss(selection.solidColor);
}

QString ColorPickerValueModel::formattedValue(const State& state, Format format) {
  if (state.selection.isEmpty()) {
    return QString();
  }
  if (state.mode == Mode::Gradient && !state.selection.gradientStops.isEmpty()) {
    return formattedValue(AdColorSelection::gradient(normalizeStops(state.selection.gradientStops,
                                                                    state.selection.solidColor)),
                          format, state.activeStopIndex);
  }
  return formattedValue(state.selection, format, state.activeStopIndex);
}

QString ColorPickerValueModel::formattedValue(const AdColorSelection& selection, Format format,
                                              int activeStopIndex) {
  Q_UNUSED(activeStopIndex)
  if (selection.isEmpty()) {
    return QString();
  }
  if (selection.isGradient()) {
    QStringList parts;
    const QVector<AdColorGradientStop> stops =
        normalizeStops(selection.gradientStops, selection.solidColor);
    parts.reserve(stops.size());
    for (const AdColorGradientStop& stop : stops) {
      parts.append(
          QStringLiteral("%1 %2%").arg(formattedColorString(stop.color, format)).arg(stop.percent));
    }
    return QStringLiteral("linear-gradient(90deg, %1)").arg(parts.join(QStringLiteral(", ")));
  }
  return formattedColorString(selection.solidColor, format);
}

QColor ColorPickerValueModel::parseCssColor(const QString& value, bool* ok) {
  const QString text = value.trimmed();
  QColor cssHex;
  if (parseCssHexColor(text, &cssHex)) {
    if (ok) {
      *ok = true;
    }
    return cssHex;
  }

  QColor parsed(text);
  if (parsed.isValid()) {
    if (ok) {
      *ok = true;
    }
    return parsed;
  }

  static const QRegularExpression rgbRegex(
      QStringLiteral("^rgba?\\s*\\(\\s*([0-9]{1,3})\\s*,\\s*([0-9]{1,3})\\s*,\\s*([0-9]{1,3})"
                     "(?:\\s*,\\s*([0-9]+(?:\\.[0-9]+)?%?))?\\s*\\)$"),
      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch rgbMatch = rgbRegex.match(text);
  if (rgbMatch.hasMatch()) {
    bool rOk = false;
    bool gOk = false;
    bool bOk = false;
    const int red = std::clamp(rgbMatch.captured(1).toInt(&rOk), 0, 255);
    const int green = std::clamp(rgbMatch.captured(2).toInt(&gOk), 0, 255);
    const int blue = std::clamp(rgbMatch.captured(3).toInt(&bOk), 0, 255);
    if (rOk && gOk && bOk) {
      int alpha = 255;
      if (!rgbMatch.captured(4).trimmed().isEmpty()) {
        bool aOk = false;
        const QString alphaText = rgbMatch.captured(4).trimmed();
        if (alphaText.endsWith(QLatin1Char('%'))) {
          const double percent = alphaText.left(alphaText.size() - 1).toDouble(&aOk);
          alpha = std::clamp(qRound(percent * 255.0 / 100.0), 0, 255);
        } else {
          double alphaRaw = alphaText.toDouble(&aOk);
          if (alphaRaw > 1.0) {
            alphaRaw = alphaRaw / 255.0;
          }
          alpha = std::clamp(qRound(alphaRaw * 255.0), 0, 255);
        }
        if (!aOk) {
          if (ok) {
            *ok = false;
          }
          return QColor();
        }
      }
      const QColor color(red, green, blue, alpha);
      if (ok) {
        *ok = color.isValid();
      }
      return color;
    }
  }

  static const QRegularExpression hsbRegex(
      QStringLiteral("^hsb\\s*\\(\\s*([-+]?[0-9]+(?:\\.[0-9]+)?)\\s*,\\s*([0-9]+(?:\\.[0-9]+)?)%"
                     "\\s*,\\s*([0-9]+(?:\\.[0-9]+)?)%"
                     "(?:\\s*,\\s*([0-9]+(?:\\.[0-9]+)?%?))?\\s*\\)$"),
      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch match = hsbRegex.match(text);
  if (match.hasMatch()) {
    bool hOk = false;
    bool sOk = false;
    bool bOk = false;

    const double hueRaw = match.captured(1).toDouble(&hOk);
    const double satRaw = match.captured(2).toDouble(&sOk);
    const double briRaw = match.captured(3).toDouble(&bOk);
    if (hOk && sOk && bOk) {
      double alphaRaw = 1.0;
      if (!match.captured(4).trimmed().isEmpty()) {
        bool aOk = false;
        const QString alphaText = match.captured(4).trimmed();
        if (alphaText.endsWith(QLatin1Char('%'))) {
          alphaRaw = alphaText.left(alphaText.size() - 1).toDouble(&aOk) / 100.0;
        } else {
          alphaRaw = alphaText.toDouble(&aOk);
          if (alphaRaw > 1.0) {
            alphaRaw = alphaRaw / 100.0;
          }
        }
        if (!aOk) {
          if (ok) {
            *ok = false;
          }
          return QColor();
        }
      }

      const int hue = ((qRound(hueRaw) % 360) + 360) % 360;
      const int sat = std::clamp(qRound(satRaw * 255.0 / 100.0), 0, 255);
      const int bri = std::clamp(qRound(briRaw * 255.0 / 100.0), 0, 255);
      const int alpha = std::clamp(qRound(alphaRaw * 255.0), 0, 255);
      const QColor color = QColor::fromHsv(hue, sat, bri, alpha);
      if (ok) {
        *ok = color.isValid();
      }
      return color;
    }
  }

  if (ok) {
    *ok = false;
  }
  return QColor();
}

AdColorSelection ColorPickerValueModel::parseCssValue(const QString& value, bool* ok) {
  const QString trimmed = value.trimmed();
  if (trimmed.isEmpty()) {
    if (ok) {
      *ok = true;
    }
    return AdColorSelection::empty(QColor(0, 0, 0, 0));
  }

  static const QRegularExpression gradientStopRe(
      QStringLiteral("(#(?:[0-9a-fA-F]{3,8})|rgba?\\([^\\)]+\\)|hsb\\([^\\)]+\\)|[a-zA-Z]+)"
                     "\\s*([0-9]+(?:\\.[0-9]+)?)%"),
      QRegularExpression::CaseInsensitiveOption);

  if (trimmed.startsWith(QStringLiteral("linear-gradient"), Qt::CaseInsensitive)) {
    QVector<AdColorGradientStop> stops;
    QRegularExpressionMatchIterator it = gradientStopRe.globalMatch(trimmed);
    while (it.hasNext()) {
      const QRegularExpressionMatch match = it.next();
      if (!match.hasMatch()) {
        continue;
      }
      bool colorOk = false;
      const QColor color = parseCssColor(match.captured(1).trimmed(), &colorOk);
      if (!colorOk || !color.isValid()) {
        continue;
      }
      bool percentOk = false;
      const double percent = match.captured(2).toDouble(&percentOk);
      if (!percentOk) {
        continue;
      }
      stops.append(AdColorGradientStop{color, std::clamp(qRound(percent), 0, 100)});
    }
    if (!stops.isEmpty()) {
      if (ok) {
        *ok = true;
      }
      return AdColorSelection::gradient(normalizeStops(stops, stops.constFirst().color));
    }
  }

  bool colorOk = false;
  const QColor color = parseCssColor(trimmed, &colorOk);
  if (ok) {
    *ok = colorOk && color.isValid();
  }
  return (colorOk && color.isValid()) ? AdColorSelection::solid(color) : AdColorSelection();
}

QString ColorPickerValueModel::colorToString(const QColor& color, Format format) {
  if (!color.isValid()) {
    return QString();
  }

  if (format == Format::Hex) {
    return (color.alpha() >= 255) ? colorToHexRgbLower(color) : colorToHexRgbaLower(color);
  }

  if (format == Format::Rgb) {
    if (color.alpha() >= 255) {
      return QStringLiteral("rgb(%1, %2, %3)")
          .arg(color.red())
          .arg(color.green())
          .arg(color.blue());
    }
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(formatPercent(static_cast<double>(color.alphaF())));
  }

  int hue = color.hsvHue();
  if (hue < 0) {
    hue = 0;
  }
  const int sat = qRound(static_cast<double>(color.saturationF()) * 100.0);
  const int bri = qRound(static_cast<double>(color.valueF()) * 100.0);
  if (color.alpha() >= 255) {
    return QStringLiteral("hsb(%1, %2%, %3%)").arg(hue).arg(sat).arg(bri);
  }
  return QStringLiteral("hsb(%1, %2%, %3%, %4)")
      .arg(hue)
      .arg(sat)
      .arg(bri)
      .arg(formatPercent(static_cast<double>(color.alphaF())));
}

QString ColorPickerValueModel::colorToCss(const QColor& color) {
  if (!color.isValid()) {
    return QString();
  }
  if (color.alpha() >= 255) {
    return QStringLiteral("rgb(%1,%2,%3)").arg(color.red()).arg(color.green()).arg(color.blue());
  }
  return QStringLiteral("rgba(%1,%2,%3,%4)")
      .arg(color.red())
      .arg(color.green())
      .arg(color.blue())
      .arg(formatPercent(static_cast<double>(color.alphaF())));
}

QString ColorPickerValueModel::formattedColorString(const QColor& color, Format format) {
  QString text = colorToString(color, format);
  if (format == Format::Hex) {
    text = text.toUpper();
  }
  return text;
}

}  // namespace adqt::widgets::detail
