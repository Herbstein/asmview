#include "main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>

#include "breadcrumb.hpp"
#include "highlighters.hpp"
#include "ribbon.hpp"
#include "theme.hpp"

namespace asmview {
namespace {

constexpr auto kSettingsGeometry = "geometry";
constexpr auto kSettingsState = "windowState";
constexpr auto kSettingsSplitter = "splitter";
constexpr auto kSettingsIntel = "intelSyntax";
constexpr auto kSettingsDeps = "includeDeps";
constexpr auto kSettingsAutoReload = "autoReload";
constexpr auto kSettingsFold = "foldToFile";

QString shortPath(const QString &path, const QString &root) {
  const QString relative = QDir(root).relativeFilePath(path);
  return relative.startsWith(u"..") ? path : relative;
}

// Two source lines this close in the file must not share a band colour.
constexpr int kSourceProximity = 10;
// Nor two that land this close together in the assembly listing, however far
// apart they are in the file: the pane is where they are read side by side.
constexpr int kAsmProximity = 24;

} // namespace

MainWindow::MainWindow(Request request, QWidget *parent)
    : QMainWindow(parent),
      request_(std::move(request)) {
  setWindowTitle(QStringLiteral("asmview"));
  buildUi();
  buildActions();

  QSettings settings;
  intelAction_->setChecked(
      settings.value(kSettingsIntel, request_.intel).toBool());
  depsAction_->setChecked(
      settings.value(kSettingsDeps, request_.includeDeps).toBool());
  autoReloadAction_->setChecked(
      settings.value(kSettingsAutoReload, true).toBool());
  foldAction_->setChecked(settings.value(kSettingsFold, false).toBool());
  request_.intel = intelAction_->isChecked();
  request_.includeDeps = depsAction_->isChecked();
  restoreGeometry(settings.value(kSettingsGeometry).toByteArray());
  restoreState(settings.value(kSettingsState).toByteArray());
  splitter_->restoreState(settings.value(kSettingsSplitter).toByteArray());

  // A rebuild replaces the binary; give the linker a moment to finish before
  // reading it back.
  rebuildTimer_ = new QTimer(this);
  rebuildTimer_->setSingleShot(true);
  rebuildTimer_->setInterval(750);
  connect(rebuildTimer_, &QTimer::timeout, this, [this] {
    if (!request_.binary.isEmpty() &&
        !watcher_->files().contains(request_.binary)) {
      watcher_->addPath(request_.binary); // the old inode is gone
    }
    if (autoReloadAction_->isChecked()) {
      reload();
    }
  });

  watcher_ = new QFileSystemWatcher(this);
  connect(watcher_, &QFileSystemWatcher::fileChanged, this, [this] {
    rebuildTimer_->start();
  });

  if (!request_.binary.isEmpty()) {
    watcher_->addPath(request_.binary);
    reload();
  } else {
    statusLeft_->setText(
        QStringLiteral("no binary loaded -- Ctrl+O to open one"));
  }
}

void MainWindow::buildUi() {
  source_ = new SourceView(this);
  asm_ = new AsmView(this);
  cppHighlighter_ = new CppHighlighter(source_->document());
  asmHighlighter_ = new AsmHighlighter(asm_->document());
  asmHighlighter_->setStyleHook([this](int blockNumber) {
    AsmHighlighter::LineStyle style;
    if (const Row *row = asm_->rowAt(blockNumber)) {
      style.symbol = row->symbol;
      style.dim = !row->symbol && row->fileId < 0;
    } else if (asm_->isMarker(blockNumber)) {
      style.dim = true;
    }
    return style;
  });

  breadcrumb_ = new Breadcrumb(this);
  auto *sourceSide = new QWidget(this);
  auto *sourceLayout = new QVBoxLayout(sourceSide);
  sourceLayout->setContentsMargins(0, 0, 0, 0);
  sourceLayout->setSpacing(0);
  sourceLayout->addWidget(breadcrumb_);
  sourceLayout->addWidget(source_, 1);

  // The ribbon rides with the assembly pane rather than sitting in the
  // splitter: a fixed-width child of a splitter would give the user two
  // handles to drag, one of which cannot move anything.
  ribbon_ = new Ribbon(source_, asm_, this);
  auto *asmSide = new QWidget(this);
  auto *asmLayout = new QHBoxLayout(asmSide);
  asmLayout->setContentsMargins(0, 0, 0, 0);
  asmLayout->setSpacing(0);
  asmLayout->addWidget(ribbon_);
  asmLayout->addWidget(asm_, 1);

  splitter_ = new QSplitter(Qt::Horizontal, this);
  splitter_->addWidget(sourceSide);
  splitter_->addWidget(asmSide);
  splitter_->setStretchFactor(0, 4);
  splitter_->setStretchFactor(1, 5);
  splitter_->setChildrenCollapsible(false);
  splitter_->setHandleWidth(1);

  // Clicking a crumb or a depth tick is an explicit request for that rung, so
  // both are allowed to change which file is open. Nothing else is.
  connect(breadcrumb_, &Breadcrumb::depthSelected, this, [this](int depth) {
    focusDepth(depth, true);
  });
  connect(asm_, &AsmView::depthPicked, this, [this](int block, int depth) {
    const int row = asm_->rowForBlock(block);
    if (row >= 0) {
      selectRow(row, false);
      focusDepth(depth, true);
    }
  });
  connect(asm_, &AsmView::travelRequested, this, [this](int block) {
    const int row = asm_->instructionRowAtBlock(block);
    if (row >= 0) {
      selectRow(row, true);
    }
  });
  connect(breadcrumb_, &Breadcrumb::followRequested, this, [this] {
    if (selectedRow_ >= 0) {
      selectRow(selectedRow_, true);
    }
  });
  connect(source_, &CodeView::viewChanged, this, &MainWindow::paintLinks);
  connect(asm_, &CodeView::viewChanged, this, &MainWindow::paintLinks);

  findEdit_ = new QLineEdit(this);
  findEdit_->setPlaceholderText(QStringLiteral("find in assembly"));
  findEdit_->setClearButtonEnabled(true);
  auto *findClose = new QAction(this);
  findClose->setShortcut(Qt::Key_Escape);
  findClose->setShortcutContext(Qt::WidgetWithChildrenShortcut);

  findBar_ = new QWidget(this);
  auto *findLayout = new QHBoxLayout(findBar_);
  findLayout->setContentsMargins(4, 2, 4, 2);
  findLayout->addWidget(findEdit_);
  findBar_->hide();
  findBar_->addAction(findClose);
  connect(findClose, &QAction::triggered, this, [this] {
    findBar_->hide();
    asm_->setFocus();
  });
  connect(findEdit_, &QLineEdit::returnPressed, this, [this] {
    findNext(QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier));
  });

  auto *central = new QWidget(this);
  auto *layout = new QVBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(splitter_, 1);
  layout->addWidget(findBar_);
  setCentralWidget(central);

  // Inline stack dock: the point of the whole tool, so it gets its own panel.
  stackList_ = new QListWidget(this);
  stackList_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  auto *stackDock = new QDockWidget(QStringLiteral("Inline stack"), this);
  stackDock->setObjectName(QStringLiteral("stackDock"));
  stackDock->setWidget(stackList_);
  addDockWidget(Qt::BottomDockWidgetArea, stackDock);
  resizeDocks({stackDock}, {150}, Qt::Vertical);
  connect(stackList_,
          &QListWidget::itemActivated,
          this,
          [this](QListWidgetItem *item) {
            const QString path = item->data(Qt::UserRole).toString();
            if (!path.isEmpty()) {
              openPath(path, item->data(Qt::UserRole + 1).toInt());
            }
          });

  statusLeft_ = new QLabel(this);
  statusRight_ = new QLabel(this);
  statusBar()->addWidget(statusLeft_, 1);
  statusBar()->addPermanentWidget(statusRight_);

  connect(source_,
          &CodeView::blockActivated,
          this,
          &MainWindow::syncFromSource);
  connect(asm_, &CodeView::blockActivated, this, &MainWindow::syncFromAsm);
  connect(source_, &CodeView::blockHovered, this, &MainWindow::hoverFromSource);
  connect(asm_, &CodeView::blockHovered, this, &MainWindow::hoverFromAsm);
}

void MainWindow::buildActions() {
  auto *toolbar = addToolBar(QStringLiteral("Main"));
  toolbar->setObjectName(QStringLiteral("mainToolBar"));
  toolbar->setMovable(false);

  auto *open = toolbar->addAction(QStringLiteral("Open"));
  open->setShortcut(QKeySequence::Open);
  connect(open, &QAction::triggered, this, &MainWindow::openBinary);

  reloadAction_ = toolbar->addAction(QStringLiteral("Reload"));
  reloadAction_->setShortcut(Qt::Key_F5);
  connect(reloadAction_, &QAction::triggered, this, &MainWindow::reload);

  toolbar->addSeparator();
  toolbar->addWidget(new QLabel(QStringLiteral(" source "), this));
  fileCombo_ = new QComboBox(this);
  fileCombo_->setMinimumWidth(280);
  fileCombo_->setSizeAdjustPolicy(
      QComboBox::AdjustToMinimumContentsLengthWithIcon);
  toolbar->addWidget(fileCombo_);
  connect(fileCombo_, &QComboBox::activated, this, [this](int index) {
    const int fileId = fileCombo_->itemData(index).toInt();
    if (fileId < 0) {
      return;
    }
    showFile(fileId);
    // Re-read the pinned instruction against the file just opened: its
    // line numbers, and possibly its rung, are different here. Without
    // this the link still points at a line number from the old file.
    if (selectedRow_ >= 0) {
      selectRow(selectedRow_, false);
    } else {
      setActiveLink({});
    }
  });

  toolbar->addSeparator();
  intelAction_ = toolbar->addAction(QStringLiteral("Intel"));
  intelAction_->setCheckable(true);
  intelAction_->setToolTip(QStringLiteral("Intel syntax instead of AT&T"));
  connect(intelAction_, &QAction::toggled, this, [this](bool on) {
    request_.intel = on;
    reload();
  });

  depsAction_ = toolbar->addAction(QStringLiteral("Deps"));
  depsAction_->setCheckable(true);
  depsAction_->setToolTip(QStringLiteral(
      "Treat bundled dependencies as project code instead of system code"));
  connect(depsAction_, &QAction::toggled, this, [this](bool on) {
    request_.includeDeps = on;
    reload();
  });

  foldAction_ = toolbar->addAction(QStringLiteral("Fold"));
  foldAction_->setCheckable(true);
  foldAction_->setToolTip(QStringLiteral(
      "Collapse assembly that has nothing to do with the open file"));
  connect(foldAction_, &QAction::toggled, this, [this](bool on) {
    asm_->setFolded(on);
    paintLinks();
  });

  autoReloadAction_ = toolbar->addAction(QStringLiteral("Auto"));
  autoReloadAction_->setCheckable(true);
  autoReloadAction_->setToolTip(
      QStringLiteral("Re-analyse when the binary is rebuilt"));

  auto *find = new QAction(this);
  find->setShortcut(QKeySequence::Find);
  connect(find, &QAction::triggered, this, [this] {
    findBar_->show();
    findEdit_->setFocus();
    findEdit_->selectAll();
  });
  addAction(find);

  auto *deeper = new QAction(this);
  deeper->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Down));
  connect(deeper, &QAction::triggered, this, [this] { stepDepth(1); });
  addAction(deeper);

  auto *shallower = new QAction(this);
  shallower->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
  connect(shallower, &QAction::triggered, this, [this] { stepDepth(-1); });
  addAction(shallower);

  auto *nextCliff = new QAction(this);
  nextCliff->setShortcut(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Down));
  connect(nextCliff, &QAction::triggered, this, [this] { stepNarrowing(1); });
  addAction(nextCliff);

  auto *previousCliff = new QAction(this);
  previousCliff->setShortcut(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Up));
  connect(previousCliff, &QAction::triggered, this, [this] {
    stepNarrowing(-1);
  });
  addAction(previousCliff);

  // Sideways: the same call path, wherever else the inliner put a copy of it.
  // In an LTO build that is often the only way to tell one of five copies
  // from another.
  auto *nextSite = new QAction(this);
  nextSite->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Right));
  connect(nextSite, &QAction::triggered, this, [this] { stepSibling(1); });
  addAction(nextSite);

  auto *previousSite = new QAction(this);
  previousSite->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
  connect(previousSite, &QAction::triggered, this, [this] { stepSibling(-1); });
  addAction(previousSite);
}

void MainWindow::closeEvent(QCloseEvent *event) {
  QSettings settings;
  settings.setValue(kSettingsGeometry, saveGeometry());
  settings.setValue(kSettingsState, saveState());
  settings.setValue(kSettingsSplitter, splitter_->saveState());
  settings.setValue(kSettingsIntel, intelAction_->isChecked());
  settings.setValue(kSettingsDeps, depsAction_->isChecked());
  settings.setValue(kSettingsAutoReload, autoReloadAction_->isChecked());
  settings.setValue(kSettingsFold, foldAction_->isChecked());
  QMainWindow::closeEvent(event);
}

void MainWindow::openBinary() {
  const QString path = QFileDialog::getOpenFileName(
      this,
      QStringLiteral("Open binary"),
      request_.binary.isEmpty() ? QDir::currentPath() : request_.binary);
  if (path.isEmpty()) {
    return;
  }
  if (!request_.binary.isEmpty()) {
    watcher_->removePath(request_.binary);
  }
  request_.binary = path;
  request_.root = guessRoot(path);
  watcher_->addPath(path);
  reload();
}

void MainWindow::reload() {
  if (running_ || request_.binary.isEmpty()) {
    return;
  }
  running_ = true;
  setBusy(true);

  auto *thread = new QThread(this);
  auto *worker = new Worker;
  worker->moveToThread(thread);
  const Request request = request_;
  connect(thread, &QThread::started, worker, [worker, request] {
    worker->run(request);
  });
  connect(worker, &Worker::finished, this, &MainWindow::onAnalysisFinished);
  connect(worker, &Worker::finished, thread, &QThread::quit);
  connect(thread, &QThread::finished, worker, &QObject::deleteLater);
  connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  thread->start();
}

void MainWindow::setBusy(bool busy) {
  reloadAction_->setEnabled(!busy);
  if (busy) {
    statusLeft_->setText(QStringLiteral("disassembling %1...")
                             .arg(QFileInfo(request_.binary).fileName()));
  }
}

void MainWindow::onAnalysisFinished(asmview::AnalysisPtr result) {
  running_ = false;
  setBusy(false);

  if (!result->error.isEmpty()) {
    statusLeft_->setText(result->error);
    if (!analysis_) { // nothing to fall back on
      QMessageBox::warning(this, QStringLiteral("asmview"), result->error);
    }
    return;
  }

  const QString previousPath = sourcePath_;
  const int previousLine = source_->currentBlockNumber() + 1;

  analysis_ = std::move(result);
  selectedRow_ = -1;
  active_ = {};
  hover_ = {};
  source_->setAnalysis(analysis_);
  breadcrumb_->setAnalysis(analysis_);
  asm_->setFolded(foldAction_->isChecked());
  asm_->setAnalysis(analysis_);

  fileCombo_->clear();
  for (int i = 0; i < analysis_->files.size(); ++i) {
    if (analysis_->files.at(i).project) {
      fileCombo_->addItem(shortPath(analysis_->files.at(i).path, request_.root),
                          i);
    }
  }

  setWindowTitle(QStringLiteral("asmview -- %1")
                     .arg(QFileInfo(analysis_->binary).fileName()));

  // Come back to where the cursor was before the rebuild; failing that, open
  // on the first line that actually produced code, so that neither pane
  // starts out parked on PLT stubs.
  int fileId = analysis_->fileId(previousPath);
  int line = previousLine;
  if (fileId < 0) {
    fileId = fileCombo_->count() > 0 ? fileCombo_->itemData(0).toInt() : -1;
    line = -1;
  }
  if (fileId >= 0) {
    if (line <= 0) {
      const QVector<int> lines = codeLines(fileId);
      line = lines.isEmpty() ? 1 : lines.first();
    }
    sourcePath_.clear(); // force a re-read: the file may have changed too
    showFile(fileId, line);
    syncFromSource(line - 1);
  }
  updateStatus();
}

QVector<int> MainWindow::codeLines(int fileId) const {
  QVector<int> lines;
  if (!analysis_ || fileId < 0) {
    return lines;
  }
  for (auto it = analysis_->index.constBegin();
       it != analysis_->index.constEnd();
       ++it) {
    if (static_cast<int>(it.key() >> 32) == fileId) {
      lines.append(static_cast<int>(it.key() & 0xffffffffU));
    }
  }
  std::sort(lines.begin(), lines.end());
  return lines;
}

// Band colours are a graph colouring, not a cycle. Cycling in source order
// only guarantees that lines a dozen apart in the *file* differ, and the pane
// where they are actually compared is the assembly one, where a line from the
// top of the file and a line from the bottom routinely end up adjacent. Two
// sections that can be seen at the same time must not share a hue.
FileColors MainWindow::colorsFor(int fileId) const {
  FileColors colors;
  const QVector<int> lines = codeLines(fileId);
  if (!analysis_ || lines.isEmpty()) {
    return colors;
  }

  QHash<int, int> node;
  node.reserve(lines.size());
  for (int i = 0; i < lines.size(); ++i) {
    node.insert(lines.at(i), i);
  }

  QVector<QSet<int>> neighbours(lines.size());
  const auto link = [&](int a, int b) {
    if (a != b) {
      neighbours[a].insert(b);
      neighbours[b].insert(a);
    }
  };

  for (int i = 0; i < lines.size(); ++i) {
    for (int j = i + 1;
         j < lines.size() && lines.at(j) - lines.at(i) <= kSourceProximity;
         ++j) {
      link(i, j);
    }
  }

  // Sweep the listing once with a window of the recent past, rather than
  // comparing every pair of lines: a file with a thousand live lines would
  // otherwise cost a million interval tests on every file switch.
  QVector<QPair<int, int>> window; // (row, node)
  for (int row = 0; row < analysis_->rows.size(); ++row) {
    while (!window.isEmpty() &&
           row - window.constFirst().first > kAsmProximity) {
      window.removeFirst();
    }
    for (const quint64 key : analysis_->rows.at(row).keys) {
      if (static_cast<int>(key >> 32) != fileId) {
        continue;
      }
      const int here = node.value(static_cast<int>(key & 0xffffffffU), -1);
      if (here < 0) {
        continue;
      }
      for (const auto &earlier : std::as_const(window)) {
        link(earlier.second, here);
      }
      window.append({row, here});
    }
  }

  const int palette = theme::bandCount();
  if (palette <= 0) {
    return colors;
  }
  QVector<int> assigned(lines.size(), -1);
  for (int i = 0; i < lines.size(); ++i) {
    QSet<int> taken;
    for (const int other : std::as_const(neighbours.at(i))) {
      if (assigned.at(other) >= 0) {
        taken.insert(assigned.at(other));
      }
    }
    // Start the search where the plain cycle would have put it, so an
    // unconstrained run of lines still walks the palette in order and looks
    // deliberate rather than random.
    int pick = i % palette;
    for (int step = 0; step < palette; ++step) {
      const int candidate = (i + step) % palette;
      if (!taken.contains(candidate)) {
        pick = candidate;
        break;
      }
    }
    assigned[i] = pick;
    colors.band.insert(lines.at(i), theme::band(pick));
    colors.strong.insert(lines.at(i), theme::strongBand(pick));
  }
  return colors;
}

void MainWindow::showFile(int fileId, int line) {
  if (!analysis_ || fileId < 0 || fileId >= analysis_->files.size()) {
    return;
  }
  const QString path = analysis_->files.at(fileId).path;
  const FileColors colors = colorsFor(fileId);
  currentColors_ = colors;

  if (path != sourcePath_) {
    QFile file(path);
    QString text;
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      QTextStream stream(&file);
      text = stream.readAll();
    } else {
      text =
          QStringLiteral("<cannot read %1: %2>").arg(path, file.errorString());
    }
    source_->setPlainText(text);
    sourcePath_ = path;
  }
  sourceFileId_ = fileId;

  source_->setFile(fileId, colors);
  asm_->setFile(fileId, colors);
  breadcrumb_->setOpenFile(fileId);
  const int comboIndex = fileCombo_->findData(fileId);
  fileCombo_->setCurrentIndex(
      comboIndex); // -1 for a system header, which is honest

  if (line > 0) {
    const bool wasSyncing = syncing_;
    syncing_ = true;
    source_->goToBlock(line - 1);
    syncing_ = wasSyncing;
  }
}

void MainWindow::openPath(const QString &path, int line) {
  if (analysis_) {
    const int fileId = analysis_->fileId(path);
    if (fileId >= 0) {
      showFile(fileId, line);
      syncFromSource(line - 1);
      return;
    }
  }

  // Not a file the symbolizer ever named: readable, but nothing links to it.
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    statusLeft_->setText(QStringLiteral("cannot read %1").arg(path));
    return;
  }
  QTextStream stream(&file);
  source_->setPlainText(stream.readAll());
  sourcePath_ = path;
  sourceFileId_ = -1;
  source_->setFile(-1, {});
  breadcrumb_->setOpenFile(-1);
  currentColors_ = {};
  fileCombo_->setCurrentIndex(-1);
  if (line > 0) {
    source_->goToBlock(line - 1);
    source_->setActiveLines({line});
  }
  updateStatus();
}

// The frame of `row` that lands in `fileId`, if the file appears in its inline
// stack at all. This is what lets an instruction be read against the file you
// are looking at rather than against wherever it was innermost.
quint64 MainWindow::keyInFile(const Row &row, int fileId, bool *found) {
  *found = false;
  if (fileId < 0) {
    return 0;
  }
  for (const quint64 key : row.keys) {
    if (static_cast<int>(key >> 32) == fileId) {
      *found = true;
      return key;
    }
  }
  return 0;
}

bool MainWindow::touchesOpenFile(int row) const {
  if (!analysis_ || row < 0 || row >= analysis_->rows.size()) {
    return false;
  }
  bool found = false;
  keyInFile(analysis_->rows.at(row), sourceFileId_, &found);
  return found;
}

// -- selection and depth ----------------------------------------------------

// Pin an instruction: the breadcrumb describes its inline stack from here on,
// and the depth starts wherever the open file sits in that stack.
void MainWindow::selectRow(int row, bool travel) {
  if (!analysis_ || row < 0 || row >= analysis_->rows.size()) {
    return;
  }
  selectedRow_ = row;
  chainSizes_ = regionSizes(*analysis_, row);
  const QVector<DepthStep> chain = depthChain(analysis_->rows.at(row));
  breadcrumb_->setStack(chain, chainSizes_, request_.root);

  int depth = -1;
  for (int i = 0; i < chain.size(); ++i) {
    if (chain.at(i).fileId == sourceFileId_) {
      depth = i;
      break;
    }
  }
  if (depth < 0) { // nothing from the open file: start at your own code
    for (int i = 0; i < chain.size(); ++i) {
      if (chain.at(i).project) {
        depth = i;
        break;
      }
    }
  }
  updateInlineStack(row);
  focusDepth(depth < 0 ? 0 : depth, travel);
}

// Stand on one rung of the chain. The region at that rung becomes the
// selection: not the one instruction you clicked, the whole stretch of
// assembly that came down the same path, which is what you were actually
// pointing at. Each step deeper narrows it.
void MainWindow::focusDepth(int depth, bool travel) {
  breadcrumb_->setDepth(depth);
  const DepthStep *step = breadcrumb_->stepAt(depth);
  if (step == nullptr || selectedRow_ < 0) {
    setActiveLink({});
    updateStatus();
    return;
  }

  const bool wasSyncing = syncing_;
  syncing_ = true;
  if (travel && step->fileId != sourceFileId_) {
    showFile(step->fileId);
  }
  if (step->fileId == sourceFileId_) {
    source_->goToBlock(step->line - 1);
  }
  syncing_ = wasSyncing;

  setActiveLink(linkForDepth(selectedRow_, depth));
  updateStatus();
}

void MainWindow::stepDepth(int delta) {
  if (breadcrumb_->count() == 0) {
    return;
  }
  focusDepth(qBound(0, breadcrumb_->depth() + delta, breadcrumb_->count() - 1),
             true);
}

// Skip past the rungs that were only a wrapper. Alt+Down still walks every
// layer, because looking at each one is the point; this is for when you already
// know the next three files are std:: plumbing and you want the step where the
// code you clicked actually stopped being most of the region.
void MainWindow::stepNarrowing(int delta) {
  if (chainSizes_.isEmpty() || delta == 0) {
    return;
  }
  const int last = static_cast<int>(chainSizes_.size()) - 1;
  for (int i = breadcrumb_->depth() + delta; i >= 0 && i <= last; i += delta) {
    if (i == 0 || chainSizes_.at(i) != chainSizes_.at(i - 1)) {
      focusDepth(i, true);
      return;
    }
  }
  stepDepth(delta); // nothing narrows in that direction; a plain step is honest
}

// Jump to the next stretch of assembly in the current selection. A region is
// usually several runs -- the same inlined code, emitted in several places, or
// one loop body split around a branch -- and stepping between them is the only
// way to compare the copies.
void MainWindow::stepSibling(int delta) {
  if (active_.spans.isEmpty()) {
    return;
  }
  int here = -1;
  for (int i = 0; i < active_.spans.size(); ++i) {
    const Span &span = active_.spans.at(i);
    if (active_.anchorRow >= span.first && active_.anchorRow <= span.last) {
      here = i;
      break;
    }
    if (active_.anchorRow < span.first) {
      here = delta > 0 ? i - 1 : i;
      break;
    }
  }
  if (here < 0) {
    here = delta > 0 ? -1 : active_.spans.size();
  }
  const int sites = static_cast<int>(active_.spans.size());
  const int next = qBound(0, here + delta, sites - 1);
  const Span span = active_.spans.at(next);
  const int depth = breadcrumb_->depth();

  // Re-anchor on the new site rather than only moving the marker: the
  // breadcrumb has to describe the instruction it is pointing at. The region
  // itself does not change -- same prefix, same spans -- so this reads as
  // moving along one selection, not as making a new one.
  syncing_ = true;
  asm_->goToRow(span.first);
  syncing_ = false;
  selectRow(span.first, false);
  focusDepth(qBound(0, depth, breadcrumb_->count() - 1), false);

  statusRight_->setText(QStringLiteral("site %1/%2   %3 instructions")
                            .arg(next + 1)
                            .arg(sites)
                            .arg(span.last - span.first + 1));
}

// -- linking ----------------------------------------------------------------

// Everything one source line produced, wherever it landed. This is the right
// answer for a question asked from the source side: a line that was inlined
// into ten call sites genuinely is in ten places.
Link MainWindow::linkForKey(quint64 key) const {
  Link link;
  if (!analysis_) {
    return link;
  }
  const int line = static_cast<int>(key & 0xffffffffU);
  if (static_cast<int>(key >> 32) == sourceFileId_) {
    link.sourceLines.append(line);
  }
  link.spans = keySpans(*analysis_, key);
  // Hue of the line it belongs to. Callers that use this as the *selection*
  // override it: the selection ribbon has to be the same colour as the
  // selection bracket, or the two stop reading as one thing.
  const QColor strong = currentColors_.strong.value(line);
  link.color = strong.isValid() ? strong : theme::palette().hoverLine;
  return link;
}

// The region at one rung of one instruction's chain. This is the right answer
// for a question asked from the assembly side: you are looking at a stretch of
// code and asking what it came from, not at an instruction.
Link MainWindow::linkForDepth(int row, int depth) const {
  Link link;
  const DepthStep *step = breadcrumb_->stepAt(depth);
  if (!analysis_ || step == nullptr) {
    return link;
  }
  link.anchorRow = row;
  link.spans = regionSpans(*analysis_, row, depth);
  link.color = theme::palette().selectionRail;
  if (step->fileId == sourceFileId_) {
    for (const quint64 key : step->keys) {
      link.sourceLines.append(static_cast<int>(key & 0xffffffffU));
    }
  }
  return link;
}

void MainWindow::setActiveLink(const Link &link) {
  active_ = link;
  source_->setActiveLines(active_.sourceLines);
  asm_->setRegion(active_.spans, active_.anchorRow);

  // The bar above the source pane carries the warning, since that is the
  // pane telling the lie: it is showing a file the selection is not in.
  const bool elsewhere = selectedRow_ >= 0 && !active_.spans.isEmpty() &&
                         active_.sourceLines.isEmpty();
  QString target;
  if (elsewhere && analysis_) {
    const DepthStep *step = breadcrumb_->stepAt(breadcrumb_->depth());
    if (step != nullptr) {
      target = QFileInfo(analysis_->path(step->fileId)).fileName();
    }
  }
  breadcrumb_->setElsewhere(elsewhere,
                            QFileInfo(sourcePath_).fileName(),
                            target);
  paintLinks();
}

void MainWindow::setHoverLink(const Link &link) {
  hover_ = link;
  source_->setHoverLines(hover_.sourceLines);
  asm_->setHoverRegion(hover_.spans, hover_.color);
  paintLinks();
}

QVector<Span> MainWindow::sourceRuns(const QVector<int> &lines) const {
  QVector<int> blocks;
  blocks.reserve(lines.size());
  for (const int line : lines) {
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
  return runs;
}

// Redraws the link from state that is already computed. Called on every scroll
// and resize, so it must not recompute what the link *is* -- only where the two
// ends of it currently are on screen.
void MainWindow::paintLinks() {
  const theme::Palette &p = theme::palette();

  const QVector<Span> activeAsm = asm_->blockRuns(active_.spans);
  const QVector<Span> activeSource = sourceRuns(active_.sourceLines);
  if (active_.empty()) {
    ribbon_->clear();
  } else {
    ribbon_->setLink(activeSource,
                     activeAsm,
                     active_.color.isValid() ? active_.color : p.selectionRail);
  }

  if (hover_.empty()) {
    ribbon_->clearHover();
  } else {
    ribbon_->setHover(sourceRuns(hover_.sourceLines),
                      asm_->blockRuns(hover_.spans),
                      hover_.color.isValid() ? hover_.color : p.selectionRail);
  }

  // The marks are the only thing that distinguishes "just past the edge" from
  // "at the other end of the binary", so they follow the selection alone --
  // a preview flickering down the scrollbar would be worse than nothing.
  asm_->setMarks(activeAsm,
                 asm_->blockForRow(active_.anchorRow),
                 p.selectionRail);
  source_->setMarks(activeSource,
                    activeSource.isEmpty() ? -1 : activeSource.first().first,
                    p.selectionRail);
}

void MainWindow::syncFromSource(int blockNumber) {
  if (syncing_ || !analysis_ || sourceFileId_ < 0) {
    return;
  }
  const int line = blockNumber + 1;
  const quint64 key = sourceKey(sourceFileId_, line);
  const QVector<int> rows = analysis_->index.value(key);
  if (rows.isEmpty()) {
    selectedRow_ = -1;
    Link link;
    link.sourceLines.append(line);
    setActiveLink(link);
    updateStatus();
    return;
  }

  syncing_ = true;
  asm_->goToRow(rows.first());
  syncing_ = false;

  selectedRow_ = rows.first();
  chainSizes_ = regionSizes(*analysis_, selectedRow_);
  const QVector<DepthStep> chain = depthChain(analysis_->rows.at(selectedRow_));
  breadcrumb_->setStack(chain, chainSizes_, request_.root);
  // Clicking a source line means "this line", so hold the depth there even
  // when the instruction was inlined deeper.
  for (int i = 0; i < chain.size(); ++i) {
    if (chain.at(i).keys.contains(key)) {
      breadcrumb_->setDepth(i);
      break;
    }
  }
  updateInlineStack(selectedRow_);

  // Key-based, not region-based: the question asked was "where did this line
  // go", and the answer is every copy of it. Stepping the breadcrumb from
  // here narrows to one path.
  Link link = linkForKey(key);
  link.anchorRow = selectedRow_;
  link.color = theme::palette().selectionRail;
  setActiveLink(link);
  updateStatus();
}

void MainWindow::syncFromAsm(int blockNumber) {
  if (syncing_ || !analysis_) {
    return;
  }
  const int row = asm_->instructionRowAtBlock(blockNumber);
  if (row < 0) {
    return;
  }
  // Never travels. Clicking an instruction that has nothing to do with the
  // open file still pins it, still redraws the breadcrumb, still fills the
  // inline stack -- it just does not throw away the file you were reading.
  // Changing file is a separate act: a crumb, or a double click.
  selectRow(row, false);
}

void MainWindow::hoverFromSource(int blockNumber) {
  if (!analysis_ || sourceFileId_ < 0 || blockNumber < 0) {
    clearHover();
    return;
  }
  const quint64 key = sourceKey(sourceFileId_, blockNumber + 1);
  if (!analysis_->index.contains(key)) {
    clearHover();
    return;
  }
  setHoverLink(linkForKey(key));
}

// Hovering never navigates and never reaches outside the open file: it previews
// a link that exists. An instruction with no frame in this file has no link to
// preview, and lighting up its group instead only says "the pointer moved".
void MainWindow::hoverFromAsm(int blockNumber) {
  if (!analysis_ || blockNumber < 0) {
    clearHover();
    return;
  }
  const Row *row = asm_->rowAt(blockNumber);
  if (row == nullptr || row->symbol) {
    clearHover();
    return;
  }

  bool found = false;
  const quint64 key = keyInFile(*row, sourceFileId_, &found);
  if (!found) {
    clearHover(); // the pane still tints the row itself, and that is enough
    return;
  }
  setHoverLink(linkForKey(key));
}

void MainWindow::clearHover() {
  if (hover_.empty()) {
    return;
  }
  setHoverLink({});
}

void MainWindow::updateInlineStack(int row) {
  stackList_->clear();
  if (!analysis_ || row < 0 || row >= analysis_->rows.size()) {
    return;
  }
  const Row &instruction = analysis_->rows.at(row);

  for (int i = 0; i < instruction.frames.size(); ++i) {
    const Frame &frame = instruction.frames.at(i);
    QString label = QStringLiteral("%1%2")
                        .arg(QString(i * 2, u' '))
                        .arg(frame.function.isEmpty() ? QStringLiteral("??")
                                                      : frame.function);
    if (!frame.file.isEmpty()) {
      label += QStringLiteral("   %1:%2")
                   .arg(shortPath(frame.file, request_.root))
                   .arg(frame.line);
    }
    auto *item = new QListWidgetItem(label);
    item->setData(Qt::UserRole, frame.file);
    item->setData(Qt::UserRole + 1, frame.line);
    if (!frame.project) {
      item->setForeground(theme::palette().dimText);
    }
    stackList_->addItem(item);
  }
}

void MainWindow::updateStatus() {
  if (!analysis_) {
    return;
  }
  const int percent =
      analysis_->instructions > 0
          ? analysis_->attributed * 100 / analysis_->instructions
          : 0;
  statusLeft_->setText(
      QStringLiteral("%1  --  %2 instructions, %3 attributed (%4%), %5 files")
          .arg(QFileInfo(analysis_->binary).fileName())
          .arg(analysis_->instructions)
          .arg(analysis_->attributed)
          .arg(percent)
          .arg(analysis_->projectFiles));

  QString right;
  if (selectedRow_ >= 0 && selectedRow_ < analysis_->rows.size()) {
    const Row &row = analysis_->rows.at(selectedRow_);
    if (row.symbolIndex >= 0) {
      const QString name = analysis_->symbols.at(row.symbolIndex).name;
      right = name.size() > 48 ? name.left(45) + QStringLiteral("...") : name;
    }

    // How big the selection is and how it is broken up: "310 in 4 runs"
    // says more about what you are looking at than an address does.
    int instructions = 0;
    for (const Span &span : std::as_const(active_.spans)) {
      instructions += span.last - span.first + 1;
    }
    if (instructions > 0) {
      right += active_.spans.size() > 1
                   ? QStringLiteral("   %1 instr in %2 runs")
                         .arg(instructions)
                         .arg(active_.spans.size())
                   : QStringLiteral("   %1 instr").arg(instructions);
    }

    const DepthStep *step = breadcrumb_->stepAt(breadcrumb_->depth());
    if (step != nullptr) {
      right += QStringLiteral("   %1:%2   depth %3/%4")
                   .arg(QFileInfo(analysis_->path(step->fileId)).fileName())
                   .arg(step->line)
                   .arg(breadcrumb_->depth() + 1)
                   .arg(breadcrumb_->count());
    } else {
      right += QStringLiteral("   <no project frame>");
    }
    // Say so rather than silently moving: the pane you were reading stays
    // put, and this is the affordance for changing that.
    if (!touchesOpenFile(selectedRow_)) {
      right += QStringLiteral("   -- not in %1; double-click to follow")
                   .arg(QFileInfo(sourcePath_).fileName());
    }
  }
  statusRight_->setText(right);
}

void MainWindow::findNext(bool backwards) {
  const QString needle = findEdit_->text();
  if (needle.isEmpty()) {
    return;
  }
  QTextDocument::FindFlags flags;
  if (backwards) {
    flags |= QTextDocument::FindBackward;
  }
  if (!asm_->find(needle, flags)) {
    // Wrap around.
    QTextCursor cursor = asm_->textCursor();
    cursor.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
    asm_->setTextCursor(cursor);
    if (!asm_->find(needle, flags)) {
      statusLeft_->setText(QStringLiteral("no match for \"%1\"").arg(needle));
    }
  }
}

} // namespace asmview
