#pragma once

#include <QMainWindow>

#include "analysis.hpp"
#include "code_view.hpp"

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QFileSystemWatcher;
class QSplitter;
class QTimer;

namespace asmview {

class AsmHighlighter;
class Breadcrumb;
class CppHighlighter;
class Ribbon;

// What the two panes and the ribbon are currently showing as one thing. Held
// as state rather than recomputed from the widgets, because scrolling has to
// redraw the link without changing what the link *is*.
struct Link {
  QVector<int> sourceLines; // 1-based, in the open file; empty when elsewhere
  QVector<Span> spans;      // analysis row runs
  int anchorRow = -1;       // the instruction the breadcrumb describes
  QColor color;

  bool empty() const { return sourceLines.isEmpty() && spans.isEmpty(); }
};

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(Request request, QWidget *parent = nullptr);

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  void buildUi();
  void buildActions();

  void reload();
  void openBinary();
  void onAnalysisFinished(asmview::AnalysisPtr result);
  void setBusy(bool busy);

  void showFile(int fileId, int line = -1);
  void openPath(const QString &path, int line);
  FileColors colorsFor(int fileId) const;
  QVector<int> codeLines(int fileId) const;
  static quint64 keyInFile(const Row &row, int fileId, bool *found);
  bool touchesOpenFile(int row) const;

  void syncFromSource(int blockNumber);
  void syncFromAsm(int blockNumber);
  void hoverFromSource(int blockNumber);
  void hoverFromAsm(int blockNumber);
  void clearHover();
  void updateInlineStack(int row);
  void updateStatus();

  // Pin an instruction and rebuild the chain. `travel` is what decides
  // whether the source pane is allowed to change file, and only ever comes
  // from something the user did on purpose.
  void selectRow(int row, bool travel);
  void focusDepth(int depth, bool travel);
  void stepDepth(int delta);
  void stepNarrowing(int delta); // skip the rungs that cover the same code
  void stepSibling(int delta); // the same region, somewhere else in the binary

  Link linkForKey(quint64 key) const;
  Link linkForDepth(int row, int depth) const;
  void setActiveLink(const Link &link);
  void setHoverLink(const Link &link);
  void paintLinks(); // geometry only: safe to call on every scroll
  QVector<Span> sourceRuns(const QVector<int> &lines) const;

  void findNext(bool backwards);

  Request request_;
  AnalysisPtr analysis_;

  SourceView *source_ = nullptr;
  AsmView *asm_ = nullptr;
  AsmHighlighter *asmHighlighter_ = nullptr;
  CppHighlighter *cppHighlighter_ = nullptr;

  Breadcrumb *breadcrumb_ = nullptr;
  Ribbon *ribbon_ = nullptr;
  QSplitter *splitter_ = nullptr;
  QComboBox *fileCombo_ = nullptr;
  QListWidget *stackList_ = nullptr;
  QLabel *statusLeft_ = nullptr;
  QLabel *statusRight_ = nullptr;
  QWidget *findBar_ = nullptr;
  QLineEdit *findEdit_ = nullptr;

  QAction *reloadAction_ = nullptr;
  QAction *intelAction_ = nullptr;
  QAction *depsAction_ = nullptr;
  QAction *autoReloadAction_ = nullptr;
  QAction *foldAction_ = nullptr;

  QFileSystemWatcher *watcher_ = nullptr;
  QTimer *rebuildTimer_ = nullptr;

  FileColors
      currentColors_; // colours of the open file, kept off the scroll path
  QString sourcePath_;
  int sourceFileId_ = -1;
  int selectedRow_ = -1;    // the instruction the breadcrumb describes
  QVector<int> chainSizes_; // region size at each rung of the selected chain
  Link active_;
  Link hover_;
  bool syncing_ = false;
  bool running_ = false;
};

} // namespace asmview
