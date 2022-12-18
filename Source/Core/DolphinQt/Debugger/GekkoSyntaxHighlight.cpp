#include "DolphinQt/Debugger/GekkoSyntaxHighlight.h"

#include "Common/Assembler/GekkoParser.h"

namespace
{
using namespace Common::GekkoAssembler;
using namespace Common::GekkoAssembler::detail;

class HighlightParsePlugin : public ParsePlugin
{
private:
  std::vector<int> paren_stack;
  std::vector<std::pair<int, int>> matched_parens;
  std::vector<std::tuple<int, int, HighlightFormat>> formatting;

  void HighlightCurToken(HighlightFormat format)
  {
    const int len = static_cast<int>(owner->lexer.LookaheadRef().token_val.length());
    const int off = static_cast<int>(owner->lexer.ColNumber());
    formatting.emplace_back(off, len, format);
  }

public:
  auto MoveParens() { return std::move(matched_parens); }
  auto MoveFormatting() { return std::move(formatting); }

  void OnDirectivePre(GekkoDirective)
  {
    HighlightCurToken(HighlightFormat::kDirective);
  }

  void OnInstructionPre(ParseInfo const&, bool)
  {
    HighlightCurToken(HighlightFormat::kMnemonic);
  }

  virtual void OnTerminal(Terminal type, AssemblerToken const& val)
  {
    switch (type)
    {
      case Terminal::kId:
        HighlightCurToken(HighlightFormat::kSymbol);
        break;

      case Terminal::kHex:
      case Terminal::kDec:
      case Terminal::kOct:
      case Terminal::kBin:
      case Terminal::kFlt:
        HighlightCurToken(HighlightFormat::kImmediate);
        break;

      case Terminal::kGPR:
        HighlightCurToken(HighlightFormat::kGPR);
        break;

      case Terminal::kFPR:
        HighlightCurToken(HighlightFormat::kGPR);
        break;

      case Terminal::kSPR:
        HighlightCurToken(HighlightFormat::kSPR);
        break;

      case Terminal::kCRField:
        HighlightCurToken(HighlightFormat::kCRField);
        break;

      case Terminal::kLt:
      case Terminal::kGt:
      case Terminal::kEq:
      case Terminal::kSo:
        HighlightCurToken(HighlightFormat::kCRFlag);
        break;

      case Terminal::kStr:
        HighlightCurToken(HighlightFormat::kStr);

      default:
        break;
    }
  }

  virtual void OnHiaddr(std::string_view)
  {
    HighlightCurToken(HighlightFormat::kSymbol);
    auto&& [ha_pos, ha_tok] = owner->lexer.LookaheadTagRef(2);
    formatting.emplace_back(ha_pos.col, ha_tok.token_val.length(), HighlightFormat::kHaLa);
  }

  virtual void OnLoaddr(std::string_view id)
  {
    OnHiaddr(id);
  }

  virtual void OnOpenParen(ParenType type)
  {
    paren_stack.push_back(static_cast<int>(owner->lexer.ColNumber()));
  }

  virtual void OnCloseParen(ParenType type)
  {
    if (paren_stack.empty())
    {
      return;
    }

    matched_parens.emplace_back(paren_stack.back(), static_cast<int>(owner->lexer.ColNumber()));
    paren_stack.pop_back();
  }

  virtual void OnError()
  {
    formatting.emplace_back(owner->error->col, owner->error->len, HighlightFormat::kError);
  }

  virtual void OnLabelDecl(std::string_view name)
  {
    size_t len = owner->lexer.LookaheadRef().token_val.length();
    size_t off = owner->lexer.ColNumber();
    formatting.emplace_back(len, off, HighlightFormat::kSymbol);
  }

  virtual void OnVarDecl(std::string_view name)
  {
    OnLabelDecl(name);
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
      [this](std::pair<int, int> const& p)
      {
        return p.first == m_cursor_loc || p.second == m_cursor_loc;
      });
    if (paren_it != info->parens.end())
    {
      HighlightSubstr(paren_it->first, 1, HighlightFormat::kParen);
      HighlightSubstr(paren_it->second, 1, HighlightFormat::kParen);
    }
  }
  else if (m_mode == 2) {}

  for (auto&& [off, len, format] : info->block_format)
  {
    HighlightSubstr(off, len, format);
  }
}

void GekkoSyntaxHighlight::HighlightSubstr(int start, int len, HighlightFormat format)
{
  QTextCharFormat hl_format;
  constexpr QColor kNormalColor = QColor(0x3c, 0x38, 0x36); // Gruvbox fg
  constexpr QColor kDirectiveColor = QColor(0x9d, 0x00, 0x06); // Gruvbox darkred
  constexpr QColor kMnemonicColor = QColor(0x79, 0x74, 0x0e); // Gruvbox darkgreen
  constexpr QColor kImmColor = QColor(0xb5, 0x76, 0x14); // Gruvbox darkyellow
  constexpr QColor kBuiltinColor = QColor(0x42, 0x7b, 0x58); // Gruvbox darkaqua
  constexpr QColor kHaLaColor = QColor(0xaf, 0x3a, 0x03); // Gruvbox darkorange
  constexpr QColor kHoverBgColor = QColor(0xfb, 0xf1, 0xc7); // Gruvbox bg0
  constexpr QColor kStringColor = QColor(0x98, 0x97, 0x1a); // Gruvbox green

  switch (format)
  {
    case HighlightFormat::kDirective:
      hl_format.setForeground(kDirectiveColor);
      break;
    case HighlightFormat::kMnemonic:
      hl_format.setForeground(kMnemonicColor);
      break;
    case HighlightFormat::kSymbol:
      hl_format.setForeground(kNormalColor);
      break;
    case HighlightFormat::kImmediate:
      hl_format.setForeground(kImmColor);
      break;
    case HighlightFormat::kGPR:
      hl_format.setForeground(kBuiltinColor);
      break;
    case HighlightFormat::kFPR:
      hl_format.setForeground(kBuiltinColor);
      break;
    case HighlightFormat::kSPR:
      hl_format.setForeground(kBuiltinColor);
      break;
    case HighlightFormat::kCRField:
      hl_format.setForeground(kBuiltinColor);
      break;
    case HighlightFormat::kCRFlag:
      hl_format.setForeground(kBuiltinColor);
      break;
    case HighlightFormat::kStr:
      hl_format.setForeground(kStringColor);
      break;
    case HighlightFormat::kHaLa:
      hl_format.setForeground(kHaLaColor);
      break;
    case HighlightFormat::kParen:
      hl_format.setForeground(kNormalColor);
      hl_format.setBackground(kHoverBgColor);
      break;
    case HighlightFormat::kDefault:
      hl_format.clearForeground();
      hl_format.clearBackground();
      break;
    case HighlightFormat::kError:
      hl_format.setForeground(kNormalColor);
      hl_format.setUnderlineColor(Qt::red);
      hl_format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
      hl_format.setToolTip(QStringLiteral("Error here!"));
      break;
  }

  setFormat(start, len, hl_format);
}
