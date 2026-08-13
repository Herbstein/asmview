// Colours, derived once from whatever palette the application ended up with,
// so the tool looks native in both light and dark desktop themes.
#pragma once

#include <QColor>
#include <QVector>

namespace asmview::theme {

struct Palette {
  bool dark = false;

  // Compiler-Explorer style line bands: one colour per source line that
  // produced code, `strong` for a line the pointer is on. These are context
  // only -- they say "different line", never "selected".
  QVector<QColor> band;
  QVector<QColor> strong;
  QVector<QColor> ticks; // saturated marks, one hue per file

  // Selecting must not repaint the code. A line's colour says which source
  // line it came from and nothing else; the selection is drawn *around* the
  // code as a bracket, in one colour that never varies.
  QColor selectionRail;   // the bracket: gutter rail, ribbon, overview marks
  QColor selectionStrong; // chrome only -- the current breadcrumb crumb
  QColor hoverLine;       // bare acknowledgement that the pointer is here

  QColor loopLine;   // loop brackets in the right margin
  QColor loopActive; // a loop the anchor instruction is inside

  // The selected instruction is in a file other than the one on screen. Loud
  // enough to be seen without looking for it, since the alternative is
  // reading the wrong file without noticing.
  QColor elsewhereBar;
  QColor elsewhereText;

  QColor gutterBackground;
  QColor gutterText;
  QColor gutterTextStrong;
  QColor dimText;

  // Assembly
  QColor symbolText;
  QColor mnemonicText;
  QColor controlFlow;
  QColor registerText;
  QColor numberText;
  QColor commentText;

  // C++
  QColor keyword;
  QColor type;
  QColor string;
  QColor comment;
  QColor preprocessor;
};

// Built on first use; requires a QApplication to exist.
const Palette &palette();

// Band colour by slot, cycling. How slots are handed out is the caller's
// problem: see MainWindow, which colours by proximity rather than in order.
int bandCount();
QColor band(int slot);
QColor strongBand(int slot);
// Stable per-file colour for the assembly gutter's depth ticks.
QColor tick(int fileId);

} // namespace asmview::theme
