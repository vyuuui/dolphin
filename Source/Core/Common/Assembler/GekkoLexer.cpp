#include "Common/Assembler/GekkoLexer.h"

#include "Common/Assert.h"

#include <iterator>
#include <numeric>

namespace Common::GekkoAssembler::detail
{
namespace
{

constexpr bool is_octal(char c)
{
  return c >= '0' && c <= '7';
}

constexpr bool is_binary(char c)
{
  return c == '0' || c == '1';
}

template <typename T>
constexpr T convert_nib(char c)
{
  if (c >= 'a' && c <= 'f') { return static_cast<T>(c - 'a' + 10); }
  if (c >= 'A' && c <= 'F') { return static_cast<T>(c - 'A' + 10); }
  return static_cast<T>(c - '0');
}

constexpr TokenType single_char_token(char ch)
{
  switch (ch)
  {
    case ',':
      return TokenType::kComma;
    case '(':
      return TokenType::kLparen;
    case ')':
      return TokenType::kRparen;
    case '|':
      return TokenType::kPipe;
    case '^':
      return TokenType::kCaret;
    case '&':
      return TokenType::kAmpersand;
    case '+':
      return TokenType::kPlus;
    case '-':
      return TokenType::kMinus;
    case '*':
      return TokenType::kStar;
    case '/':
      return TokenType::kSlash;
    case '~':
      return TokenType::kTilde;
    case '@':
      return TokenType::kAt;
    case ':':
      return TokenType::kColon;
    case '`':
      return TokenType::kGrave;
    case '.':
      return TokenType::kDot;
    case '\0':
      return TokenType::kEof;
    case '\n':
      return TokenType::kEol;
    default:
      return TokenType::kInvalid;
  }
}

// Convert a string literal into its raw-data form
template <typename Cont>
void ConvertStringLiteral(std::string_view literal, std::back_insert_iterator<Cont> out_it)
{
  for (size_t i = 1; i < literal.size() - 1;)
  {
    if (literal[i] == '\\')
    {
      ++i;
      if (is_octal(literal[i]))
      {
        // Octal escape
        char octal_escape = 0;
        for (char c = literal[i]; is_octal(c); c = literal[++i])
        {
          octal_escape = (octal_escape << 3) + (c - '0');
        }
        out_it = static_cast<u8>(octal_escape);
      }
      else if (literal[i] == 'x')
      {
        // Hex escape
        char hex_escape = 0;
        for (char c = literal[++i]; std::isxdigit(c); c = literal[++i])
        {
          hex_escape = (hex_escape << 4) + convert_nib<char>(c);
        }
        out_it = static_cast<u8>(hex_escape);
      }
      else
      {
        char simple_escape;
        switch (literal[i])
        {
          case '\'': simple_escape = '\x27'; break;
          case '"':  simple_escape = '\x22'; break;
          case '?':  simple_escape = '\x3f'; break;
          case '\\': simple_escape = '\x5c'; break;
          case 'a':  simple_escape = '\x07'; break;
          case 'b':  simple_escape = '\x08'; break;
          case 'f':  simple_escape = '\x0c'; break;
          case 'n':  simple_escape = '\x0a'; break;
          case 'r':  simple_escape = '\x0d'; break;
          case 't':  simple_escape = '\x09'; break;
          case 'v':  simple_escape = '\x0b'; break;
          default:   simple_escape = literal[i]; break;
        }
        out_it = static_cast<u8>(simple_escape);
        ++i;
      }
    }
    else
    {
      out_it = static_cast<u8>(literal[i]);
      ++i;
    }
  }
}

template <std::unsigned_integral T>
std::optional<T> eval_integral(TokenType tp, std::string_view val)
{
  constexpr auto hex_step = [](T acc, char c) { return acc << 4 | convert_nib<T>(c); };
  constexpr auto dec_step = [](T acc, char c) { return acc * 10 + (c - '0'); };
  constexpr auto oct_step = [](T acc, char c) { return acc << 3 | (c - '0'); };
  constexpr auto bin_step = [](T acc, char c) { return acc << 1 | (c - '0'); };

  switch (tp)
  {
    case TokenType::kHexadecimalLit:
      return std::accumulate(val.begin() + 2, val.end(), T{0}, hex_step);
    case TokenType::kDecimalLit:
      return std::accumulate(val.begin(), val.end(), T{0}, dec_step);
    case TokenType::kOctalLit:
      return std::accumulate(val.begin() + 1, val.end(), T{0}, oct_step);
    case TokenType::kBinaryLit:
      return std::accumulate(val.begin(), val.end(), T{0}, bin_step);
    case TokenType::kGPR:
    case TokenType::kFPR:
      return std::accumulate(val.begin() + 1, val.end(), T{0}, dec_step);
    case TokenType::kCRField:
      return std::accumulate(val.begin() + 2, val.end(), T{0}, dec_step);
    case TokenType::kSPR: 
      return static_cast<T>(sprg_map.find(val)->second);
    case TokenType::kLt:
      return T{0};
    case TokenType::kGt:
      return T{1};
    case TokenType::kEq:
      return T{2};
    case TokenType::kSo:
      return T{3};
    default:
      return std::nullopt;
  }
}
}  // namespace

void ConvertStringLiteral(std::string_view literal, std::vector<u8>& out_vec)
{
  ConvertStringLiteral(literal, std::back_inserter(out_vec));
}

std::string_view TokenTypeToStr(TokenType tp)
{
  switch (tp)
  {
    case TokenType::kGPR:
      return "GPR";
    case TokenType::kFPR:
      return "FPR";
    case TokenType::kSPR:
      return "SPR";
    case TokenType::kCRField:
      return "CR Field";
    case TokenType::kLt:
    case TokenType::kGt:
    case TokenType::kEq:
    case TokenType::kSo:
      return "CR Bit";
    case TokenType::kIdentifier:
      return "Identifier";
    case TokenType::kStringLit:
      return "String Literal";
    case TokenType::kDecimalLit:
      return "Decimal Literal";
    case TokenType::kBinaryLit:
      return "Binary Literal";
    case TokenType::kHexadecimalLit:
      return "Hexadecimal Literal";
    case TokenType::kOctalLit:
      return "Octal Literal";
    case TokenType::kFloatLit:
      return "Float Literal";
    case TokenType::kInvalid:
      return "Invalid";
    case TokenType::kLsh:
      return "<<";
    case TokenType::kRsh:
      return ">>";
    case TokenType::kComma:
      return ",";
    case TokenType::kLparen:
      return "(";
    case TokenType::kRparen:
      return ")";
    case TokenType::kPipe:
      return "|";
    case TokenType::kCaret:
      return "^";
    case TokenType::kAmpersand:
      return "&";
    case TokenType::kPlus:
      return "+";
    case TokenType::kMinus:
      return "-";
    case TokenType::kStar:
      return "*";
    case TokenType::kSlash:
      return "/";
    case TokenType::kTilde:
      return "~";
    case TokenType::kAt:
      return "@";
    case TokenType::kColon:
      return ":";
    case TokenType::kGrave:
      return "`";
    case TokenType::kDot:
      return ".";
    case TokenType::kEof:
      return "End of File";
    case TokenType::kEol:
      return "End of Line";
    default:
      return "";
  }
}

std::string_view AssemblerToken::TypeStr() const
{
  return TokenTypeToStr(token_type);
}

std::string_view AssemblerToken::ValStr() const
{
  switch (token_type)
  {
    case TokenType::kEol:
      return "<EOL>";
    case TokenType::kEof:
      return "<EOF>";
    default:
      return token_val;
  }
}

template<>
std::optional<float> AssemblerToken::EvalToken() const
{
  if (token_type == TokenType::kFloatLit)
  {
    return std::stof(std::string(token_val));
  }
  return std::nullopt;
}

template<>
std::optional<double> AssemblerToken::EvalToken() const
{
  if (token_type == TokenType::kFloatLit)
  {
    return std::stod(std::string(token_val));
  }
  return std::nullopt;
}

template<>
std::optional<u8> AssemblerToken::EvalToken() const
{
  return eval_integral<u8>(token_type, token_val);
}

template<>
std::optional<u16> AssemblerToken::EvalToken() const
{
  return eval_integral<u16>(token_type, token_val);
}

template<>
std::optional<u32> AssemblerToken::EvalToken() const
{
  return eval_integral<u32>(token_type, token_val);
}

template<>
std::optional<u64> AssemblerToken::EvalToken() const
{
  return eval_integral<u64>(token_type, token_val);
}

size_t Lexer::LineNumber() const
{
  return lexed_tokens.empty() ? pos.line : tag_of(lexed_tokens.front()).line;
}

size_t Lexer::ColNumber() const
{
  return lexed_tokens.empty() ? pos.col : tag_of(lexed_tokens.front()).col;
}

std::string_view Lexer::CurrentLine() const
{
  const size_t line_index = lexed_tokens.empty() ? pos.index : tag_of(lexed_tokens.front()).index;
  size_t begin_index = line_index == 0 ? 0 : line_index - 1;
  for (; begin_index > 0; begin_index--)
  {
    if (lex_string[begin_index] == '\n')
    {
      begin_index++;
      break;
    }
  }
  size_t end_index = begin_index;
  for (; end_index < lex_string.size(); end_index++)
  {
    if (lex_string[end_index] == '\n')
    {
      end_index++;
      break;
    }
  }
  return lex_string.substr(begin_index, end_index - begin_index);
}

void Lexer::SetIdentifierMatchRule(IdentifierMatchRule set)
{
  FeedbackTokens();
  match_rule = set;
}

Tagged<Lexer::CursorPosition, AssemblerToken> const&
Lexer::LookaheadTagRef(size_t num_fwd) const
{
  while (lexed_tokens.size() < num_fwd)
  {
    LookaheadRef();
  }
  return lexed_tokens[num_fwd];
}

AssemblerToken Lexer::Lookahead() const
{
  if (lexed_tokens.empty())
  {
    CursorPosition pos_pre = pos;
    lexed_tokens.emplace_back(pos_pre, LexSingle());
  }
  return value_of(lexed_tokens.front());
}

AssemblerToken const& Lexer::LookaheadRef() const
{
  if (lexed_tokens.empty())
  {
    CursorPosition pos_pre = pos;
    lexed_tokens.emplace_back(pos_pre, LexSingle());
  }
  return value_of(lexed_tokens.front());
}

TokenType Lexer::LookaheadType() const
{
  return LookaheadRef().token_type;
}

AssemblerToken Lexer::LookaheadFloat() const
{
  FeedbackTokens();
  SkipWs();

  CursorPosition pos_pre = pos;
  ScanStart();

  std::optional<std::string_view> failure_reason = RunDfa(float_dfa);

  // Special case: lex at least a single char for no matches for errors to make sense
  if (scan_pos.index == pos_pre.index)
  {
    Step();
  }

  std::string_view tok_str = ScanFinishOut();
  AssemblerToken tok;
  if (!failure_reason)
  {
    tok = AssemblerToken {
      TokenType::kFloatLit,
      tok_str,
      "",
      Interval {0, 0},
    };
  } else {
    tok = AssemblerToken {
      TokenType::kInvalid,
      tok_str,
      *failure_reason,
      Interval {0, tok_str.length()},
    };
  }

  lexed_tokens.emplace_back(pos_pre, tok);
  return tok;
}

void Lexer::Eat()
{
  if (lexed_tokens.empty())
  {
    LexSingle();
  } else {
    lexed_tokens.pop_front();
  }
}

void Lexer::EatAndReset()
{
  Eat();
  SetIdentifierMatchRule(IdentifierMatchRule::kTypical);
}

std::optional<std::string_view> Lexer::RunDfa(std::vector<DfaNode> const& dfa) const
{
  size_t dfa_index = 0;
  bool transition_found;
  do {
    transition_found = false;
    if (Peek() == '\0')
    {
      break;
    }

    DfaNode const& n = dfa[dfa_index];
    for (auto&& edge : n.edges)
    {
      if (edge.first(Peek()))
      {
        transition_found = true;
        dfa_index = edge.second;
        break;
      }
    }

    if (transition_found)
    {
      Step();
    }
  } while (transition_found);

  return dfa[dfa_index].match_failure_reason;
}

void Lexer::SkipWs() const
{
  ScanStart();
  for (char c = Peek(); std::isspace(c) && c != '\n'; c = Step().Peek());
  ScanFinish();
}

void Lexer::FeedbackTokens() const
{
  if (lexed_tokens.empty())
  {
    return;
  }
  pos = scan_pos = tag_of(lexed_tokens.front());
  lexed_tokens.clear();
}

bool Lexer::IdentifierHeadExtra(char h) const
{
  switch (match_rule)
  {
    case IdentifierMatchRule::kTypical:
    case IdentifierMatchRule::kMnemonic:
      return false;
    case IdentifierMatchRule::kDirective:
      return std::isdigit(h);
  }
  return false;
}

bool Lexer::IdentifierExtra(char c) const
{
  switch (match_rule)
  {
    case IdentifierMatchRule::kTypical:
    case IdentifierMatchRule::kDirective:
      return false;
    case IdentifierMatchRule::kMnemonic:
      return c == '+' || c == '-' || c == '.';
  }
  return false;
}

void Lexer::ScanStart() const
{
  scan_pos = pos;
}

void Lexer::ScanFinish() const
{
  pos = scan_pos;
}

std::string_view Lexer::ScanFinishOut() const
{
  const size_t start = pos.index;
  pos = scan_pos;
  return lex_string.substr(start, scan_pos.index - start);
}

char Lexer::Peek() const
{
  if (scan_pos.index >= lex_string.length())
  {
    return 0;
  }
  return lex_string[scan_pos.index];
}

Lexer const& Lexer::Step() const
{
  if (scan_pos.index >= lex_string.length())
  {
    return *this;
  }

  if (Peek() == '\n')
  {
    scan_pos.line++;
    scan_pos.col = 0;
  } else {
    scan_pos.col++;
  }
  scan_pos.index++;
  return *this;
}

TokenType
Lexer::LexStringLit(std::string_view& invalid_reason, Interval& invalid_region) const
{
  // The open quote has alread been matched
  const size_t string_start = scan_pos.index - 1;
  TokenType token_type = TokenType::kStringLit;

  std::optional<std::string_view> failure_reason = RunDfa(string_dfa);

  if (failure_reason)
  {
    token_type = TokenType::kInvalid;
    invalid_reason = *failure_reason;
    invalid_region = Interval {0, scan_pos.index - string_start};
  }

  return token_type;
}

TokenType Lexer::ClassifyAlnum() const
{
  const std::string_view alnum =
    lex_string.substr(pos.index, scan_pos.index - pos.index);
  constexpr auto valid_regnum = [](std::string_view rn)
    {
      if (rn.length() == 1 && std::isdigit(rn[0]))
      {
        return true;
      }
      else if (rn.length() == 2 && std::isdigit(rn[0]) &&
               std::isdigit(rn[1]))
      {
        if (rn[0] == '1' || rn[0] == '2')
        {
          return true;
        }

        if (rn[0] == '3')
        {
          return rn[1] <= '2';
        }
      }

      return false;
    };

  if (alnum[0] == 'r' && valid_regnum(alnum.substr(1)))
  {
    return TokenType::kGPR;
  }
  else if (alnum[0] == 'f' && valid_regnum(alnum.substr(1)))
  {
    return TokenType::kFPR;
  }
  else if (alnum.length() == 3 && alnum.substr(0, 2) == "cr" &&
             alnum[2] >= '0' && alnum[2] <= '7')
  {
    return TokenType::kCRField;
  }
  else if (alnum == "lt")
  {
    return TokenType::kLt;
  }
  else if (alnum == "gt")
  {
    return TokenType::kGt;
  }
  else if (alnum == "eq")
  {
    return TokenType::kEq;
  }
  else if (alnum == "so")
  {
    return TokenType::kSo;
  }
  else if (sprg_map.find(alnum) != sprg_map.end())
  {
    return TokenType::kSPR;
  }
  return TokenType::kIdentifier;
}

AssemblerToken Lexer::LexSingle() const
{
  SkipWs();

  ScanStart();
  const char h = Peek();

  TokenType token_type;
  std::string_view invalid_reason = "";
  Interval invalid_region = Interval {0, 0};

  Step();

  if (std::isalpha(h) || h == '_' || IdentifierHeadExtra(h))
  {
    for (char c = Peek();
         std::isalnum(c) || c == '_' || IdentifierExtra(c);
         c = Step().Peek());

    token_type = ClassifyAlnum();
  }
  else if (h == '"')
  {
    token_type = LexStringLit(invalid_reason, invalid_region);
  }
  else if (h == '0')
  {
    const char imm_type = Peek();

    if (imm_type == 'x')
    {
      token_type = TokenType::kHexadecimalLit;
      Step();
      for (char c = Peek(); std::isxdigit(c); c = Step().Peek());
    }
    else if (imm_type == 'b')
    {
      token_type = TokenType::kBinaryLit;
      Step();
      for (char c = Peek(); is_binary(c); c = Step().Peek());
    }
    else if (is_octal(imm_type))
    {
      token_type = TokenType::kOctalLit;
      for (char c = Peek(); is_octal(c); c = Step().Peek());
    }
    else
    {
      token_type = TokenType::kDecimalLit;
    }
  }
  else if (std::isdigit(h))
  {
    for (char c = Peek(); std::isdigit(c); c = Step().Peek());
    token_type = TokenType::kDecimalLit;
  }
  else if (h == '<' || h == '>')
  {
    // Special case for two-character operators
    const char second_ch = Peek();
    if (second_ch == h)
    {
      Step();
      token_type = second_ch == '<' ? TokenType::kLsh : TokenType::kRsh;
    }
    else
    {
      token_type = TokenType::kInvalid;
      invalid_reason = "Unrecognized character";
      invalid_region = Interval {0, 1};
    }
  }
  else
  {
    token_type = single_char_token(h);
    if (token_type == TokenType::kInvalid)
    {
      invalid_reason = "Unrecognized character";
      invalid_region = Interval {0, 1};
    }
  }

  AssemblerToken new_tok = {token_type, ScanFinishOut(), invalid_reason, invalid_region};
  SkipWs();
  return new_tok;
}
}  // namespace Common::GekkoAssembler::detail
