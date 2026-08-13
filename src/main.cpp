// asmview -- cursor-linked source/assembly viewer for LTO builds.
//
// CLion's assembly view disassembles a translation unit; with -flto=full the
// interesting question is what the *linked* binary looks like once everything
// has been inlined. This disassembles the linked binary instead and walks each
// instruction's inline stack back to the project source line responsible for
// it. Needs a build with debug info, so RelWithDebInfo.

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>

#include "analysis.hpp"
#include "main_window.hpp"

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("asmview"));
  QCoreApplication::setOrganizationName(QStringLiteral("asmview"));
  QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

  qRegisterMetaType<asmview::Request>();
  qRegisterMetaType<asmview::AnalysisPtr>();

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("Cursor-linked source/assembly viewer for LTO builds."));
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument(QStringLiteral("binary"),
                               QStringLiteral("Linked binary built with -g."));

  const QCommandLineOption symbolOption(
      {QStringLiteral("s"), QStringLiteral("symbol")},
      QStringLiteral("Disassemble only this symbol."),
      QStringLiteral("symbol"));
  const QCommandLineOption rootOption(
      QStringLiteral("root"),
      QStringLiteral("Project root; files outside it count as system code."),
      QStringLiteral("dir"));
  const QCommandLineOption attOption(
      QStringLiteral("att"),
      QStringLiteral("AT&T syntax instead of Intel."));
  const QCommandLineOption depsOption(
      QStringLiteral("deps"),
      QStringLiteral("Treat bundled dependencies as project code."));
  const QCommandLineOption objdumpOption(QStringLiteral("objdump"),
                                         QStringLiteral("llvm-objdump to use."),
                                         QStringLiteral("path"),
                                         QStringLiteral("llvm-objdump"));
  const QCommandLineOption symbolizerOption(
      QStringLiteral("symbolizer"),
      QStringLiteral("llvm-symbolizer to use."),
      QStringLiteral("path"),
      QStringLiteral("llvm-symbolizer"));
  parser.addOptions({symbolOption,
                     rootOption,
                     attOption,
                     depsOption,
                     objdumpOption,
                     symbolizerOption});
  parser.process(app);

  asmview::Request request;
  const QStringList positional = parser.positionalArguments();
  request.binary = positional.isEmpty()
                       ? asmview::guessBinary(QDir::currentPath())
                       : QFileInfo(positional.first()).absoluteFilePath();
  request.root =
      parser.isSet(rootOption)
          ? QFileInfo(parser.value(rootOption)).absoluteFilePath()
          : (request.binary.isEmpty() ? QDir::currentPath()
                                      : asmview::guessRoot(request.binary));
  request.symbol = parser.value(symbolOption);
  request.intel = !parser.isSet(attOption);
  request.includeDeps = parser.isSet(depsOption);
  request.objdump = parser.value(objdumpOption);
  request.symbolizer = parser.value(symbolizerOption);

  asmview::MainWindow window(request);
  window.resize(1500, 900);
  window.show();
  return app.exec();
}
