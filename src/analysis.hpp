// Disassembly and inline-stack attribution.
//
// Disassembles a linked binary and resolves each instruction's full inline
// stack, then attributes it to the innermost *project* file in that stack.
// System headers are opaque: code inlined out of <vector> or <cmath> is
// reported against the line of your code that pulled it in, not against the
// header. Every project frame in the stack is indexed, not just the innermost
// one, so a source line owns everything that got inlined into it.
#pragma once

#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

namespace asmview {

// One frame of an instruction's inline stack, innermost first.
struct Frame {
  QString function;
  QString file; // empty when the symbolizer had no location
  int line = 0;
  bool project = false;
  int fileId = -1; // interned; -1 when there is no location to intern
};

// Every file the symbolizer named, system headers included: digging into an
// inline stack has to be able to land on <vector> as readily as on your own
// code, which means system files need line indices too. `project` is what
// separates the two for display.
struct SourceFile {
  QString path;
  bool project = false;
  int instructions = 0; // instructions with this file anywhere in the stack
};

// A row is one line of the assembly pane: either a symbol header or an
// instruction.
struct Row {
  bool symbol = false;
  quint64 address = 0;
  QString mnemonic;
  QString operands;
  QString text; // rendered form; the symbol name for symbol rows
  QVector<Frame> frames;
  QVector<quint64> keys; // packed (file, line) of every project frame
  // One key per rung of the inline ladder, outermost first: the call site the
  // rung was entered from. Two instructions with the same first n rungs came
  // down the same path, which is what makes a region a region.
  QVector<quint64> rungs;
  int fileId = -1; // innermost project frame, -1 when none
  int line = 0;
  int symbolIndex = -1;
};

// A backward branch and the instruction it jumps back to. It is the only loop
// structure a linked binary still admits to, and after inlining it is often the
// only structure left that matches how you think about the code.
struct Loop {
  int first = 0; // the header: the row the back edge targets
  int last = 0;  // the back edge itself
  // Which column to draw in. Nesting indents, but so does mere overlap: after
  // loop rotation the two are not reliably distinguishable, and a column is
  // an honest claim where a nesting level would not be.
  int depth = 0;
  int instructions = 0; // body size
};

struct Symbol {
  QString name;
  quint64 address = 0;
  int row = 0; // index of the symbol's own row
  int instructions = 0;
  int attributed = 0;
  int fileId = -1;
};

// Source locations are interned into a single integer so they can key a hash.
constexpr quint64 sourceKey(int fileId, int line) {
  return (static_cast<quint64>(static_cast<quint32>(fileId)) << 32) |
         static_cast<quint32>(line);
}

struct Analysis {
  QString binary;
  QVector<SourceFile> files; // in the order they were first seen
  QVector<Row> rows;
  QVector<Symbol> symbols;
  QVector<Loop> loops;                // sorted by first, outermost before inner
  QHash<quint64, QVector<int>> index; // source key -> row indices
  int maxLoopDepth = 0;
  int instructions = 0;
  int attributed = 0; // instructions with at least one project frame
  int projectFiles = 0;
  QString error; // non-empty means the run failed and nothing else is valid

  int fileId(const QString &path) const;
  QString path(int fileId) const;
};

using AnalysisPtr = std::shared_ptr<const Analysis>;

struct Request {
  QString binary;
  QString root;
  QString symbol; // when set, disassemble only this symbol
  QString objdump = QStringLiteral("llvm-objdump");
  QString symbolizer = QStringLiteral("llvm-symbolizer");
  bool intel = true;
  bool includeDeps = false;
};

// One rung of the ladder an instruction was inlined down. A run of consecutive
// frames in the same file is one rung, not several: `new ParallelForTask` puts
// the call site and the constructor body in the same file, and walking those as
// separate levels is noise. The lines of the run are all kept, so the highlight
// still covers everything that file contributed at this depth.
struct DepthStep {
  int fileId = -1;
  QString function;  // innermost function of the run
  int line = 0;      // innermost line: where the code actually is
  int outerLine = 0; // outermost line of the run: where it was called from
  bool project = false;
  QVector<quint64> keys;
};

// Outermost first, which is the opposite of how the symbolizer reports it.
QVector<DepthStep> depthChain(const Row &row);

// An inclusive run of rows. The unit the panes highlight: a single instruction
// is rarely the thing you are looking at, a stretch of them is.
struct Span {
  int first = 0;
  int last = 0;

  bool operator==(const Span &other) const = default;
};

// Every stretch of assembly that reached `depth` down the same path as `row`.
// At depth 0 that is everything the outermost call site produced; each step
// deeper narrows it, which is what makes walking the breadcrumb a zoom.
QVector<Span> regionSpans(const Analysis &analysis, int row, int depth);

// The stretches produced by one source line, whatever depth it sits at.
QVector<Span> keySpans(const Analysis &analysis, quint64 key);

// Instructions in the region at each depth of `row`'s chain, in one pass. The
// breadcrumb wears these: they turn the chain from a list of file names into a
// visible narrowing, which is the whole reason to walk it.
QVector<int> regionSizes(const Analysis &analysis, int row);

// Blocking; safe to call off the GUI thread.
Analysis analyze(const Request &request);

// Walks up from a binary looking for the directory that owns it.
QString guessRoot(const QString &binary);

// The first of the usual build directories that actually contains a binary.
QString guessBinary(const QString &startDir);

class Worker : public QObject {
  Q_OBJECT

public slots:
  void run(const asmview::Request &request);

signals:
  void finished(asmview::AnalysisPtr result);
};

} // namespace asmview

Q_DECLARE_METATYPE(asmview::Request)
Q_DECLARE_METATYPE(asmview::AnalysisPtr)
