#include "ribbon.hpp"

#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

#include "code_view.hpp"
#include "theme.hpp"

namespace asmview {
namespace {

// Height of the wedge drawn against an edge for a run that is off screen.
constexpr qreal kStub = 7.0;

} // namespace

Ribbon::Ribbon(CodeView *source, CodeView *assembly, QWidget *parent)
    : QWidget(parent),
      source_(source),
      assembly_(assembly) {
  setFixedWidth(26);
  setAttribute(Qt::WA_TransparentForMouseEvents);
}

void Ribbon::setLink(const QVector<Span> &sourceRuns,
                     const QVector<Span> &asmRuns,
                     const QColor &color) {
  active_ = Layer{sourceRuns, asmRuns, color};
  update();
}

void Ribbon::setHover(const QVector<Span> &sourceRuns,
                      const QVector<Span> &asmRuns,
                      const QColor &color) {
  hover_ = Layer{sourceRuns, asmRuns, color};
  update();
}

void Ribbon::clear() {
  active_ = Layer{};
  update();
}

void Ribbon::clearHover() {
  hover_ = Layer{};
  update();
}

Ribbon::Extent Ribbon::extentOf(const CodeView *view, const Span &run) const {
  Extent extent;
  const QRectF first = view->blockRect(run.first);
  const QRectF last = run.first == run.last ? first : view->blockRect(run.last);

  const QWidget *viewport = view->viewport();
  const qreal height = viewport->height();
  // A run can start above the viewport and end below it, in which case
  // neither end has a rectangle but the middle fills the pane. Deciding by
  // side rather than by "did we get a rectangle" is what keeps that case from
  // disappearing entirely.
  extent.above = first.isNull() && view->blockAbove(run.first);
  extent.below = last.isNull() && !view->blockAbove(run.last);
  const qreal top =
      first.isNull() ? (extent.above ? 0.0 : height) : first.top();
  const qreal bottom =
      last.isNull() ? (extent.below ? height : 0.0) : last.bottom();

  // The two panes do not start at the same y as the ribbon -- the breadcrumb
  // sits above one of them -- so everything is clamped to where that pane's
  // viewport actually is, not to the ribbon's own extent.
  const QPoint origin = viewport->mapToGlobal(QPoint(0, 0));
  const QPoint here = mapToGlobal(QPoint(0, 0));
  const qreal offset = origin.y() - here.y();
  const qreal low = qMax<qreal>(0.0, offset);
  const qreal high = qMax(low, qMin<qreal>(this->height(), height + offset));
  extent.top = qBound(low, top + offset, high);
  extent.bottom = qBound(extent.top, bottom + offset, high);

  // An off-screen run still gets a sliver against the edge it left by, so
  // "there is more of this above you" is visible rather than implied.
  if (extent.bottom - extent.top < kStub && (extent.above || extent.below)) {
    if (extent.above) {
      extent.bottom = extent.top + kStub;
    } else {
      extent.top = extent.bottom - kStub;
    }
  }
  extent.visible = extent.bottom > 0 && extent.top < this->height();
  return extent;
}

void Ribbon::paintLayer(QPainter &painter, const Layer &layer, qreal alpha) {
  if (layer.empty()) {
    return;
  }

  if (layer.sourceRuns.isEmpty()) {
    // The selection has no end in this file at all. Rather than drawing
    // nothing -- which reads exactly like "no selection" -- mark where the
    // instructions are and leave the left side visibly empty.
    QColor stub = layer.color;
    stub.setAlphaF(static_cast<float>(qMin(1.0, alpha * 1.4)));
    for (const Span &run : layer.asmRuns) {
      const Extent extent = extentOf(assembly_, run);
      if (extent.visible) {
        painter.fillRect(QRectF(width() * 0.62,
                                extent.top,
                                width() * 0.38,
                                qMax<qreal>(2.0, extent.bottom - extent.top)),
                         stub);
      }
    }
    return;
  }

  // The source side is usually one line or a short run; collapse it to a
  // single anchor so the ribbon fans out from one place rather than crossing
  // itself.
  Extent anchor = extentOf(source_, layer.sourceRuns.first());
  for (const Span &run : layer.sourceRuns) {
    const Extent extent = extentOf(source_, run);
    anchor.top = qMin(anchor.top, extent.top);
    anchor.bottom = qMax(anchor.bottom, extent.bottom);
    anchor.visible = anchor.visible || extent.visible;
    anchor.above = anchor.above && extent.above;
    anchor.below = anchor.below && extent.below;
  }
  if (!anchor.visible) {
    return;
  }

  QColor fill = layer.color;
  fill.setAlphaF(static_cast<float>(alpha));
  QColor edge = layer.color;
  edge.setAlphaF(static_cast<float>(qMin(1.0, alpha * 1.6)));

  const qreal left = 0;
  const qreal right = width();
  for (const Span &run : layer.asmRuns) {
    const Extent extent = extentOf(assembly_, run);
    if (!extent.visible) {
      continue;
    }

    // A ribbon rather than a line: the left edge is the source anchor, the
    // right edge is however tall that run of instructions turned out to be.
    QPainterPath path;
    path.moveTo(left, anchor.top);
    path.cubicTo(right * 0.5,
                 anchor.top,
                 left + right * 0.5,
                 extent.top,
                 right,
                 extent.top);
    path.lineTo(right, extent.bottom);
    path.cubicTo(left + right * 0.5,
                 extent.bottom,
                 right * 0.5,
                 anchor.bottom,
                 left,
                 anchor.bottom);
    path.closeSubpath();
    painter.fillPath(path, fill);

    // A clipped run gets a hard cap on the edge it ran off, so a stub is
    // legible as "continues past here" and not as a very short run.
    if (extent.above || extent.below) {
      const qreal y = extent.above ? extent.top : extent.bottom - 2;
      painter.fillRect(QRectF(right * 0.45, y, right * 0.55, 2), edge);
    }
  }
}

void Ribbon::paintEvent(QPaintEvent *event) {
  QPainter painter(this);
  painter.fillRect(event->rect(), theme::palette().gutterBackground);
  painter.setRenderHint(QPainter::Antialiasing);

  // The preview sits on top but stays quieter, and pushes the pinned link
  // back a little so the two never read as one shape.
  paintLayer(painter, active_, hover_.empty() ? 0.55 : 0.30);
  paintLayer(painter, hover_, 0.40);
}

} // namespace asmview
