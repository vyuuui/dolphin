#pragma once

#include <QSyntaxHighlighter>

#include "Common/Assembler/AssemblerShared.h"

enum class HighlightFormat {
  kDirective,
  kMnemonic,
  kSymbol,
  kImmediate,
  kGPR,
  kFPR,
  kSPR,
  kCRField,
  kCRFlag,
  kStr,
  kHaLa,
  kParen,
  kDefault,
  kError,
};

struct BlockInfo : public QTextBlockUserData {
  std::vector<std::tuple<int, int, HighlightFormat>> block_format;
  std::vector<std::pair<int, int>> parens;
  std::optional<Common::GekkoAssembler::AssemblerError> error;
  bool error_at_eol = false;

  virtual ~BlockInfo() {}
};

class GekkoSyntaxHighlight : public QSyntaxHighlighter {
  Q_OBJECT;

public:
  GekkoSyntaxHighlight(QTextDocument* document) : QSyntaxHighlighter(document), m_mode(0) {}

  void HighlightSubstr(int start, int len, HighlightFormat format);
  void SetMode(int mode) { m_mode = mode; }
  void SetCursorLoc(int loc) { m_cursor_loc = loc; }

protected:
  void highlightBlock(QString const& line) override;

private:
  int m_mode;
  int m_cursor_loc;

};
