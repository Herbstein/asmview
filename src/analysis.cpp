#include "analysis.hpp"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>

#include <algorithm>

namespace asmview {
namespace {

// Dependency trees that live inside the project root but are not project code.
// Treated like system headers: code inlined out of them lands on the call site.
const QSet<QString> kDepDirs = {
    QStringLiteral("vcpkg_installed"),
    QStringLiteral("vcpkg"),
    QStringLiteral("_deps"),
    QStringLiteral("third_party"),
    QStringLiteral("external"),
    QStringLiteral("buildtrees"),
};

// Runs a child process to completion, feeding it `input` while draining its
// output, so neither pipe can fill up and deadlock the pair of us.
bool runProcess(const QString &program,
                const QStringList &args,
                const QByteArray &input,
                QByteArray *out,
                QString *error) {
  QProcess proc;
  proc.setProgram(program);
  proc.setArguments(args);
  proc.start();
  if (!proc.waitForStarted(15000)) {
    *error = QStringLiteral("could not start %1: %2")
                 .arg(program, proc.errorString());
    return false;
  }

  QByteArray err;
  qint64 written = 0;
  while (written < input.size()) {
    const qint64 n = proc.write(input.constData() + written,
                                qMin<qint64>(1 << 16, input.size() - written));
    if (n <= 0) {
      break;
    }
    written += n;
    proc.waitForBytesWritten(50);
    out->append(proc.readAllStandardOutput());
    err.append(proc.readAllStandardError());
  }
  proc.closeWriteChannel();

  while (proc.waitForReadyRead(-1)) {
    out->append(proc.readAllStandardOutput());
    err.append(proc.readAllStandardError());
  }
  proc.waitForFinished(-1);
  out->append(proc.readAllStandardOutput());
  err.append(proc.readAllStandardError());

  if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
    const QString detail = QString::fromUtf8(err).trimmed();
    *error = QStringLiteral("%1 failed: %2")
                 .arg(program, detail.isEmpty() ? proc.errorString() : detail);
    return false;
  }
  return true;
}

bool isHexDigit(QChar c) {
  return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') ||
         (c >= u'A' && c <= u'F');
}

// objdump separates fields with tabs and pads with spaces; collapse all of it.
QString collapseWhitespace(QStringView text) {
  QString out;
  out.reserve(text.size());
  bool pending = false;
  for (const QChar c : text) {
    if (c.isSpace()) {
      pending = !out.isEmpty();
      continue;
    }
    if (pending) {
      out.append(u' ');
      pending = false;
    }
    out.append(c);
  }
  return out;
}

void splitInstruction(const QString &text, Row *row) {
  const qsizetype space = text.indexOf(u' ');
  if (space < 0) {
    row->mnemonic = text;
  } else {
    row->mnemonic = text.left(space);
    row->operands = text.sliced(space + 1);
  }
  row->text = row->operands.isEmpty()
                  ? row->mnemonic
                  : row->mnemonic.leftJustified(7, u' ') + u' ' + row->operands;
}

QVector<Row> parseDisassembly(const QString &text) {
  QVector<Row> rows;
  for (QStringView raw : QStringView{text}.split(u'\n')) {
    if (raw.endsWith(u'\r')) {
      raw.chop(1);
    }
    if (raw.isEmpty()) {
      continue;
    }

    // "0000000000001234 <symbol name>:"
    if (!raw.front().isSpace() && raw.endsWith(u">:")) {
      const qsizetype lt = raw.indexOf(u'<');
      const qsizetype sp = raw.indexOf(u' ');
      if (lt > 0 && sp > 0 && sp < lt) {
        bool ok = false;
        const quint64 address = raw.first(sp).toULongLong(&ok, 16);
        if (ok) {
          Row row;
          row.symbol = true;
          row.address = address;
          row.text = raw.sliced(lt + 1, raw.size() - lt - 3).toString();
          rows.append(row);
          continue;
        }
      }
    }

    // "    1234:\tmov\trax, rcx"
    qsizetype i = 0;
    while (i < raw.size() && raw[i].isSpace()) {
      ++i;
    }
    qsizetype j = i;
    while (j < raw.size() && isHexDigit(raw[j])) {
      ++j;
    }
    if (i > 0 && j > i && j < raw.size() && raw[j] == u':') {
      bool ok = false;
      const quint64 address = raw.sliced(i, j - i).toULongLong(&ok, 16);
      if (ok) {
        Row row;
        row.address = address;
        splitInstruction(collapseWhitespace(raw.sliced(j + 1)), &row);
        rows.append(row);
        continue;
      }
    }

    // Continuation of the previous instruction's comment.
    if (!rows.isEmpty() && !rows.last().symbol && raw.front().isSpace() &&
        raw.contains(u'#')) {
      rows.last().text += u"  " + collapseWhitespace(raw);
    }
  }
  return rows;
}

// llvm-symbolizer --output-style=LLVM prints, per address, alternating function
// and "file:line:column" lines, terminated by a blank line.
QVector<QVector<Frame>> parseSymbolizer(const QString &text, int expected) {
  QVector<QVector<Frame>> stacks;
  stacks.reserve(expected);

  QVector<Frame> current;
  QString function;
  bool haveFunction = false;

  for (QStringView raw : QStringView{text}.split(u'\n')) {
    const QStringView line = raw.trimmed();
    if (line.isEmpty()) {
      if (haveFunction || !current.isEmpty()) {
        stacks.append(current);
      }
      current.clear();
      haveFunction = false;
      continue;
    }
    if (!haveFunction) {
      function = line.toString();
      haveFunction = true;
      continue;
    }

    Frame frame;
    frame.function = function;
    const qsizetype lastColon = line.lastIndexOf(u':');
    const qsizetype fileColon =
        lastColon > 0 ? line.first(lastColon).lastIndexOf(u':') : -1;
    if (fileColon > 0) {
      const QStringView file = line.first(fileColon);
      bool ok = false;
      const int lineNumber =
          line.sliced(fileColon + 1, lastColon - fileColon - 1).toInt(&ok);
      if (ok && file != u"??" && !file.isEmpty()) {
        frame.file = QDir::cleanPath(file.toString());
        frame.line = lineNumber;
      }
    }
    current.append(frame);
    haveFunction = false;
  }
  if (haveFunction || !current.isEmpty()) {
    stacks.append(current);
  }
  return stacks;
}

// Recovers loops from back edges. Everything else about the source structure is
// gone by the time the linker is done, but a conditional branch to an earlier
// address in the same function is a loop and nothing else.
void detectLoops(Analysis *result) {
  QHash<quint64, int> rowAt;
  rowAt.reserve(result->rows.size());
  for (int i = 0; i < result->rows.size(); ++i) {
    if (!result->rows.at(i).symbol) {
      rowAt.insert(result->rows.at(i).address, i);
    }
  }

  QSet<quint64> seen;
  for (int i = 0; i < result->rows.size(); ++i) {
    const Row &row = result->rows.at(i);
    if (row.symbol ||
        !(row.mnemonic.startsWith(u'j') || row.mnemonic.startsWith(u"loop"))) {
      continue;
    }
    // An indirect branch has no literal target and cannot be resolved here.
    const QStringView operands = QStringView{row.operands}.trimmed();
    if (!operands.startsWith(u"0x")) {
      continue;
    }
    qsizetype end = 2;
    while (end < operands.size() && isHexDigit(operands[end])) {
      ++end;
    }
    bool ok = false;
    const quint64 target = operands.sliced(2, end - 2).toULongLong(&ok, 16);
    if (!ok || target >= row.address) {
      continue;
    }

    const int header = rowAt.value(target, -1);
    if (header < 0 || header > i ||
        result->rows.at(header).symbolIndex != row.symbolIndex) {
      continue;
    }
    // Several back edges to one header are one loop drawn once.
    const quint64 key =
        (static_cast<quint64>(static_cast<quint32>(header)) << 32) |
        static_cast<quint32>(i);
    if (seen.contains(key)) {
      continue;
    }
    seen.insert(key);

    Loop loop;
    loop.first = header;
    loop.last = i;
    result->loops.append(loop);
  }

  const auto byExtent = [](const Loop &a, const Loop &b) {
    return a.first != b.first ? a.first < b.first : a.last > b.last;
  };
  std::sort(result->loops.begin(), result->loops.end(), byExtent);

  // One rotated loop leaves several back edges whose ranges overlap by a few
  // instructions without one containing the other -- the exit paths were
  // duplicated, so each ends somewhere slightly different. Drawn literally
  // that is eight nested loops where the source has one. Overlap without
  // proper containment therefore means "the same loop, seen from another
  // edge"; only a range strictly inside another is really nested.
  for (bool merging = true; merging;) {
    merging = false;
    for (int i = 0; i < result->loops.size() && !merging; ++i) {
      for (int j = i + 1; j < result->loops.size(); ++j) {
        Loop &outer = result->loops[i];
        const Loop inner = result->loops.at(j);
        if (inner.first > outer.last) {
          break; // sorted by start: nothing later can overlap either
        }
        if (outer.first < inner.first && inner.last < outer.last) {
          continue; // genuinely nested
        }
        outer.first = qMin(outer.first, inner.first);
        outer.last = qMax(outer.last, inner.last);
        result->loops.remove(j);
        merging = true;
        break;
      }
    }
    if (merging) {
      std::sort(result->loops.begin(), result->loops.end(), byExtent);
    }
  }

  for (Loop &loop : result->loops) {
    for (int r = loop.first; r <= loop.last; ++r) {
      if (!result->rows.at(r).symbol) {
        ++loop.instructions;
      }
    }
  }

  // Columns, not a nesting count. Loop rotation and tail duplication leave
  // plenty of back edges that overlap without containing one another, and
  // treating overlap as nesting invents ten levels where there are two.
  // Assigning each loop the first column no live loop is using gives real
  // nesting its indent -- a contained loop always collides with its container
  // -- while merely overlapping loops get a column of their own instead of
  // being stacked into a fiction.
  QVector<int> columnEnd;
  for (Loop &loop : result->loops) {
    int column = 0;
    while (column < columnEnd.size() && columnEnd.at(column) >= loop.first) {
      ++column;
    }
    if (column == columnEnd.size()) {
      columnEnd.append(-1);
    }
    columnEnd[column] = loop.last;
    loop.depth = column;
    result->maxLoopDepth = qMax(result->maxLoopDepth, column + 1);
  }
}

} // namespace

QString guessRoot(const QString &binary) {
  QDir dir = QFileInfo(binary).absoluteDir();
  while (true) {
    if (dir.exists(QStringLiteral("CMakeLists.txt")) ||
        dir.exists(QStringLiteral(".git"))) {
      return dir.absolutePath();
    }
    if (!dir.cdUp()) {
      return QDir::currentPath();
    }
  }
}

QString guessBinary(const QString &startDir) {
  const QDir start(startDir);
  static const QStringList kCandidates = {
      QStringLiteral("cmake-build-relwithdebinfo/source/CPUPathTracing"),
      QStringLiteral("cmake-build-release/source/CPUPathTracing"),
      QStringLiteral("build/source/CPUPathTracing"),
  };
  for (const QString &candidate : kCandidates) {
    const QString path = start.filePath(candidate);
    if (QFileInfo(path).isExecutable()) {
      return QFileInfo(path).absoluteFilePath();
    }
  }
  return {};
}

Analysis analyze(const Request &request) {
  Analysis result;
  result.binary = request.binary;

  if (!QFileInfo::exists(request.binary)) {
    result.error = QStringLiteral("no such file: %1").arg(request.binary);
    return result;
  }

  QStringList args = {QStringLiteral("-d"),
                      QStringLiteral("-C"),
                      QStringLiteral("--no-show-raw-insn")};
  if (request.intel) {
    args << QStringLiteral("--x86-asm-syntax=intel");
  }
  if (!request.symbol.isEmpty()) {
    args << QStringLiteral("--disassemble-symbols=%1").arg(request.symbol);
  }
  args << request.binary;

  QByteArray raw;
  if (!runProcess(request.objdump, args, {}, &raw, &result.error)) {
    return result;
  }

  result.rows = parseDisassembly(QString::fromUtf8(raw));

  QByteArray addresses;
  int instructions = 0;
  for (const Row &row : std::as_const(result.rows)) {
    if (!row.symbol) {
      addresses +=
          QByteArrayLiteral("0x") + QByteArray::number(row.address, 16) + '\n';
      ++instructions;
    }
  }
  result.instructions = instructions;
  if (instructions == 0) {
    result.error = QStringLiteral("no instructions disassembled from %1 "
                                  "(stripped binary, or no such symbol?)")
                       .arg(QFileInfo(request.binary).fileName());
    return result;
  }

  QByteArray symbolized;
  if (!runProcess(request.symbolizer,
                  {QStringLiteral("--inlines"),
                   QStringLiteral("--demangle"),
                   QStringLiteral("--output-style=LLVM"),
                   QStringLiteral("--obj=%1").arg(request.binary)},
                  addresses,
                  &symbolized,
                  &result.error)) {
    return result;
  }
  const QVector<QVector<Frame>> stacks =
      parseSymbolizer(QString::fromUtf8(symbolized), instructions);

  QString rootPrefix = QFileInfo(request.root).canonicalFilePath();
  if (rootPrefix.isEmpty()) {
    rootPrefix = QDir::cleanPath(request.root);
  }
  if (!rootPrefix.endsWith(u'/')) {
    rootPrefix += u'/';
  }

  // Resolving a path is the expensive part of attribution, and the same
  // handful of paths comes back for every instruction.
  QHash<QString, int> fileIds;
  const auto fileIdFor = [&](const QString &path) {
    const auto known = fileIds.constFind(path);
    if (known != fileIds.constEnd()) {
      return known.value();
    }

    int id = -1;
    // canonicalFilePath() is empty for anything that is not a real file on
    // disk, which also drops LTO pseudo-files such as ld-temp.o.
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (!canonical.isEmpty()) {
      SourceFile file;
      file.path = canonical;
      file.project = canonical.startsWith(rootPrefix);
      if (file.project && !request.includeDeps) {
        for (const QStringView part : QStringView{canonical}.split(u'/')) {
          if (kDepDirs.contains(part.toString())) {
            file.project = false;
            break;
          }
        }
      }
      id = static_cast<int>(result.files.size());
      result.files.append(file);
    }
    fileIds.insert(path, id);
    return id;
  };

  int nextStack = 0;
  int currentSymbol = -1;
  for (int i = 0; i < result.rows.size(); ++i) {
    Row &row = result.rows[i];
    if (row.symbol) {
      currentSymbol = static_cast<int>(result.symbols.size());
      Symbol symbol;
      symbol.name = row.text;
      symbol.address = row.address;
      symbol.row = i;
      result.symbols.append(symbol);
      row.symbolIndex = currentSymbol;
      continue;
    }

    row.symbolIndex = currentSymbol;
    if (nextStack < stacks.size()) {
      row.frames = stacks[nextStack];
    }
    ++nextStack;

    for (Frame &frame : row.frames) {
      // Line 0 means the symbolizer knew the file but not the line, which
      // is nothing a source pane can point at.
      if (frame.file.isEmpty() || frame.line <= 0) {
        continue;
      }
      frame.fileId = fileIdFor(frame.file);
      if (frame.fileId < 0) {
        continue;
      }
      frame.project = result.files.at(frame.fileId).project;

      const quint64 key = sourceKey(frame.fileId, frame.line);
      if (!row.keys.contains(key)) {
        row.keys.append(key);
        ++result.files[frame.fileId].instructions;
      }
      if (frame.project && row.fileId < 0) { // innermost project frame wins
        row.fileId = frame.fileId;
        row.line = frame.line;
      }
    }

    // Index every project frame, so that a source line owns everything
    // inlined into it and not just the instructions whose innermost frame
    // it happens to be.
    for (const quint64 key : std::as_const(row.keys)) {
      result.index[key].append(i);
    }

    // The call path, one key per rung. Computed once here because every
    // repaint of the gutter and every region query would otherwise rebuild
    // the whole chain per row.
    const QVector<DepthStep> chain = depthChain(row);
    row.rungs.reserve(chain.size());
    for (const DepthStep &step : chain) {
      row.rungs.append(sourceKey(step.fileId, step.outerLine));
    }

    if (currentSymbol >= 0) {
      ++result.symbols[currentSymbol].instructions;
    }
    if (row.fileId >= 0) {
      ++result.attributed;
      if (currentSymbol >= 0) {
        Symbol &symbol = result.symbols[currentSymbol];
        ++symbol.attributed;
        if (symbol.fileId < 0) {
          symbol.fileId = row.fileId;
        }
      }
    }
  }

  detectLoops(&result);

  for (const SourceFile &file : std::as_const(result.files)) {
    if (file.project) {
      ++result.projectFiles;
    }
  }

  if (result.attributed == 0) {
    result.error =
        QStringLiteral(
            "nothing attributed to %1 -- was the binary built with -g? "
            "Try a different project root.")
            .arg(QDir::cleanPath(rootPrefix));
  }
  return result;
}

QVector<DepthStep> depthChain(const Row &row) {
  QVector<DepthStep> chain;
  // frames run innermost-first; the ladder reads the other way.
  for (auto it = row.frames.crbegin(); it != row.frames.crend(); ++it) {
    if (it->fileId < 0) {
      continue;
    }
    const quint64 key = sourceKey(it->fileId, it->line);
    if (!chain.isEmpty() && chain.last().fileId == it->fileId) {
      DepthStep &step = chain.last();
      step.function = it->function; // keep descending within the run
      step.line = it->line;
      if (!step.keys.contains(key)) {
        step.keys.append(key);
      }
      continue;
    }

    DepthStep step;
    step.fileId = it->fileId;
    step.function = it->function;
    step.line = it->line;
    step.outerLine = it->line;
    step.project = it->project;
    step.keys.append(key);
    chain.append(step);
  }
  return chain;
}

namespace {

// Collects sorted, ascending row indices into inclusive runs. A symbol row
// always breaks a run: a region never spans two functions.
QVector<Span> runsOf(const Analysis &analysis, const QVector<int> &rows) {
  QVector<Span> spans;
  for (const int row : rows) {
    if (!spans.isEmpty() && row == spans.last().last + 1 &&
        !analysis.rows.at(row).symbol) {
      spans.last().last = row;
    } else {
      spans.append(Span{row, row});
    }
  }
  return spans;
}

} // namespace

QVector<Span> regionSpans(const Analysis &analysis, int row, int depth) {
  if (row < 0 || row >= analysis.rows.size() || depth < 0) {
    return {};
  }
  const QVector<quint64> &want = analysis.rows.at(row).rungs;
  if (depth >= want.size()) {
    return {};
  }
  const int rungs = depth + 1;

  QVector<Span> spans;
  bool open = false;
  for (int i = 0; i < analysis.rows.size(); ++i) {
    const Row &candidate = analysis.rows.at(i);
    bool same = !candidate.symbol && candidate.rungs.size() >= rungs;
    for (int r = 0; same && r < rungs; ++r) {
      same = candidate.rungs.at(r) == want.at(r);
    }
    if (!same) {
      open = false;
      continue;
    }
    if (open) {
      spans.last().last = i;
    } else {
      spans.append(Span{i, i});
      open = true;
    }
  }
  return spans;
}

QVector<Span> keySpans(const Analysis &analysis, quint64 key) {
  return runsOf(analysis, analysis.index.value(key));
}

QVector<int> regionSizes(const Analysis &analysis, int row) {
  if (row < 0 || row >= analysis.rows.size()) {
    return {};
  }
  const QVector<quint64> &want = analysis.rows.at(row).rungs;
  QVector<int> counts(want.size(), 0);
  for (const Row &candidate : analysis.rows) {
    if (candidate.symbol) {
      continue;
    }
    const int limit = qMin(candidate.rungs.size(), want.size());
    int match = 0;
    while (match < limit && candidate.rungs.at(match) == want.at(match)) {
      ++match;
    }
    for (int depth = 0; depth < match; ++depth) {
      ++counts[depth];
    }
  }
  return counts;
}

int Analysis::fileId(const QString &path) const {
  for (int i = 0; i < files.size(); ++i) {
    if (files.at(i).path == path) {
      return i;
    }
  }
  return -1;
}

QString Analysis::path(int fileId) const {
  return fileId >= 0 && fileId < files.size() ? files.at(fileId).path
                                              : QString();
}

void Worker::run(const asmview::Request &request) {
  emit finished(std::make_shared<const Analysis>(analyze(request)));
}

} // namespace asmview
