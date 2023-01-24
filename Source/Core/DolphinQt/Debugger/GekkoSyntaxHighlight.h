#pragma once

#include <QSyntaxHighlighter>

#include "Common/Assembler/AssemblerShared.h"

enum class HighlightFormat
{
  Directive,
  Mnemonic,
  Symbol,
  Immediate,
  GPR,
  FPR,
  SPR,
  CRField,
  CRFlag,
  Str,
  HaLa,
  Paren,
  Default,
  Comment,
  Error,
};

struct BlockInfo : public QTextBlockUserData
{
  std::vector<std::tuple<int, int, HighlightFormat>> block_format;
  std::vector<std::pair<int, int>> parens;
  std::optional<Common::GekkoAssembler::AssemblerError> error;
  bool error_at_eol = false;

  virtual ~BlockInfo() {}
};

class GekkoSyntaxHighlight : public QSyntaxHighlighter
{
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
