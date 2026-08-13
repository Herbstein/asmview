#include "breadcrumb.hpp"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

#include "theme.hpp"

namespace asmview {

Breadcrumb::Breadcrumb(QWidget *parent) : QWidget(parent) {
  layout_ = new QHBoxLayout(this);
  layout_->setContentsMargins(6, 2, 6, 2);
  layout_->setSpacing(2);
  layout_->addStretch(1);
}

void Breadcrumb::setAnalysis(AnalysisPtr analysis) {
  analysis_ = std::move(analysis);
}

QString Breadcrumb::pathOf(int fileId) const {
  return analysis_ ? analysis_->path(fileId) : QString();
}

void Breadcrumb::setStack(const QVector<DepthStep> &chain,
                          const QVector<int> &sizes,
                          const QString &root) {
  crumbs_ = chain;
  sizes_ = sizes;
  root_ = root;
  depth_ = crumbs_.isEmpty()
               ? -1
               : qBound(0, depth_, static_cast<int>(crumbs_.size()) - 1);
  rebuild();
}

void Breadcrumb::setOpenFile(int fileId) {
  if (openFile_ == fileId) {
    return;
  }
  openFile_ = fileId;
  rebuild();
}

void Breadcrumb::setElsewhere(bool elsewhere,
                              const QString &openFile,
                              const QString &target) {
  if (elsewhere_ == elsewhere && openFileName_ == openFile &&
      target_ == target) {
    return;
  }
  elsewhere_ = elsewhere;
  openFileName_ = openFile;
  target_ = target;

  // The bar itself changes colour, not just something inside it. It sits
  // directly above the source pane, so tinting it puts the warning in the
  // same glance as the code it is warning about.
  setAutoFillBackground(elsewhere_);
  QPalette bar = palette();
  bar.setColor(QPalette::Window, theme::palette().elsewhereBar);
  setPalette(bar);
  rebuild();
}

void Breadcrumb::setDepth(int depth) {
  const int clamped =
      crumbs_.isEmpty()
          ? -1
          : qBound(0, depth, static_cast<int>(crumbs_.size()) - 1);
  if (clamped == depth_) {
    return;
  }
  depth_ = clamped;
  rebuild();
}

const DepthStep *Breadcrumb::stepAt(int depth) const {
  return depth >= 0 && depth < crumbs_.size() ? &crumbs_.at(depth) : nullptr;
}

void Breadcrumb::rebuild() {
  while (QLayoutItem *item = layout_->takeAt(0)) {
    delete item->widget();
    delete item;
  }

  const theme::Palette &p = theme::palette();

  // Leads the bar, before the chain, because it is a statement about the
  // whole thing: what you are reading and what is selected are two different
  // files, and here is the button that resolves that.
  if (elsewhere_) {
    auto *follow = new QToolButton(this);
    follow->setText(
        target_.isEmpty()
            ? QStringLiteral("selection is elsewhere")
            : QStringLiteral("⤷ selected code is in %1").arg(target_));
    follow->setToolTip(
        QStringLiteral(
            "The instruction you picked has no line in %1.\nClick to open %2 "
            "instead (or double-click the instruction).")
            .arg(openFileName_.isEmpty() ? QStringLiteral("this file")
                                         : openFileName_,
                 target_));
    follow->setAutoRaise(true);
    follow->setCursor(Qt::PointingHandCursor);
    follow->setStyleSheet(
        QStringLiteral("QToolButton { padding: 1px 8px; border-radius: 3px; "
                       "font-weight: bold; color: %1; border: 1px solid %1; }")
            .arg(p.elsewhereText.name()));
    connect(follow, &QToolButton::clicked, this, &Breadcrumb::followRequested);
    layout_->addWidget(follow);
  }

  for (int i = 0; i < crumbs_.size(); ++i) {
    const DepthStep &frame = crumbs_.at(i);
    if (i > 0) {
      auto *arrow = new QLabel(QStringLiteral("›"), this);
      arrow->setStyleSheet(QStringLiteral("color: %1;").arg(p.dimText.name()));
      layout_->addWidget(arrow);
    }

    auto *button = new QToolButton(this);
    const QString name = QFileInfo(pathOf(frame.fileId)).fileName();
    // The line shown is the innermost one, because that is where clicking
    // the crumb lands; a run that swallowed several lines of the same file
    // says how many rather than pretending to be one.
    QString text = frame.keys.size() > 1
                       ? QStringLiteral("%1:%2 +%3")
                             .arg(name)
                             .arg(frame.line)
                             .arg(frame.keys.size() - 1)
                       : QStringLiteral("%1:%2").arg(name).arg(frame.line);
    // How much assembly this rung still covers, and whether descending into
    // it bought anything. A rung that covers exactly what its parent
    // covered is a pass-through -- the inliner went through a wrapper --
    // and it is most of why a deep stack feels like it has no useful level
    // in it. Those recede; the ones that actually narrow keep their count.
    const bool narrows =
        i == 0 || i >= sizes_.size() || sizes_.at(i) != sizes_.at(i - 1);
    if (narrows && i < sizes_.size() && sizes_.at(i) > 0) {
      text += QStringLiteral(" ·%1").arg(sizes_.at(i));
    }
    button->setText(text);

    QString tip = QStringLiteral("%1\n%2:%3")
                      .arg(frame.function,
                           QDir(root_).relativeFilePath(pathOf(frame.fileId)))
                      .arg(frame.line);
    if (i < sizes_.size() && sizes_.at(i) > 0) {
      tip += QStringLiteral("\n%1 instructions came down this path")
                 .arg(sizes_.at(i));
    }
    if (frame.fileId != openFile_) {
      tip += QStringLiteral("\nopens this file in the source pane");
    }
    button->setToolTip(tip);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);

    // The crumb you are on is the loud one, in the one colour that means
    // "selected" everywhere in the window. Frames outside the project stay
    // legible but recede, since most of a deep stack is somebody else's
    // code; crumbs that would swap the source file out are marked, because
    // changing file is the one thing here that loses your place.
    QString style =
        QStringLiteral("QToolButton { padding: 1px 6px; border-radius: 3px; ");
    if (i == depth_) {
      style += QStringLiteral("font-weight: bold; color: %1; background: %2; }")
                   .arg(p.gutterTextStrong.name(), p.selectionStrong.name());
    } else {
      style += QStringLiteral("color: %1; %2 }")
                   .arg(!narrows || !frame.project ? p.dimText.name()
                                                   : p.gutterTextStrong.name(),
                        frame.fileId == openFile_
                            ? QString()
                            : QStringLiteral("font-style: italic;"));
    }
    button->setStyleSheet(style);
    connect(button, &QToolButton::clicked, this, [this, i] {
      emit depthSelected(i);
    });
    layout_->addWidget(button);
  }
  layout_->addStretch(1);
}

} // namespace asmview
