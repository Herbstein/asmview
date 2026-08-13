// The inline stack of the selected instruction, as a path you can walk.
//
// An instruction does not come from a line, it comes from a chain of them --
// your call site, whatever that inlined, and so on down into the headers. The
// breadcrumb makes that chain the thing you navigate: outermost on the left,
// deepest on the right, and the crumb you are standing on marked.
#pragma once

#include <QVector>
#include <QWidget>

#include "analysis.hpp"

class QHBoxLayout;

namespace asmview {

class Breadcrumb : public QWidget {
  Q_OBJECT

public:
  explicit Breadcrumb(QWidget *parent = nullptr);

  void setAnalysis(AnalysisPtr analysis);
  // `sizes` is how many instructions the region at each rung covers; the
  // crumbs wear it, so the chain reads as a narrowing rather than a list.
  void setStack(const QVector<DepthStep> &chain,
                const QVector<int> &sizes,
                const QString &root);
  void setDepth(int depth);
  // The file open in the source pane, so crumbs that would move you somewhere
  // else can say so before you click them.
  void setOpenFile(int fileId);
  // The selected instruction has no frame in the open file at all, so the
  // source pane is showing something unrelated. The bar says so and offers
  // the way out, because a status line does not get looked at.
  void
  setElsewhere(bool elsewhere, const QString &openFile, const QString &target);
  int depth() const { return depth_; }
  int count() const { return crumbs_.size(); }
  const DepthStep *stepAt(int depth) const;

signals:
  void depthSelected(int depth);
  void followRequested();

private:
  void rebuild();

  QString pathOf(int fileId) const;

  QHBoxLayout *layout_ = nullptr;
  AnalysisPtr analysis_;
  QVector<DepthStep> crumbs_; // outermost first
  QVector<int> sizes_;
  QString root_;
  QString openFileName_;
  QString target_;
  int depth_ = -1;
  int openFile_ = -1;
  bool elsewhere_ = false;
};

} // namespace asmview
