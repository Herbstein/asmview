// The strip between the two panes, drawing the link itself.
//
// One source line can turn into instructions scattered across half a dozen
// places in the binary. A highlight in each pane says "these are related"; the
// ribbon says how they are related, and makes the scatter visible.
//
// Two layers: the pinned selection, and a fainter preview that follows the
// pointer in either pane. Anything whose other end is off screen is drawn as a
// stub against the edge it left by, so a link is never silently absent.
#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

#include "analysis.hpp"

namespace asmview {

class CodeView;

class Ribbon : public QWidget {
  Q_OBJECT

public:
  Ribbon(CodeView *source, CodeView *assembly, QWidget *parent = nullptr);

  // Both ends are runs of blocks in their own pane's document.
  void setLink(const QVector<Span> &sourceRuns,
               const QVector<Span> &asmRuns,
               const QColor &color);
  void setHover(const QVector<Span> &sourceRuns,
                const QVector<Span> &asmRuns,
                const QColor &color);
  void clear();
  void clearHover();

  QSize sizeHint() const override { return {26, 0}; }
  QSize minimumSizeHint() const override { return {26, 0}; }

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  struct Layer {
    QVector<Span> sourceRuns;
    QVector<Span> asmRuns;
    QColor color;

    bool empty() const {
      return sourceRuns.isEmpty() || asmRuns.isEmpty() || !color.isValid();
    }
  };

  // Where a run of blocks lands in this widget's coordinates. `above` and
  // `below` say which edges it ran off, which is what turns a clipped run
  // into a stub instead of nothing.
  struct Extent {
    qreal top = 0;
    qreal bottom = 0;
    bool above = false;
    bool below = false;
    bool visible = false;
  };
  Extent extentOf(const CodeView *view, const Span &run) const;
  void paintLayer(QPainter &painter, const Layer &layer, qreal alpha);

  CodeView *source_;
  CodeView *assembly_;
  Layer active_;
  Layer hover_;
};

} // namespace asmview
