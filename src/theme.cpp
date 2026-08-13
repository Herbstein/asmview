#include "theme.hpp"

#include <QApplication>
#include <QPalette>

#include <cmath>

namespace asmview::theme {
namespace {

// Hues spread far enough apart that neighbouring source lines never look
// related, and ordered so that the first few are the calm ones.
constexpr double kHues[] =
    {0.58, 0.09, 0.33, 0.78, 0.14, 0.50, 0.92, 0.66, 0.04, 0.27, 0.85, 0.44};

QColor mix(const QColor &a, const QColor &b, double t) {
  return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t,
                          a.greenF() * (1 - t) + b.greenF() * t,
                          a.blueF() * (1 - t) + b.blueF() * t);
}

// Distance between two hues on the wheel, where 0.5 is opposite.
double hueGap(double a, double b) {
  const double d = std::fabs(a - b);
  return qMin(d, 1.0 - d);
}

Palette build() {
  Palette p;
  const QPalette app = QApplication::palette();
  const QColor background = app.color(QPalette::Base);
  const QColor text = app.color(QPalette::Text);
  const QColor accent = app.color(QPalette::Highlight);
  p.dark = background.lightnessF() < 0.5;

  // Dark bands stay well below the text they sit behind: the syntax colours
  // are light, and a mid-brightness band swallows them.
  //
  // Any hue close enough to the accent to be mistaken for a selection is
  // dropped rather than shifted: shifting it would only crowd its neighbour,
  // and eleven distinguishable context colours are as good as twelve.
  const double accentHue = accent.saturationF() > 0.15 ? accent.hueF() : -1.0;
  for (const double hue : kHues) {
    if (accentHue >= 0 && hueGap(hue, accentHue) < 0.075) {
      continue;
    }
    p.band.append(
        QColor::fromHslF(hue, p.dark ? 0.38 : 0.62, p.dark ? 0.155 : 0.87));
    p.strong.append(
        QColor::fromHslF(hue, p.dark ? 0.50 : 0.74, p.dark ? 0.245 : 0.74));
    p.ticks.append(
        QColor::fromHslF(hue, p.dark ? 0.62 : 0.58, p.dark ? 0.58 : 0.44));
  }

  p.selectionRail = mix(background, accent, p.dark ? 1.0 : 0.95);
  p.selectionStrong = mix(background, accent, p.dark ? 0.45 : 0.38);
  p.hoverLine = mix(background, text, 0.07);

  p.loopLine = mix(background, text, p.dark ? 0.38 : 0.34);
  p.loopActive = mix(background, text, p.dark ? 0.72 : 0.68);

  // Amber, and deliberately not any hue the code can wear: this is chrome
  // saying "you are not looking at what is selected", and it has to be
  // impossible to mistake for a band.
  p.elsewhereBar =
      QColor::fromHslF(0.095, p.dark ? 0.55 : 0.70, p.dark ? 0.20 : 0.84);
  p.elsewhereText =
      QColor::fromHslF(0.095, p.dark ? 0.85 : 0.90, p.dark ? 0.68 : 0.28);

  p.gutterBackground = mix(background, text, 0.06);
  p.gutterText = mix(background, text, 0.42);
  p.gutterTextStrong = mix(background, text, 0.85);
  p.dimText = mix(background, text, 0.42);

  if (p.dark) {
    p.symbolText = QColor(0x61, 0xaf, 0xef);
    p.mnemonicText = mix(background, text, 0.95);
    p.controlFlow = QColor(0xe0, 0x6c, 0x75);
    p.registerText = QColor(0x56, 0xb6, 0xc2);
    p.numberText = QColor(0xd1, 0x9a, 0x66);
    p.commentText = QColor(0x7f, 0x84, 0x8e);
    p.keyword = QColor(0xc6, 0x78, 0xdd);
    p.type = QColor(0x56, 0xb6, 0xc2);
    p.string = QColor(0x98, 0xc3, 0x79);
    p.comment = QColor(0x7f, 0x84, 0x8e);
    p.preprocessor = QColor(0xe5, 0xc0, 0x7b);
  } else {
    p.symbolText = QColor(0x40, 0x78, 0xf2);
    p.mnemonicText = mix(background, text, 0.95);
    p.controlFlow = QColor(0xe4, 0x56, 0x49);
    p.registerText = QColor(0x01, 0x84, 0xbc);
    p.numberText = QColor(0x98, 0x68, 0x01);
    p.commentText = QColor(0xa0, 0xa1, 0xa7);
    p.keyword = QColor(0xa6, 0x26, 0xa4);
    p.type = QColor(0x01, 0x84, 0xbc);
    p.string = QColor(0x50, 0xa1, 0x4f);
    p.comment = QColor(0xa0, 0xa1, 0xa7);
    p.preprocessor = QColor(0xc1, 0x84, 0x01);
  }
  return p;
}

} // namespace

const Palette &palette() {
  static const Palette instance = build();
  return instance;
}

int bandCount() { return static_cast<int>(palette().band.size()); }

QColor band(int slot) {
  const QVector<QColor> &colors = palette().band;
  return colors.at(((slot % colors.size()) + colors.size()) % colors.size());
}

QColor strongBand(int slot) {
  const QVector<QColor> &colors = palette().strong;
  return colors.at(((slot % colors.size()) + colors.size()) % colors.size());
}

QColor tick(int fileId) {
  const QVector<QColor> &colors = palette().ticks;
  // Spread consecutive file ids apart, so neighbouring files in one stack do
  // not come out as neighbouring hues.
  const int slot = fileId * 5;
  return colors.at(((slot % colors.size()) + colors.size()) % colors.size());
}

} // namespace asmview::theme
