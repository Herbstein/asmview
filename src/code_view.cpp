#include "code_view.hpp"

#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>

#include <algorithm>

#include "theme.hpp"

namespace asmview {

class Gutter : public QWidget {
public:
  explicit Gutter(CodeView *view) : QWidget(view), view_(view) {
    setMouseTracking(true);
  }

  QSize sizeHint() const override { return {view_->gutterWidth(), 0}; }

protected:
  void paintEvent(QPaintEvent *event) override { view_->paintGutter(event); }

  // The gutter is a sibling widget, so it gets its own mouse events; hovering
  // a line number has to light the line up exactly as hovering the text does.
  void mouseMoveEvent(QMouseEvent *event) override {
    view_->updateHover(
        view_->blockAtY(static_cast<int>(event->position().y())));
  }
  void leaveEvent(QEvent *) override { view_->updateHover(-1); }

  void mousePressEvent(QMouseEvent *event) override {
    const int block = view_->blockAtY(static_cast<int>(event->position().y()));
    if (block >= 0) {
      view_->gutterPressed(block, static_cast<int>(event->position().x()));
    }
  }

private:
  CodeView *view_;
};

// The strip between the text and the scrollbar. Unlike the gutter it is painted
// as a whole rather than per block, because what goes here spans runs of lines.
class RightMargin : public QWidget {
public:
  explicit RightMargin(CodeView *view) : QWidget(view), view_(view) {}

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    view_->paintRightMargin(painter, rect());
  }

private:
  CodeView *view_;
};

// A transparent overlay on the vertical scrollbar. The ribbon can only draw
// what is on screen, so on its own it cannot distinguish "the rest of this is
// one line below the fold" from "the rest of this is at the other end of the
// binary". These marks show the whole document at once.
class ScrollMarks : public QWidget {
public:
  explicit ScrollMarks(CodeView *view, QScrollBar *bar)
      : QWidget(bar),
        view_(view) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setVisible(true);
  }

protected:
  void paintEvent(QPaintEvent *) override { view_->paintMarks(); }

private:
  CodeView *view_;
};

CodeView::CodeView(QWidget *parent)
    : QPlainTextEdit(parent),
      gutter_(new Gutter(this)),
      rightMargin_(new RightMargin(this)) {
  setReadOnly(true);
  setMouseTracking(true);
  setLineWrapMode(QPlainTextEdit::NoWrap);
  setFrameShape(QFrame::NoFrame);
  setTabChangesFocus(true);
  setCenterOnScroll(true);
  setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

  marks_ = new ScrollMarks(this, verticalScrollBar());
  verticalScrollBar()->installEventFilter(this);

  connect(this,
          &QPlainTextEdit::updateRequest,
          this,
          [this](const QRect &rect, int dy) {
            if (dy != 0) {
              gutter_->scroll(0, dy);
            } else {
              gutter_->update(0, rect.y(), gutter_->width(), rect.height());
            }
            rightMargin_->update();
            refreshBands();
          });
  connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
    const int block = textCursor().blockNumber();
    if (block == lastBlock_) {
      return;
    }
    lastBlock_ = block;
    emit blockActivated(block);
  });
  connect(this, &QPlainTextEdit::textChanged, this, [this] {
    lastBlock_ = -1;
    updateGutterGeometry();
  });

  updateGutterGeometry();
}

int CodeView::currentBlockNumber() const { return textCursor().blockNumber(); }

int CodeView::characterWidth(int count) const {
  return fontMetrics().horizontalAdvance(QLatin1Char('0')) * count;
}

void CodeView::goToBlock(int blockNumber) {
  const QTextBlock block = document()->findBlockByNumber(blockNumber);
  if (!block.isValid()) {
    return;
  }

  // Deliberately *not* suppressing blockActivated: jumping here from the
  // function list or the inline stack has to drag the other pane along too.
  // The panes are kept from ping-ponging by the window, not by this.
  setTextCursor(QTextCursor(block));

  const QRectF geometry =
      blockBoundingGeometry(block).translated(contentOffset());
  if (geometry.top() < 0 || geometry.bottom() > viewport()->height()) {
    centerCursor();
  }
  refreshBands();
}

QColor CodeView::blockBackground(int) const { return {}; }

int CodeView::gutterWidth() const { return characterWidth(6); }

int CodeView::rightMarginWidth() const { return 0; }

void CodeView::paintOverlay(QPainter &) {}

void CodeView::paintRightMargin(QPainter &painter, const QRect &rect) {
  painter.fillRect(rect, theme::palette().gutterBackground);
}

QVector<CodeView::VisibleBlock> CodeView::visibleBlocks() const {
  QVector<VisibleBlock> blocks;
  const int bottom = viewport()->height();
  for (QTextBlock block = firstVisibleBlock(); block.isValid();
       block = block.next()) {
    const QRectF geometry =
        blockBoundingGeometry(block).translated(contentOffset());
    if (geometry.top() > bottom) {
      break;
    }
    if (block.isVisible()) {
      blocks.append(VisibleBlock{block.blockNumber(),
                                 qRound(geometry.top()),
                                 qRound(geometry.height())});
    }
  }
  return blocks;
}

void CodeView::paintEvent(QPaintEvent *event) {
  QPlainTextEdit::paintEvent(event);
  QPainter painter(viewport());
  paintOverlay(painter);
}

// The selection, drawn around the code rather than on it: a bar down the left
// edge of every selected run, closed off at top and bottom. Together with the
// matching rail in the gutter it brackets the region without touching a single
// character's background, which is the only way a line can keep the colour that
// says where it came from.
void CodeView::paintBracket(QPainter &painter,
                            const QVector<Span> &blockRuns,
                            const QColor &color,
                            int width) const {
  if (blockRuns.isEmpty() || !color.isValid()) {
    return;
  }
  const int right = viewport()->width();
  for (const VisibleBlock &visible : visibleBlocks()) {
    for (const Span &run : blockRuns) {
      if (visible.block < run.first || visible.block > run.last) {
        continue;
      }
      painter.fillRect(QRect(0, visible.top, width, visible.height), color);
      if (visible.block == run.first) {
        painter.fillRect(QRect(0, visible.top, right, 1), color);
      }
      if (visible.block == run.last) {
        painter.fillRect(QRect(0, visible.top + visible.height - 1, right, 1),
                         color);
      }
      break;
    }
  }
}

// Bands are painted as extra selections rather than block formats, and only for
// what is on screen: a full binary is a six-figure number of lines, and nothing
// off screen needs a colour.
void CodeView::refreshBands() {
  if (refreshing_) {
    return;
  }
  refreshing_ = true;

  QList<QTextEdit::ExtraSelection> selections;
  const int bottom = viewport()->height();
  for (QTextBlock block = firstVisibleBlock(); block.isValid();
       block = block.next()) {
    if (blockBoundingGeometry(block).translated(contentOffset()).top() >
        bottom) {
      break;
    }
    const QColor color = blockBackground(block.blockNumber());
    if (color.isValid()) {
      QTextEdit::ExtraSelection selection;
      selection.format.setBackground(color);
      selection.format.setProperty(QTextFormat::FullWidthSelection, true);
      selection.cursor = QTextCursor(block);
      selection.cursor.clearSelection();
      selections.append(selection);
    }
  }
  setExtraSelections(selections);
  gutter_->update();
  rightMargin_->update();

  refreshing_ = false;
  announceViewChange();
}

// Only when the visible range actually moved: updateRequest also fires for the
// caret blinking, and repainting both ribbons twice a second for that is waste.
void CodeView::announceViewChange() {
  const int first = firstVisibleBlock().blockNumber();
  const int height = viewport()->height();
  if (first == lastFirstVisible_ && height == lastViewportHeight_) {
    return;
  }
  lastFirstVisible_ = first;
  lastViewportHeight_ = height;
  emit viewChanged();
}

void CodeView::gutterPressed(int, int) {}

QRectF CodeView::blockRect(int blockNumber) const {
  const int bottom = viewport()->height();
  for (QTextBlock block = firstVisibleBlock(); block.isValid();
       block = block.next()) {
    const QRectF geometry =
        blockBoundingGeometry(block).translated(contentOffset());
    if (geometry.top() > bottom) {
      break;
    }
    if (block.blockNumber() == blockNumber) {
      return geometry;
    }
  }
  return {};
}

bool CodeView::blockAbove(int blockNumber) const {
  return blockNumber < firstVisibleBlock().blockNumber();
}

int CodeView::blockAtY(int y) const {
  const int bottom = viewport()->height();
  for (QTextBlock block = firstVisibleBlock(); block.isValid();
       block = block.next()) {
    const QRectF geometry =
        blockBoundingGeometry(block).translated(contentOffset());
    if (geometry.top() > bottom) {
      break;
    }
    if (y >= geometry.top() && y < geometry.bottom()) {
      return block.blockNumber();
    }
  }
  return -1;
}

void CodeView::updateHover(int blockNumber) {
  if (blockNumber == hoveredBlock_) {
    return;
  }
  hoveredBlock_ = blockNumber;
  emit blockHovered(blockNumber);
  refreshBands();
}

void CodeView::setMarks(const QVector<Span> &runs,
                        int anchor,
                        const QColor &color) {
  markRuns_ = runs;
  markAnchor_ = anchor;
  markColor_ = color;
  marks_->update();
}

void CodeView::paintMarks() {
  QPainter painter(marks_);
  if (markRuns_.isEmpty() || !markColor_.isValid()) {
    return;
  }
  const int blocks = document()->blockCount();
  if (blocks <= 0) {
    return;
  }

  // The scrollbar's arrows and margins are not worth modelling exactly; the
  // marks only have to be right to within a few pixels of the handle.
  const qreal usable = marks_->height();
  const int left = marks_->width() / 4;
  const int width = qMax(3, marks_->width() / 2);

  QColor fill = markColor_;
  fill.setAlphaF(0.75f);
  for (const Span &run : std::as_const(markRuns_)) {
    const qreal top = usable * run.first / blocks;
    const qreal bottom = usable * (run.last + 1) / blocks;
    painter.fillRect(QRectF(left, top, width, qMax(2.0, bottom - top)), fill);
  }
  if (markAnchor_ >= 0) {
    const qreal y = usable * markAnchor_ / blocks;
    painter.fillRect(QRectF(0, y - 1, marks_->width(), 3.0), markColor_);
  }
}

void CodeView::mouseMoveEvent(QMouseEvent *event) {
  QPlainTextEdit::mouseMoveEvent(event);
  updateHover(blockAtY(static_cast<int>(event->position().y())));
}

void CodeView::leaveEvent(QEvent *event) {
  QPlainTextEdit::leaveEvent(event);
  updateHover(-1);
}

bool CodeView::eventFilter(QObject *watched, QEvent *event) {
  if (watched == verticalScrollBar() &&
      (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
    marks_->setGeometry(verticalScrollBar()->rect());
    marks_->raise();
  }
  return QPlainTextEdit::eventFilter(watched, event);
}

void CodeView::resizeEvent(QResizeEvent *event) {
  QPlainTextEdit::resizeEvent(event);
  updateGutterGeometry();
  updateMarginGeometry(); // the viewport has only just settled
  // The scrollbar may have appeared or changed height without sending us a
  // resize of its own, and marks pinned to a stale rectangle are worse than
  // no marks at all.
  marks_->setGeometry(verticalScrollBar()->rect());
  marks_->raise();
  refreshBands();
}

void CodeView::updateGutterGeometry() {
  const int width = gutterWidth();
  const int right = rightMarginWidth();
  setViewportMargins(width, 0, right, 0);
  gutter_->setGeometry(QRect(contentsRect().left(),
                             contentsRect().top(),
                             width,
                             contentsRect().height()));
  updateMarginGeometry();
}

// Anchored to the viewport rather than to the frame, because the strip is
// exactly the space setViewportMargins just reserved -- the scrollbar lives
// beyond it and its width is the style's business, not ours.
void CodeView::updateMarginGeometry() {
  const int width = rightMarginWidth();
  rightMargin_->setVisible(width > 0);
  if (width <= 0) {
    return;
  }
  const QRect view = viewport()->geometry();
  rightMargin_->setGeometry(
      QRect(view.right() + 1, view.top(), width, view.height()));
  rightMargin_->raise();
}

void CodeView::paintGutter(QPaintEvent *event) {
  QPainter painter(gutter_);
  painter.fillRect(event->rect(), theme::palette().gutterBackground);

  QTextBlock block = firstVisibleBlock();
  int top =
      qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
  while (block.isValid() && top <= event->rect().bottom()) {
    const int height = qRound(blockBoundingRect(block).height());
    if (block.isVisible() && top + height >= event->rect().top()) {
      const QRect rect(0, top, gutter_->width(), height);
      // Carry the line's band across the gutter as well, so a highlighted
      // line reads as one unbroken stripe from the numbers to the text.
      const QColor background = blockBackground(block.blockNumber());
      if (background.isValid()) {
        painter.fillRect(rect, background);
      }
      paintGutterBlock(painter, block.blockNumber(), rect);
    }
    top += height;
    block = block.next();
  }
}

// -- source -----------------------------------------------------------------

namespace {

QSet<int> toSet(const QVector<int> &values) {
  QSet<int> set;
  set.reserve(values.size());
  for (const int value : values) {
    set.insert(value);
  }
  return set;
}

} // namespace

void SourceView::setAnalysis(AnalysisPtr analysis) {
  analysis_ = std::move(analysis);
}

void SourceView::setFile(int fileId, const FileColors &colors) {
  fileId_ = fileId;
  colors_ = colors;
  active_.clear();
  updateGutterGeometry();
  refreshBands();
}

void SourceView::setActiveLines(const QVector<int> &lines) {
  const QSet<int> next = toSet(lines);
  if (next == active_) {
    return;
  }
  active_ = next;
  refreshBands();
  viewport()->update();
}

void SourceView::setHoverLines(const QVector<int> &lines) {
  const QSet<int> next = toSet(lines);
  if (next == hover_) {
    return;
  }
  hover_ = next;
  refreshBands();
}

int SourceView::firstActiveLine() const {
  int first = -1;
  for (const int line : active_) {
    if (first < 0 || line < first) {
      first = line;
    }
  }
  return first;
}

// Note what is *not* here: selecting a line does not change its background.
// The colour is the line's identity -- it is how you recognise its instructions
// in the other pane -- and swapping it out at the moment you select is exactly
// the thing that made walking a chain feel like a light show.
QColor SourceView::blockBackground(int blockNumber) const {
  const int line = blockNumber + 1;
  if (hover_.contains(line)) {
    const QColor strong = colors_.strong.value(line);
    return strong.isValid() ? strong : theme::palette().hoverLine;
  }
  const QColor band = colors_.band.value(line);
  if (band.isValid()) {
    return band;
  }
  return blockNumber == hoveredBlock() ? theme::palette().hoverLine : QColor();
}

void SourceView::paintOverlay(QPainter &painter) {
  QVector<int> blocks;
  blocks.reserve(active_.size());
  for (const int line : active_) {
    blocks.append(line - 1);
  }
  std::sort(blocks.begin(), blocks.end());

  QVector<Span> runs;
  for (const int block : std::as_const(blocks)) {
    if (!runs.isEmpty() && block <= runs.last().last + 1) {
      runs.last().last = block;
    } else {
      runs.append(Span{block, block});
    }
  }
  paintBracket(painter, runs, theme::palette().selectionRail, 3);
}

int SourceView::gutterWidth() const { return characterWidth(11); }

void SourceView::paintGutterBlock(QPainter &painter,
                                  int blockNumber,
                                  const QRect &rect) {
  const int line = blockNumber + 1;
  const theme::Palette &p = theme::palette();
  const bool selected = active_.contains(line);

  // A swatch on the left edge repeats the line's band colour, so the link to
  // the assembly pane survives even where the band itself is off screen. On a
  // selected line it becomes the selection rail instead: that is the one
  // colour that has to stay the same all the way down a chain.
  const QColor band = colors_.band.value(line);
  if (selected || band.isValid()) {
    painter.fillRect(
        QRect(rect.left(), rect.top(), characterWidth(1) / 2, rect.height()),
        selected ? p.selectionRail : band);
  }

  int count = 0;
  if (analysis_ && fileId_ >= 0) {
    count = static_cast<int>(
        analysis_->index.value(sourceKey(fileId_, line)).size());
  }

  const QRect textRect =
      rect.adjusted(characterWidth(1), 0, -characterWidth(1) / 2, 0);
  if (count > 0) {
    painter.setPen(p.gutterText);
    painter.drawText(textRect,
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QString::number(count));
  }
  painter.setPen(selected ? p.gutterTextStrong : p.gutterText);
  painter.drawText(textRect,
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(line));
}

// -- assembly ---------------------------------------------------------------

namespace {

// Does this row have a frame in `fileId` anywhere in its inline stack?
bool touchesFile(const Row &row, int fileId) {
  for (const quint64 key : row.keys) {
    if (static_cast<int>(key >> 32) == fileId) {
      return true;
    }
  }
  return false;
}

constexpr int kMaxTicks = 6;
constexpr int kTickPitch = 4;
constexpr int kMinFoldRun = 4;
constexpr int kMaxLoopColumns = 5;
constexpr int kLoopPitch = 7;
// A region shredded by a couple of interleaved foreign instructions is still
// one region to the eye. The background stays honest about the breaks; the
// rail closes gaps this small so the shape survives.
constexpr int kRailBridge = 3;

} // namespace

void AsmView::setAnalysis(AnalysisPtr analysis) {
  analysis_ = std::move(analysis);
  region_.clear();
  rail_.clear();
  hoverRegion_.clear();
  expanded_.clear();
  anchorRow_ = -1;

  maxDepth_ = 0;
  if (analysis_) {
    for (const Row &row : analysis_->rows) {
      maxDepth_ = qMax(maxDepth_, static_cast<int>(row.rungs.size()));
    }
  }
  rebuild();
}

void AsmView::setFile(int fileId, const FileColors &colors) {
  const bool refold = folded_ && fileId != fileId_;
  fileId_ = fileId;
  colors_ = colors;
  if (refold) {
    expanded_.clear();
    rebuild();
  } else {
    refreshBands();
  }
}

void AsmView::setFolded(bool folded) {
  if (folded_ == folded) {
    return;
  }
  folded_ = folded;
  expanded_.clear();
  rebuild();
}

// Builds the document and the block/row mapping together: once anything can be
// folded away, "block number" and "row index" stop being the same number.
void AsmView::rebuild() {
  const int previousRow = rowForBlock(currentBlockNumber());

  blockToRow_.clear();
  rowToBlock_.clear();
  markerFirstRow_.clear();
  if (!analysis_) {
    setPlainText(QString());
    return;
  }

  const QVector<Row> &rows = analysis_->rows;
  rowToBlock_.fill(-1, rows.size());

  QVector<bool> keep(rows.size(), true);
  if (folded_) {
    QVector<int> keptPerSymbol(analysis_->symbols.size(), 0);
    for (int i = 0; i < rows.size(); ++i) {
      if (rows.at(i).symbol) {
        continue;
      }
      keep[i] = touchesFile(rows.at(i), fileId_);
      if (keep.at(i) && rows.at(i).symbolIndex >= 0) {
        ++keptPerSymbol[rows.at(i).symbolIndex];
      }
    }
    // A function header earns its place if anything under it survived.
    for (int i = 0; i < rows.size(); ++i) {
      if (rows.at(i).symbol) {
        const int symbol = rows.at(i).symbolIndex;
        keep[i] = symbol >= 0 && keptPerSymbol.at(symbol) > 0;
      }
    }
  }

  const auto lineFor = [](const Row &row) {
    return row.symbol ? row.text + u':' : row.text;
  };

  QString document;
  document.reserve(rows.size() * 32);
  const auto emitRow = [&](int row) {
    rowToBlock_[row] = static_cast<int>(blockToRow_.size());
    blockToRow_.append(row);
    document += lineFor(rows.at(row));
    document += u'\n';
  };

  int i = 0;
  while (i < rows.size()) {
    if (keep.at(i)) {
      emitRow(i);
      ++i;
      continue;
    }

    const int runStart = i;
    int hidden = 0;
    while (i < rows.size() && !keep.at(i)) {
      if (!rows.at(i).symbol) {
        ++hidden;
      }
      ++i;
    }

    // A marker that hides two instructions costs a line to save a line, and
    // a column of them reads worse than the code would have. Short gaps
    // stay visible, the way context lines do in a diff.
    if (hidden < kMinFoldRun) {
      for (int row = runStart; row < i; ++row) {
        emitRow(row);
      }
      continue;
    }

    const bool open = expanded_.contains(runStart);
    markerFirstRow_.insert(static_cast<int>(blockToRow_.size()), runStart);
    blockToRow_.append(-1);
    document +=
        open ? QStringLiteral("    %1 instructions elsewhere -- click to fold")
                   .arg(hidden)
             : QStringLiteral("    %1 instructions elsewhere -- click to show")
                   .arg(hidden);
    document += u'\n';
    if (open) {
      for (int row = runStart; row < i; ++row) {
        emitRow(row);
      }
    }
  }
  if (document.endsWith(u'\n')) {
    document.chop(1);
  }

  setPlainText(document);
  updateGutterGeometry();
  if (previousRow >= 0) {
    goToRow(previousRow);
  }
}

int AsmView::rowForBlock(int blockNumber) const {
  return blockNumber >= 0 && blockNumber < blockToRow_.size()
             ? blockToRow_.at(blockNumber)
             : -1;
}

int AsmView::blockForRow(int row) const {
  return row >= 0 && row < rowToBlock_.size() ? rowToBlock_.at(row) : -1;
}

bool AsmView::isMarker(int blockNumber) const {
  return blockNumber >= 0 && blockNumber < blockToRow_.size() &&
         blockToRow_.at(blockNumber) < 0;
}

int AsmView::instructionRowAtBlock(int blockNumber) const {
  if (!analysis_) {
    return -1;
  }
  for (int block = qMax(0, blockNumber); block < blockToRow_.size(); ++block) {
    const int row = blockToRow_.at(block);
    if (row >= 0 && !analysis_->rows.at(row).symbol) {
      return row;
    }
  }
  return -1;
}

const Row *AsmView::rowAt(int blockNumber) const {
  const int row = rowForBlock(blockNumber);
  return analysis_ && row >= 0 ? &analysis_->rows.at(row) : nullptr;
}

void AsmView::goToRow(int row) {
  const int block = blockForRow(row);
  if (block >= 0) {
    goToBlock(block);
  }
}

QVector<Span> AsmView::blockRuns(const QVector<Span> &rowSpans) const {
  QVector<Span> runs;
  for (const Span &span : rowSpans) {
    // A span can be entirely folded away, or clipped at either end; walk in
    // from both sides to whatever is still in the document.
    int first = -1;
    for (int row = span.first; row <= span.last && first < 0; ++row) {
      first = blockForRow(row);
    }
    if (first < 0) {
      continue;
    }
    int last = -1;
    for (int row = span.last; row >= span.first && last < 0; --row) {
      last = blockForRow(row);
    }
    if (!runs.isEmpty() && first <= runs.last().last + 1) {
      runs.last().last = qMax(runs.last().last, last);
    } else {
      runs.append(Span{first, last});
    }
  }
  return runs;
}

int AsmView::spanIndexOf(const QVector<Span> &spans, int row) const {
  if (row < 0 || spans.isEmpty()) {
    return -1;
  }
  const auto it = std::upper_bound(
      spans.begin(),
      spans.end(),
      row,
      [](int value, const Span &span) { return value < span.first; });
  if (it == spans.begin()) {
    return -1;
  }
  const Span &candidate = *(it - 1);
  return row <= candidate.last ? static_cast<int>((it - 1) - spans.begin())
                               : -1;
}

void AsmView::setRegion(const QVector<Span> &spans, int anchorRow) {
  region_ = spans;
  anchorRow_ = anchorRow;

  rail_.clear();
  for (const Span &span : std::as_const(region_)) {
    if (!rail_.isEmpty() && span.first - rail_.last().last <= kRailBridge) {
      rail_.last().last = span.last;
    } else {
      rail_.append(span);
    }
  }
  refreshBands();
  viewport()->update(); // the bracket lives on the viewport, not in the bands
}

void AsmView::setHoverRegion(const QVector<Span> &spans, const QColor &color) {
  if (hoverRegion_ == spans && hoverColor_ == color) {
    return;
  }
  hoverRegion_ = spans;
  hoverColor_ = color;
  refreshBands();
}

// As in the source pane: selection is drawn around the code by paintOverlay,
// never into it. The only thing that changes a row's colour here is the
// pointer, and only while it is on that row.
QColor AsmView::blockBackground(int blockNumber) const {
  const int row = rowForBlock(blockNumber);
  if (row < 0 || analysis_->rows.at(row).symbol) {
    return {};
  }

  if (hoverColor_.isValid() && spanIndexOf(hoverRegion_, row) >= 0) {
    return hoverColor_;
  }

  // The band comes from wherever this instruction touches the open file, at
  // any depth -- that is the question the pane is answering.
  for (const quint64 rowKey : analysis_->rows.at(row).keys) {
    if (static_cast<int>(rowKey >> 32) == fileId_) {
      const QColor band =
          colors_.band.value(static_cast<int>(rowKey & 0xffffffffU));
      if (band.isValid()) {
        return band;
      }
      break;
    }
  }
  // Nothing to do with the open file. Acknowledge the pointer and no more:
  // lighting up a whole unrelated group under the cursor is noise, and it was
  // never a group you asked about.
  return blockNumber == hoveredBlock() ? theme::palette().hoverLine : QColor();
}

int AsmView::tickWidth() const {
  return maxDepth_ == 0 ? 0 : qMin(maxDepth_, kMaxTicks) * kTickPitch + 4;
}

// Wide enough for the anchor arrowhead to live in, since the rail is what the
// arrowhead points off.
int AsmView::railWidth() const { return 11; }

int AsmView::gutterWidth() const {
  return characterWidth(9) + tickWidth() + railWidth();
}

int AsmView::loopColumns() const {
  return analysis_ ? qMin(analysis_->maxLoopDepth, kMaxLoopColumns) : 0;
}

int AsmView::rightMarginWidth() const {
  return loopColumns() == 0 ? 0 : loopColumns() * kLoopPitch + 8;
}

void AsmView::paintOverlay(QPainter &painter) {
  paintBracket(painter, blockRuns(rail_), theme::palette().selectionRail, 3);
}

// Loop nesting, recovered from back edges and drawn as brackets that embrace
// the code: the outermost loop is the outermost bracket. After inlining this is
// usually the only structure in the pane that matches the shape of the source,
// which makes it the thing to navigate by when the inline chain has gone vague.
void AsmView::paintRightMargin(QPainter &painter, const QRect &rect) {
  const theme::Palette &p = theme::palette();
  painter.fillRect(rect, p.gutterBackground);
  if (!analysis_ || analysis_->loops.isEmpty()) {
    return;
  }

  const QVector<VisibleBlock> visible = visibleBlocks();
  if (visible.isEmpty()) {
    return;
  }
  const int firstRow = rowForBlock(visible.first().block);
  const int lastRow = rowForBlock(visible.last().block);
  if (firstRow < 0 && lastRow < 0) {
    return;
  }

  // Only the loops that reach the window, found once rather than per line.
  QVector<int> nearby;
  for (int i = 0; i < analysis_->loops.size(); ++i) {
    const Loop &loop = analysis_->loops.at(i);
    if (loop.last >= firstRow && loop.first <= qMax(firstRow, lastRow)) {
      nearby.append(i);
    }
  }

  for (const VisibleBlock &block : visible) {
    const int row = rowForBlock(block.block);
    if (row < 0) {
      continue;
    }
    for (const int index : std::as_const(nearby)) {
      const Loop &loop = analysis_->loops.at(index);
      if (row < loop.first || row > loop.last ||
          loop.depth >= kMaxLoopColumns) {
        continue;
      }
      // A loop the anchor is inside is the one you are working in.
      const bool inside = anchorRow_ >= loop.first && anchorRow_ <= loop.last;
      painter.setPen(Qt::NoPen);
      const QColor color = inside ? p.loopActive : p.loopLine;
      const int x = 3 + loop.depth * kLoopPitch;
      const int thickness = inside ? 2 : 1;
      painter.fillRect(QRect(x, block.top, thickness, block.height), color);

      // Horizontals reach back towards the code, so the bracket closes
      // around the body rather than pointing away from it.
      if (row == loop.first || row == loop.last) {
        const int y = row == loop.first ? block.top
                                        : block.top + block.height - thickness;
        painter.fillRect(QRect(0, y, x, thickness), color);
      }
      // An arrowhead on the back edge: this end is the jump, not the top.
      if (row == loop.last) {
        QPainterPath head;
        const qreal cy = block.top + block.height * 0.5;
        head.moveTo(x + thickness, cy - 3);
        head.lineTo(x + thickness + 4, cy);
        head.lineTo(x + thickness, cy + 3);
        head.closeSubpath();
        painter.fillPath(head, color);
      }
    }
  }
}

void AsmView::paintGutterBlock(QPainter &painter,
                               int blockNumber,
                               const QRect &rect) {
  const int row = rowForBlock(blockNumber);
  if (row < 0) {
    return;
  }
  const Row &instruction = analysis_->rows.at(row);
  const theme::Palette &p = theme::palette();

  // The rail: one unbroken bar down the whole selected region, capped at both
  // ends. This is what says "the thing you picked is this tall", which is the
  // question a single highlighted instruction can never answer.
  const int railSpan = spanIndexOf(rail_, row);
  const int railX = rect.left() + 1;
  if (railSpan >= 0) {
    painter.fillRect(QRect(railX, rect.top(), 2, rect.height()),
                     p.selectionRail);
    const Span &span = rail_.at(railSpan);
    if (row == span.first || row == span.last) {
      const int y = row == span.first ? rect.top() : rect.bottom() - 1;
      painter.fillRect(QRect(railX, y, railWidth() - 2, 2), p.selectionRail);
    }
  }

  // The anchor. Everything else on this rail is the same weight, so the one
  // instruction the breadcrumb is actually describing gets a solid arrowhead
  // as tall as the line, hanging off the rail and pointing at its own text.
  if (row == anchorRow_) {
    const qreal top = rect.top() + 1.0;
    const qreal bottom = rect.bottom() - 1.0;
    const qreal middle = (top + bottom) * 0.5;
    QPainterPath head;
    head.moveTo(railX, top);
    head.lineTo(railX + railWidth() - 3, middle);
    head.lineTo(railX, bottom);
    head.closeSubpath();
    painter.fillPath(head, p.selectionRail);
  }
  if (instruction.symbol) {
    return;
  }

  // One tick per depth step, outermost first, coloured by file: inline depth
  // for every instruction at once, without clicking anything.
  int x = rect.left() + railWidth() + 2;
  for (int i = 0; i < instruction.rungs.size() && i < kMaxTicks; ++i) {
    painter.fillRect(
        QRect(x, rect.top() + 1, 3, qMax(1, rect.height() - 2)),
        theme::tick(static_cast<int>(instruction.rungs.at(i) >> 32)));
    x += kTickPitch;
  }

  const QRect textRect =
      rect.adjusted(railWidth() + tickWidth(), 0, -characterWidth(1), 0);
  painter.setPen(row == anchorRow_ ? p.gutterTextStrong : p.gutterText);
  painter.drawText(
      textRect,
      Qt::AlignRight | Qt::AlignVCenter,
      QString::number(instruction.address, 16).rightJustified(6, u'0'));
}

void AsmView::gutterPressed(int blockNumber, int x) {
  const Row *row = rowAt(blockNumber);
  const int ticksLeft = railWidth();
  if (row == nullptr || row->symbol || x < ticksLeft ||
      x >= ticksLeft + tickWidth()) {
    return;
  }
  const int depth = (x - ticksLeft - 2) / kTickPitch;
  if (depth >= 0 && depth < row->rungs.size()) {
    emit depthPicked(blockNumber, depth);
  }
}

void AsmView::mousePressEvent(QMouseEvent *event) {
  const int block = blockAtY(static_cast<int>(event->position().y()));
  if (isMarker(block)) {
    const int first = markerFirstRow_.value(block, -1);
    if (first >= 0) {
      if (expanded_.contains(first)) {
        expanded_.remove(first);
      } else {
        expanded_.insert(first);
      }
      rebuild();
      return;
    }
  }
  CodeView::mousePressEvent(event);
}

void AsmView::mouseDoubleClickEvent(QMouseEvent *event) {
  const int block = blockAtY(static_cast<int>(event->position().y()));
  if (block >= 0 && !isMarker(block)) {
    emit travelRequested(block);
    return;
  }
  CodeView::mouseDoubleClickEvent(event);
}

} // namespace asmview
