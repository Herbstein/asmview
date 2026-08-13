#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>

#include <functional>

namespace asmview {

class AsmHighlighter : public QSyntaxHighlighter {
  Q_OBJECT

public:
  struct LineStyle {
    bool symbol = false;
    bool dim = false; // no project frame at all: system or runtime code
  };
  using StyleHook = std::function<LineStyle(int blockNumber)>;

  explicit AsmHighlighter(QTextDocument *document);
  void setStyleHook(StyleHook hook);

protected:
  void highlightBlock(const QString &text) override;

private:
  StyleHook hook_;
  QRegularExpression registers_;
  QRegularExpression numbers_;
  QRegularExpression symbols_;
};

class CppHighlighter : public QSyntaxHighlighter {
  Q_OBJECT

public:
  explicit CppHighlighter(QTextDocument *document);

protected:
  void highlightBlock(const QString &text) override;

private:
  struct Rule {
    QRegularExpression pattern;
    QTextCharFormat format;
  };
  QVector<Rule> rules_;
  QRegularExpression commentStart_;
  QRegularExpression commentEnd_;
  QTextCharFormat commentFormat_;
};

} // namespace asmview
