// The two panes. Both are read-only text views with a custom gutter, per-line
// background bands, and a strip of marks over the scrollbar; they differ in
// what the gutter shows and where a line's colour comes from.
#pragma once

#include <QHash>
#include <QPlainTextEdit>
#include <QSet>

#include "analysis.hpp"

namespace asmview {

class Gutter;
class RightMargin;
class ScrollMarks;

// Per-line colours for one file: the calm band a line always carries, and the
// saturated version used when the pointer is on it. Neither ever means
// "selected". A line's colour is its identity across the two panes, and
// selecting draws a bracket around it rather than repainting it.
struct FileColors {
  QHash<int, QColor> band;   // source line -> colour
  QHash<int, QColor> strong; // source line -> colour
};

class CodeView : public QPlainTextEdit {
  Q_OBJECT

public:
  explicit CodeView(QWidget *parent = nullptr);

  int currentBlockNumber() const;
  int hoveredBlock() const { return hoveredBlock_; }
  void goToBlock(int blockNumber);
  void refreshBands();

  // Geometry, for the ribbon: the block's rectangle in viewport coordinates,
  // null when it is not on screen.
  QRectF blockRect(int blockNumber) const;
  bool blockAbove(int blockNumber) const;

  // Marks painted over the scrollbar, in block coordinates. They answer the
  // question the ribbon cannot: whether the rest of a highlight is just past
  // the edge of the pane or a thousand lines away.
  void setMarks(const QVector<Span> &runs, int anchor, const QColor &color);

signals:
  void blockActivated(int blockNumber);
  void blockHovered(int blockNumber); // -1 once the pointer leaves
  void viewChanged();                 // the set of visible blocks moved

protected:
  // Invalid colour means "leave this line alone". A line's background says
  // which source line it came from; selection must never touch it.
  virtual QColor blockBackground(int blockNumber) const;
  // Not pure: the constructor lays out the gutter before any subclass exists.
  virtual int gutterWidth() const;
  virtual void
  paintGutterBlock(QPainter &painter, int blockNumber, const QRect &rect) = 0;
  virtual void gutterPressed(int blockNumber, int x);

  // Drawn on the viewport after the text. This is where selection lives, so
  // that marking something never means recolouring it.
  virtual void paintOverlay(QPainter &painter);

  // A strip between the text and the scrollbar. Zero width means no strip.
  virtual int rightMarginWidth() const;
  virtual void paintRightMargin(QPainter &painter, const QRect &rect);

  // Visible blocks top to bottom, in viewport coordinates. Brackets and loop
  // markers span runs of blocks, so they cannot be drawn one block at a time
  // the way the gutter is.
  struct VisibleBlock {
    int block = 0;
    int top = 0;
    int height = 0;
  };
  QVector<VisibleBlock> visibleBlocks() const;

  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;
  int characterWidth(int count) const;
  void updateGutterGeometry();
  int blockAtY(int y) const;

  // Runs of blocks drawn as one bracket by paintOverlay.
  void paintBracket(QPainter &painter,
                    const QVector<Span> &blockRuns,
                    const QColor &color,
                    int width) const;

private:
  friend class Gutter;
  friend class RightMargin;
  friend class ScrollMarks;
  void paintGutter(QPaintEvent *event);
  void paintMarks();
  void updateHover(int blockNumber);
  void announceViewChange();
  void updateMarginGeometry();

  QWidget *gutter_ = nullptr;
  QWidget *rightMargin_ = nullptr;
  ScrollMarks *marks_ = nullptr;
  bool refreshing_ = false;
  int lastBlock_ = -1;
  int hoveredBlock_ = -1;
  int lastFirstVisible_ = -1;
  int lastViewportHeight_ = -1;

  QVector<Span> markRuns_;
  int markAnchor_ = -1;
  QColor markColor_;
};

class SourceView : public CodeView {
  Q_OBJECT

public:
  using CodeView::CodeView;

  void setAnalysis(AnalysisPtr analysis);
  void setFile(int fileId, const FileColors &colors);

  // A depth step can own several lines of one file, so both the selection and
  // the preview are sets rather than single lines.
  void setActiveLines(const QVector<int> &lines);
  void setHoverLines(const QVector<int> &lines);
  int firstActiveLine() const;

protected:
  QColor blockBackground(int blockNumber) const override;
  int gutterWidth() const override;
  void paintGutterBlock(QPainter &painter,
                        int blockNumber,
                        const QRect &rect) override;
  void paintOverlay(QPainter &painter) override;

private:
  AnalysisPtr analysis_;
  FileColors colors_;
  int fileId_ = -1;
  QSet<int> active_;
  QSet<int> hover_;
};

// The assembly pane keeps its own mapping between document blocks and analysis
// rows, because folding means the two stop being the same thing.
class AsmView : public CodeView {
  Q_OBJECT

public:
  using CodeView::CodeView;

  void setAnalysis(AnalysisPtr analysis);
  void setFile(int fileId, const FileColors &colors);
  void setFolded(bool folded);
  bool folded() const { return folded_; }

  // What the panes highlight is a region -- a set of row runs -- with one
  // instruction inside it as the anchor the breadcrumb describes.
  void setRegion(const QVector<Span> &spans, int anchorRow);
  void setHoverRegion(const QVector<Span> &spans, const QColor &color);

  int rowForBlock(int blockNumber) const;
  int blockForRow(int row) const;
  // First instruction at or after this block, in document order; -1 if none.
  int instructionRowAtBlock(int blockNumber) const;
  bool isMarker(int blockNumber) const;
  const Row *rowAt(int blockNumber) const;
  void goToRow(int row);
  // Row runs projected onto the document, skipping whatever is folded away.
  QVector<Span> blockRuns(const QVector<Span> &rowSpans) const;

signals:
  void depthPicked(int blockNumber, int depth);
  void travelRequested(int blockNumber); // double click: take me to its file

protected:
  QColor blockBackground(int blockNumber) const override;
  int gutterWidth() const override;
  void paintGutterBlock(QPainter &painter,
                        int blockNumber,
                        const QRect &rect) override;
  void gutterPressed(int blockNumber, int x) override;
  void paintOverlay(QPainter &painter) override;
  int rightMarginWidth() const override;
  void paintRightMargin(QPainter &painter, const QRect &rect) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
  void rebuild();
  int tickWidth() const;
  int railWidth() const;
  int loopColumns() const;
  // Position of a row inside the selected region: which span, and whether it
  // caps one. -1 for a row outside the region.
  int spanIndexOf(const QVector<Span> &spans, int row) const;

  AnalysisPtr analysis_;
  FileColors colors_;
  int fileId_ = -1;
  bool folded_ = false;

  QVector<int> blockToRow_; // -1 for a fold marker
  QVector<int> rowToBlock_; // -1 when the row is folded away
  QHash<int, int> markerFirstRow_;
  QSet<int> expanded_; // first row of every run the user opened back up
  int maxDepth_ = 0;

  QVector<Span> region_; // as computed: honest about every break
  QVector<Span> rail_;   // the same, with hairline gaps closed up
  QVector<Span> hoverRegion_;
  QColor hoverColor_;
  int anchorRow_ = -1;
};

} // namespace asmview
