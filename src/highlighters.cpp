#include "highlighters.hpp"

#include <QTextDocument>

#include "theme.hpp"

namespace asmview {
namespace {

bool isControlFlow(QStringView mnemonic) {
  if (mnemonic.startsWith(u'j')) { // jmp, je, jne, jbe, ...
    return true;
  }
  static const QSet<QStringView> kOthers =
      {u"call", u"ret", u"retq", u"loop", u"syscall", u"ud2", u"int3"};
  return kOthers.contains(mnemonic);
}

} // namespace

AsmHighlighter::AsmHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document),
      registers_(QStringLiteral(
          R"(\b(?:[re]?(?:ax|bx|cx|dx|si|di|bp|sp|ip)|r(?:[89]|1[0-5])[bwd]?|)"
          R"([abcd][hl]|[xyz]mm\d+|k[0-7]|[cdefgs]s|st\(\d\))\b)")),
      numbers_(QStringLiteral(R"(\b(?:0x[0-9a-fA-F]+|\d+)\b)")),
      symbols_(QStringLiteral(R"(<[^>]+>)")) {}

void AsmHighlighter::setStyleHook(StyleHook hook) {
  hook_ = std::move(hook);
  rehighlight();
}

void AsmHighlighter::highlightBlock(const QString &text) {
  const theme::Palette &p = theme::palette();
  const LineStyle style =
      hook_ ? hook_(currentBlock().blockNumber()) : LineStyle{};

  if (style.symbol) {
    QTextCharFormat format;
    format.setForeground(p.symbolText);
    format.setFontWeight(QFont::Bold);
    setFormat(0, text.length(), format);
    return;
  }

  // Code with no project frame in its inline stack recedes entirely; picking
  // it apart into mnemonics and registers would only draw the eye to it.
  if (style.dim) {
    QTextCharFormat format;
    format.setForeground(p.dimText);
    setFormat(0, text.length(), format);
    return;
  }

  const qsizetype comment = text.indexOf(u'#');
  const qsizetype end = comment < 0 ? text.length() : comment;

  qsizetype mnemonicEnd = text.indexOf(u' ');
  if (mnemonicEnd < 0 || mnemonicEnd > end) {
    mnemonicEnd = end;
  }
  QTextCharFormat mnemonicFormat;
  const QStringView mnemonic = QStringView{text}.first(mnemonicEnd);
  mnemonicFormat.setForeground(isControlFlow(mnemonic) ? p.controlFlow
                                                       : p.mnemonicText);
  mnemonicFormat.setFontWeight(QFont::DemiBold);
  setFormat(0, static_cast<int>(mnemonicEnd), mnemonicFormat);

  const auto applyWithin = [&](const QRegularExpression &pattern,
                               const QColor &color) {
    QRegularExpressionMatchIterator it = pattern.globalMatch(text);
    while (it.hasNext()) {
      const QRegularExpressionMatch match = it.next();
      if (match.capturedStart() < mnemonicEnd || match.capturedEnd() > end) {
        continue;
      }
      QTextCharFormat format;
      format.setForeground(color);
      setFormat(static_cast<int>(match.capturedStart()),
                static_cast<int>(match.capturedLength()),
                format);
    }
  };
  applyWithin(numbers_, p.numberText);
  applyWithin(registers_, p.registerText);
  applyWithin(symbols_, p.symbolText);

  if (comment >= 0) {
    QTextCharFormat format;
    format.setForeground(p.commentText);
    setFormat(static_cast<int>(comment),
              static_cast<int>(text.length() - comment),
              format);
  }
}

// -- C++ --------------------------------------------------------------------

CppHighlighter::CppHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document) {
  const theme::Palette &p = theme::palette();

  const auto add = [this](const QString &pattern,
                          const QColor &color,
                          bool italic = false,
                          int weight = QFont::Normal) {
    Rule rule;
    rule.pattern = QRegularExpression(pattern);
    rule.format.setForeground(color);
    rule.format.setFontItalic(italic);
    rule.format.setFontWeight(weight);
    rules_.append(rule);
  };

  static const QString kKeywords = QStringLiteral(
      R"(\b(?:alignas|alignof|auto|break|case|catch|class|concept|const|consteval|constexpr)"
      R"(|constinit|const_cast|continue|co_await|co_return|co_yield|decltype|default|delete)"
      R"(|do|dynamic_cast|else|enum|explicit|export|extern|false|final|for|friend|goto|if)"
      R"(|inline|mutable|namespace|new|noexcept|nullptr|operator|override|private|protected)"
      R"(|public|reinterpret_cast|requires|return|sizeof|static|static_assert|static_cast)"
      R"(|struct|switch|template|this|thread_local|throw|true|try|typedef|typename|union)"
      R"(|using|virtual|volatile|while)\b)");
  static const QString kTypes = QStringLiteral(
      R"(\b(?:bool|char|char8_t|char16_t|char32_t|double|float|int|long|short|signed)"
      R"(|size_t|unsigned|void|wchar_t|u?int(?:8|16|32|64)_t)\b)");

  add(kKeywords, p.keyword, false, QFont::DemiBold);
  add(kTypes, p.type);
  add(QStringLiteral(
          R"(\b(?:0[xX][0-9a-fA-F']+|\d[\d']*\.?[\d']*(?:[eE][-+]?\d+)?[fFuUlL]*)\b)"),
      p.numberText);
  add(QStringLiteral(R"(^\s*#\s*\w+)"), p.preprocessor);
  add(QStringLiteral(R"("(?:[^"\\]|\\.)*")"), p.string);
  add(QStringLiteral(R"('(?:[^'\\]|\\.)*')"), p.string);
  add(QStringLiteral(R"(//[^\n]*)"), p.comment, true);

  commentStart_ = QRegularExpression(QStringLiteral(R"(/\*)"));
  commentEnd_ = QRegularExpression(QStringLiteral(R"(\*/)"));
  commentFormat_.setForeground(p.comment);
  commentFormat_.setFontItalic(true);
}

void CppHighlighter::highlightBlock(const QString &text) {
  for (const Rule &rule : std::as_const(rules_)) {
    QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
    while (it.hasNext()) {
      const QRegularExpressionMatch match = it.next();
      setFormat(static_cast<int>(match.capturedStart()),
                static_cast<int>(match.capturedLength()),
                rule.format);
    }
  }

  setCurrentBlockState(0);
  int start = 0;
  if (previousBlockState() != 1) {
    start = static_cast<int>(text.indexOf(commentStart_));
  }
  while (start >= 0) {
    const QRegularExpressionMatch match = commentEnd_.match(text, start);
    int length = 0;
    if (match.hasMatch()) {
      length = static_cast<int>(match.capturedEnd() - start);
    } else {
      setCurrentBlockState(1);
      length = static_cast<int>(text.length() - start);
    }
    setFormat(start, length, commentFormat_);
    start = static_cast<int>(text.indexOf(commentStart_, start + length));
  }
}

} // namespace asmview
