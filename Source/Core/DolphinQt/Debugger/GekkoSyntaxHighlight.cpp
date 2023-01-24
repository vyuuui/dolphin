#include "DolphinQt/Debugger/GekkoSyntaxHighlight.h"

#include "Common/Assembler/GekkoParser.h"

namespace
{
using namespace Common::GekkoAssembler;
using namespace Common::GekkoAssembler::detail;

class HighlightParsePlugin : public ParsePlugin
{
public:
  virtual ~HighlightParsePlugin() {}

  std::vector<std::pair<int, int>>&& MoveParens() { return std::move(m_matched_parens); }
  std::vector<std::tuple<int, int, HighlightFormat>>&& MoveFormatting()
  {
    return std::move(m_formatting);
  }

  void OnDirectivePre(GekkoDirective) { HighlightCurToken(HighlightFormat::Directive); }

  void OnInstructionPre(ParseInfo const&, bool) { HighlightCurToken(HighlightFormat::Mnemonic); }

  virtual void OnTerminal(Terminal type, AssemblerToken const& val)
  {
    switch (type)
    {
    case Terminal::Id:
      HighlightCurToken(HighlightFormat::Symbol);
      break;

    case Terminal::Hex:
    case Terminal::Dec:
    case Terminal::Oct:
    case Terminal::Bin:
    case Terminal::Flt:
      HighlightCurToken(HighlightFormat::Immediate);
      break;

    case Terminal::GPR:
      HighlightCurToken(HighlightFormat::GPR);
      break;

    case Terminal::FPR:
      HighlightCurToken(HighlightFormat::GPR);
      break;

    case Terminal::SPR:
      HighlightCurToken(HighlightFormat::SPR);
      break;

    case Terminal::CRField:
      HighlightCurToken(HighlightFormat::CRField);
      break;

    case Terminal::Lt:
    case Terminal::Gt:
    case Terminal::Eq:
    case Terminal::So:
      HighlightCurToken(HighlightFormat::CRFlag);
      break;

    case Terminal::Str:
      HighlightCurToken(HighlightFormat::Str);

    default:
      break;
    }
  }

  virtual void OnHiaddr(std::string_view)
  {
    HighlightCurToken(HighlightFormat::Symbol);
    auto&& [ha_pos, ha_tok] = m_owner->lexer.LookaheadTagRef(2);
    m_formatting.emplace_back(static_cast<int>(ha_pos.col),
                              static_cast<int>(ha_tok.token_val.length()), HighlightFormat::HaLa);
  }

  virtual void OnLoaddr(std::string_view id) { OnHiaddr(id); }

  virtual void OnOpenParen(ParenType type)
  {
    m_paren_stack.push_back(static_cast<int>(m_owner->lexer.ColNumber()));
  }

  virtual void OnCloseParen(ParenType type)
  {
    if (m_paren_stack.empty())
    {
      return;
    }

    m_matched_parens.emplace_back(m_paren_stack.back(),
                                  static_cast<int>(m_owner->lexer.ColNumber()));
    m_paren_stack.pop_back();
  }

  virtual void OnError()
  {
    m_formatting.emplace_back(static_cast<int>(m_owner->error->col),
                              static_cast<int>(m_owner->error->len), HighlightFormat::Error);
  }

  virtual void OnLabelDecl(std::string_view name)
  {
    const int len = static_cast<int>(m_owner->lexer.LookaheadRef().token_val.length());
    const int off = static_cast<int>(m_owner->lexer.ColNumber());
    m_formatting.emplace_back(len, off, HighlightFormat::Symbol);
  }

  virtual void OnVarDecl(std::string_view name) { OnLabelDecl(name); }

private:
  std::vector<int> m_paren_stack;
  std::vector<std::pair<int, int>> m_matched_parens;
  std::vector<std::tuple<int, int, HighlightFormat>> m_formatting;

  void HighlightCurToken(HighlightFormat format)
  {
    const int len = static_cast<int>(m_owner->lexer.LookaheadRef().token_val.length());
    const int off = static_cast<int>(m_owner->lexer.ColNumber());
    m_formatting.emplace_back(off, len, format);
  }
};
}  // namespace

void GekkoSyntaxHighlight::highlightBlock(QString const& text)
{
  BlockInfo* info = static_cast<BlockInfo*>(currentBlockUserData());
  if (info == nullptr)
  {
    info = new BlockInfo;
    setCurrentBlockUserData(info);
  }

  qsizetype comment_idx = text.indexOf(QLatin1Char('#'));
  if (comment_idx != -1)
  {
    HighlightSubstr(comment_idx, text.length() - comment_idx, HighlightFormat::Comment);
  }

  if (m_mode == 0)
  {
    HighlightParsePlugin plugin;
    ParseWithPlugin(&plugin, text.toStdString());

    info->block_format = plugin.MoveFormatting();
    info->parens = plugin.MoveParens();
    info->error = std::move(plugin.Error());
    info->error_at_eol = info->error && info->error->len == 0;
  }
  else if (m_mode == 1)
  {
    auto paren_it = std::find_if(info->parens.begin(), info->parens.end(),
                                 [this](std::pair<int, int> const& p) {
                                   return p.first == m_cursor_loc || p.second == m_cursor_loc;
                                 });
    if (paren_it != info->parens.end())
    {
      HighlightSubstr(paren_it->first, 1, HighlightFormat::Paren);
      HighlightSubstr(paren_it->second, 1, HighlightFormat::Paren);
    }
  }
  else if (m_mode == 2)
  {
  }

  for (auto&& [off, len, format] : info->block_format)
  {
    HighlightSubstr(off, len, format);
  }
}

void GekkoSyntaxHighlight::HighlightSubstr(int start, int len, HighlightFormat format)
{
  QTextCharFormat hl_format;
  constexpr QColor NORMAL_COLOR = QColor(0x3c, 0x38, 0x36);     // Gruvbox fg
  constexpr QColor DIRECTIVE_COLOR = QColor(0x9d, 0x00, 0x06);  // Gruvbox darkred
  constexpr QColor MNEMONIC_COLOR = QColor(0x79, 0x74, 0x0e);   // Gruvbox darkgreen
  constexpr QColor IMM_COLOR = QColor(0xb5, 0x76, 0x14);        // Gruvbox darkyellow
  constexpr QColor BUILTIN_COLOR = QColor(0x07, 0x66, 0x78);    // Gruvbox darkblue
  constexpr QColor HA_LA_COLOR = QColor(0xaf, 0x3a, 0x03);      // Gruvbox darkorange
  constexpr QColor HOVER_BG_COLOR = QColor(0xfb, 0xf1, 0xc7);   // Gruvbox bg0
  constexpr QColor STRING_COLOR = QColor(0x98, 0x97, 0x1a);     // Gruvbox green
  constexpr QColor COMMENT_COLOR = QColor(0x68, 0x9d, 0x6a);    // Gruvbox aqua

  switch (format)
  {
  case HighlightFormat::Directive:
    hl_format.setForeground(DIRECTIVE_COLOR);
    break;
  case HighlightFormat::Mnemonic:
    hl_format.setForeground(MNEMONIC_COLOR);
    break;
  case HighlightFormat::Symbol:
    hl_format.setForeground(NORMAL_COLOR);
    break;
  case HighlightFormat::Immediate:
    hl_format.setForeground(IMM_COLOR);
    break;
  case HighlightFormat::GPR:
    hl_format.setForeground(BUILTIN_COLOR);
    break;
  case HighlightFormat::FPR:
    hl_format.setForeground(BUILTIN_COLOR);
    break;
  case HighlightFormat::SPR:
    hl_format.setForeground(BUILTIN_COLOR);
    break;
  case HighlightFormat::CRField:
    hl_format.setForeground(BUILTIN_COLOR);
    break;
  case HighlightFormat::CRFlag:
    hl_format.setForeground(BUILTIN_COLOR);
    break;
  case HighlightFormat::Str:
    hl_format.setForeground(STRING_COLOR);
    break;
  case HighlightFormat::HaLa:
    hl_format.setForeground(HA_LA_COLOR);
    break;
  case HighlightFormat::Paren:
    hl_format.setForeground(NORMAL_COLOR);
    hl_format.setBackground(HOVER_BG_COLOR);
    break;
  case HighlightFormat::Default:
    hl_format.clearForeground();
    hl_format.clearBackground();
    break;
  case HighlightFormat::Comment:
    hl_format.setForeground(COMMENT_COLOR);
    break;
  case HighlightFormat::Error:
    hl_format.setForeground(NORMAL_COLOR);
    hl_format.setUnderlineColor(Qt::red);
    hl_format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
    hl_format.setToolTip(QStringLiteral("Error here!"));
    break;
  }

  setFormat(start, len, hl_format);
}
