#include "GekkoParser.h"

#include <algorithm>
#include <functional>
#include <map>
#include <numeric>
#include <type_traits>

#include <fmt/format.h>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/Assembler/AssemblerTables.h"
#include "Common/Assembler/GekkoLexer.h"
#include "Common/Assert.h"

namespace Common::GekkoAssembler::detail
{
namespace
{
bool MatchOperandFirst(AssemblerToken const& tok)
{
  switch (tok.token_type)
  {
    case TokenType::kMinus:
    case TokenType::kTilde:
    case TokenType::kLparen:
    case TokenType::kGrave:
    case TokenType::kIdentifier:
    case TokenType::kDecimalLit:
    case TokenType::kOctalLit:
    case TokenType::kHexadecimalLit:
    case TokenType::kBinaryLit:
    case TokenType::kDot:
      return true;
    default:
      return false;
  }
}

void ParseImm(ParseState& state)
{
  AssemblerToken tok = state.lexer.Lookahead();
  switch (tok.token_type)
  {
    case TokenType::kHexadecimalLit:
      state.plugin.OnTerminal(Terminal::kHex, tok); break;
    case TokenType::kDecimalLit:
      state.plugin.OnTerminal(Terminal::kDec, tok); break;
    case TokenType::kOctalLit:
      state.plugin.OnTerminal(Terminal::kOct, tok); break;
    case TokenType::kBinaryLit:
      state.plugin.OnTerminal(Terminal::kBin, tok); break;
    default:
      state.EmitErrorHere(
        fmt::format("Invalid {} with value '{}'", tok.TypeStr(), tok.ValStr()));
      return;
  }
  if (state.error) { return; }
  state.lexer.Eat();
}

void ParseId(ParseState& state)
{
  AssemblerToken tok = state.lexer.Lookahead();
  if (tok.token_type == TokenType::kIdentifier)
  {
    state.plugin.OnTerminal(Terminal::kId, tok);
    if (state.error) { return; }
    state.lexer.Eat();
  }
  else
  {
    state.EmitErrorHere(
        fmt::format("Expected an identifier, but found '{}'", tok.ValStr()));
  }
}

void ParseIdLocation(ParseState& state)
{
  std::array<AssemblerToken, 3> toks;
  state.lexer.LookaheadN(toks);

  if (toks[1].token_type == TokenType::kAt)
  {
    if (toks[2].token_val == "ha")
    {
      state.plugin.OnHiaddr(toks[0].token_val);
      if (state.error) { return; }
      state.lexer.EatN<3>();
      return;
    }
    else if (toks[2].token_val == "l")
    {
      state.plugin.OnLoaddr(toks[0].token_val);
      if (state.error) { return; }
      state.lexer.EatN<3>();
      return;
    }
  }

  ParseId(state);
}

void ParsePpcBuiltin(ParseState& state)
{
  AssemblerToken tok = state.lexer.Lookahead();
  switch (tok.token_type)
  {
    case TokenType::kGPR:
      state.plugin.OnTerminal(Terminal::kGPR, tok); break;
    case TokenType::kFPR:
      state.plugin.OnTerminal(Terminal::kFPR, tok); break;
    case TokenType::kSPR:
      state.plugin.OnTerminal(Terminal::kSPR, tok); break;
    case TokenType::kCRField:
      state.plugin.OnTerminal(Terminal::kCRField, tok); break;
    case TokenType::kLt:
      state.plugin.OnTerminal(Terminal::kLt, tok); break;
    case TokenType::kGt:
      state.plugin.OnTerminal(Terminal::kGt, tok); break;
    case TokenType::kEq:
      state.plugin.OnTerminal(Terminal::kEq, tok); break;
    case TokenType::kSo: 
      state.plugin.OnTerminal(Terminal::kSo, tok); break;
    default:
      state.EmitErrorHere(
          fmt::format("Unexpected token '{}' in ppc builtin", state.lexer.LookaheadRef().ValStr()));
      break;
  }
  if (state.error) { return; }
  state.lexer.Eat();
}

void ParseBaseexpr(ParseState& state)
{
  TokenType tok = state.lexer.LookaheadType();
  switch (tok)
  {
    case TokenType::kHexadecimalLit:
    case TokenType::kDecimalLit:
    case TokenType::kOctalLit:
    case TokenType::kBinaryLit:
      ParseImm(state);
      break;

    case TokenType::kIdentifier:
      ParseIdLocation(state);
      break;

    case TokenType::kGPR:
    case TokenType::kFPR:
    case TokenType::kSPR:
    case TokenType::kCRField:
    case TokenType::kLt:
    case TokenType::kGt:
    case TokenType::kEq:
    case TokenType::kSo: 
      ParsePpcBuiltin(state);
      break;

    case TokenType::kDot:
      state.plugin.OnTerminal(Terminal::kDot, state.lexer.Lookahead());
      if (state.error) { return; }
      state.lexer.Eat();
      break;

    default:
      state.EmitErrorHere(
          fmt::format("Unexpected token '{}' in expression", state.lexer.LookaheadRef().ValStr()));
      break;
  }
}

void ParseBitor(ParseState& state);
void ParseParen(ParseState& state)
{
  if (state.HasToken(TokenType::kLparen))
  {
    state.plugin.OnOpenParen(ParenType::kNormal);
    if (state.error) { return; }

    state.lexer.Eat();
    ParseBitor(state);
    if (state.error) { return; }

    if (state.HasToken(TokenType::kRparen))
    {
      state.plugin.OnCloseParen(ParenType::kNormal);
    }
    state.ParseToken(TokenType::kRparen);
  }
  else if (state.HasToken(TokenType::kGrave))
  {
    state.plugin.OnOpenParen(ParenType::kRelConv);

    state.lexer.Eat();
    ParseBitor(state);
    if (state.error) { return; }

    if (state.HasToken(TokenType::kGrave))
    {
      state.plugin.OnCloseParen(ParenType::kRelConv);
    }
    state.ParseToken(TokenType::kGrave);
  }
  else
  {
    ParseBaseexpr(state);
  }
}

void ParseUnary(ParseState& state)
{
  TokenType tok = state.lexer.LookaheadType();
  if (tok == TokenType::kMinus || tok == TokenType::kTilde)
  {
    state.lexer.Eat();
    ParseUnary(state);
    if (state.error) { return; }

    if (tok == TokenType::kMinus)
    {
      state.plugin.OnOperator(AsmOp::kNeg);
    }
    else
    {
      state.plugin.OnOperator(AsmOp::kNot);
    }
  }
  else
  {
    ParseParen(state);
  }
}

void ParseMultiplication(ParseState& state)
{
  ParseUnary(state);
  if (state.error) { return; }

  TokenType tok = state.lexer.LookaheadType();
  while (tok == TokenType::kStar || tok == TokenType::kSlash)
  {
    state.lexer.Eat();
    ParseUnary(state);
    if (state.error) { return; }

    if (tok == TokenType::kStar)
    {
      state.plugin.OnOperator(AsmOp::kMul);
    }
    else
    {
      state.plugin.OnOperator(AsmOp::kDiv);
    }
    tok = state.lexer.LookaheadType();
  }
}

void ParseAddition(ParseState& state)
{
  ParseMultiplication(state);
  if (state.error) { return; }

  TokenType tok = state.lexer.LookaheadType();
  while (tok == TokenType::kPlus || tok == TokenType::kMinus)
  {
    state.lexer.Eat();
    ParseMultiplication(state);
    if (state.error) { return; }

    if (tok == TokenType::kPlus)
    {
      state.plugin.OnOperator(AsmOp::kAdd);
    }
    else
    {
      state.plugin.OnOperator(AsmOp::kSub);
    }
    tok = state.lexer.LookaheadType();
  }
}

void ParseShift(ParseState& state)
{
  ParseAddition(state);
  if (state.error) { return; }

  TokenType tok = state.lexer.LookaheadType();
  while (tok == TokenType::kLsh || tok == TokenType::kRsh)
  {
    state.lexer.Eat();
    ParseAddition(state);
    if (state.error) { return; }

    if (tok == TokenType::kLsh)
    {
      state.plugin.OnOperator(AsmOp::kLsh);
    }
    else
    {
      state.plugin.OnOperator(AsmOp::kRsh);
    }
    tok = state.lexer.LookaheadType();
  }
}

void ParseBitand(ParseState& state)
{
  ParseShift(state);
  if (state.error) { return; }

  while (state.HasToken(TokenType::kAmpersand))
  {
    state.lexer.Eat();
    ParseShift(state);
    if (state.error) { return; }

    state.plugin.OnOperator(AsmOp::kAnd);
  }
}

void ParseBitxor(ParseState& state)
{
  ParseBitand(state);
  if (state.error) { return; }

  while (state.HasToken(TokenType::kCaret))
  {
    state.lexer.Eat();
    ParseBitand(state);
    if (state.error) { return; }

    state.plugin.OnOperator(AsmOp::kXor);
  }
}

void ParseBitor(ParseState& state)
{
  ParseBitxor(state);
  if (state.error) { return; }

  while (state.HasToken(TokenType::kPipe))
  {
    state.lexer.Eat();
    ParseBitxor(state);
    if (state.error) { return; }

    state.plugin.OnOperator(AsmOp::kOr);
  }
}

void ParseOperand(ParseState& state)
{
  state.plugin.OnOperandPre();
  ParseBitor(state);
  if (state.error) { return; }
  state.plugin.OnOperandPost();
}

void ParseOperandList(ParseState& state, ParseAlg alg)
{
  if (alg == ParseAlg::None)
  {
    return;
  }
  if (alg == ParseAlg::NoneOrOp1)
  {
    if (MatchOperandFirst(state.lexer.Lookahead()))
    {
      ParseOperand(state);
    }
    return;
  }

  enum ParseStep
  {
    _Operand, _Comma, _Lparen, _Rparen, _OptComma
  };
  std::vector<ParseStep> steps;

  switch (alg)
  {
    case ParseAlg::Op1:
      steps = {_Operand}; break;
    case ParseAlg::Op1Or2:
      steps = {_Operand, _OptComma, _Operand}; break;
    case ParseAlg::Op2Or3:
      steps = {_Operand, _Comma, _Operand, _OptComma, _Operand}; break;
    case ParseAlg::Op1Off1:
      steps = {_Operand, _Comma, _Operand, _Lparen, _Operand, _Rparen}; break;
    case ParseAlg::Op2:
      steps = {_Operand, _Comma, _Operand}; break;
    case ParseAlg::Op3:
      steps = {_Operand, _Comma, _Operand, _Comma, _Operand}; break;
    case ParseAlg::Op4:
      steps = {_Operand, _Comma, _Operand, _Comma, _Operand, _Comma, _Operand}; break;
    case ParseAlg::Op5:
      steps = {_Operand, _Comma, _Operand, _Comma, _Operand, _Comma, _Operand, _Comma, _Operand}; break;
    case ParseAlg::Op1Off1Op2:
      steps = {_Operand, _Comma, _Operand, _Lparen, _Operand, _Rparen, _Comma, _Operand, _Comma, _Operand}; break;
    default:
      ASSERT(false);
      return;
  }


  for (ParseStep step : steps)
  {
    bool stop_parse = false;
    switch (step)
    {
      case _Operand:
        ParseOperand(state); break;
      case _Comma:
        state.ParseToken(TokenType::kComma); break;
      case _Lparen:
        state.ParseToken(TokenType::kLparen); break;
      case _Rparen:
        state.ParseToken(TokenType::kRparen); break;
      case _OptComma:
        if (state.HasToken(TokenType::kComma))
        {
          state.ParseToken(TokenType::kComma);
        }
        else
        {
          stop_parse = true;
        }
        break;
    }
    if (state.error) { return; }
    if (stop_parse)
    {
      break;
    }
  }
}

void ParseInstruction(ParseState& state)
{
  state.lexer.SetIdentifierMatchRule(Lexer::IdentifierMatchRule::kMnemonic);

  AssemblerToken mnemonic_token = state.lexer.Lookahead();
  if (mnemonic_token.token_type != TokenType::kIdentifier)
  {
    state.lexer.SetIdentifierMatchRule(Lexer::IdentifierMatchRule::kTypical);
    return;
  }

  auto mnemonic_tokens_it = mnemonic_tokens.find(mnemonic_token.token_val);
  bool is_extended = false;
  if (mnemonic_tokens_it == mnemonic_tokens.end())
  {
    mnemonic_tokens_it = extended_mnemonic_tokens.find(mnemonic_token.token_val);
    if (mnemonic_tokens_it == extended_mnemonic_tokens.end())
    {
      state.EmitErrorHere(
        fmt::format("Unknown or unsupported mnemonic '{}'", mnemonic_token.ValStr()));
      return;
    }
    is_extended = true;
  }

  state.plugin.OnInstructionPre(mnemonic_tokens_it->second, is_extended);

  state.lexer.EatAndReset();

  ParseOperandList(state, mnemonic_tokens_it->second.parse_algorithm);
  if (state.error) { return; }

  state.plugin.OnInstructionPost(mnemonic_tokens_it->second, is_extended);
}

void ParseLabel(ParseState& state)
{
  std::array<AssemblerToken, 2> tokens;
  state.lexer.LookaheadN(tokens);

  if (tokens[0].token_type == TokenType::kIdentifier &&
      tokens[1].token_type == TokenType::kColon)
  {
    state.plugin.OnLabelDecl(tokens[0].token_val);
    if (state.error) { return; }
    state.lexer.EatN<2>();
  }
}

void ParseResolvedExpr(ParseState& state)
{
  state.plugin.OnResolvedExprPre();
  ParseBitor(state);
  if (state.error) { return; }
  state.plugin.OnResolvedExprPost();
}

void ParseExpressionList(ParseState& state)
{
  ParseResolvedExpr(state);
  if (state.error) { return; }

  while (state.HasToken(TokenType::kComma))
  {
    state.lexer.Eat();
    ParseResolvedExpr(state);
    if (state.error) { return; }
  }
}

void ParseFloat(ParseState& state)
{
  AssemblerToken flt_token = state.lexer.LookaheadFloat();
  if (flt_token.token_type != TokenType::kFloatLit)
  {
    state.EmitErrorHere("Invalid floating point literal");
    return;
  }
  state.plugin.OnTerminal(Terminal::kFlt, flt_token);
  state.lexer.Eat();
}

void ParseFloatList(ParseState& state)
{
  ParseFloat(state);
  if (state.error) { return; }

  while (state.HasToken(TokenType::kComma))
  {
    state.lexer.Eat();
    ParseFloat(state);
    if (state.error) { return; }
  }
}

void ParseDefvar(ParseState& state)
{
  AssemblerToken tok = state.lexer.Lookahead();
  if (tok.token_type == TokenType::kIdentifier)
  {
    state.plugin.OnVarDecl(tok.token_val);
    if (state.error) { return; }
    state.lexer.Eat();

    state.ParseToken(TokenType::kComma);
    if (state.error) { return; }

    ParseResolvedExpr(state);
  }
  else
  {
    state.EmitErrorHere(
        fmt::format("Expected an identifier, but found '{}'", tok.ValStr()));
  }
}

void ParseString(ParseState& state)
{
  AssemblerToken tok = state.lexer.Lookahead();
  if (tok.token_type == TokenType::kStringLit)
  {
    state.plugin.OnTerminal(Terminal::kStr, tok);
    state.lexer.Eat();
  }
  else
  {
    state.EmitErrorHere(
        fmt::format("Expected a string literal, but found '{}'", tok.ValStr()));
  }
}

void ParseDirective(ParseState& state)
{
  //TODO: test directives
  state.lexer.SetIdentifierMatchRule(
      Lexer::IdentifierMatchRule::kDirective
    );
  AssemblerToken tok = state.lexer.Lookahead();
  if (tok.token_type != TokenType::kIdentifier)
  {
    state.EmitErrorHere(
        fmt::format("Unexpected token '{}' in directive type", tok.ValStr()));
    return;
  }

  auto directive_it = directives_map.find(tok.token_val);
  if (directive_it == directives_map.end())
  {
    state.EmitErrorHere(
        fmt::format("Unknown assembler directive '{}'", tok.ValStr()));
    return;
  }

  state.plugin.OnDirectivePre(directive_it->second);

  state.lexer.EatAndReset();
  switch (directive_it->second)
  {
    case GekkoDirective::kByte:
    case GekkoDirective::k2byte:
    case GekkoDirective::k4byte:
    case GekkoDirective::k8byte:
      ParseExpressionList(state);
      break;

    case GekkoDirective::kFloat:
    case GekkoDirective::kDouble:
      ParseFloatList(state);
      break;

    case GekkoDirective::kLocate:
    case GekkoDirective::kZeros:
    case GekkoDirective::kSkip:
      ParseResolvedExpr(state);
      break;

    case GekkoDirective::kPadAlign:
    case GekkoDirective::kAlign:
      ParseImm(state);
      break;

    case GekkoDirective::kDefVar:
      ParseDefvar(state);
      break;

    case GekkoDirective::kAscii:
    case GekkoDirective::kAsciz:
      ParseString(state);
      break;
  }

  if (state.error) { return; }

  state.plugin.OnDirectivePost(directive_it->second);
}

void ParseLine(ParseState& state)
{
  if (state.HasToken(TokenType::kDot))
  {
    state.ParseToken(TokenType::kDot);
    ParseDirective(state);
  }
  else
  {
    ParseInstruction(state);
  }
}

void ParseProgram(ParseState& state)
{
  AssemblerToken tok = state.lexer.Lookahead();
  if (tok.token_type == TokenType::kEof)
  {
    state.eof = true;
    return;
  }
  ParseLabel(state);
  if (state.error) { return; }
  ParseLine(state);
  if (state.error) { return; }
  
  while (!state.eof && !state.error)
  {
    tok = state.lexer.Lookahead();
    if (tok.token_type == TokenType::kEof)
    {
      state.eof = true;
    }
    else if (tok.token_type == TokenType::kEol)
    {
      state.lexer.Eat();
      ParseLabel(state);
      if (state.error) { return; }
      ParseLine(state);
    }
    else
    {
      state.EmitErrorHere(
        fmt::format("Unexpected token '{}' where line should have ended",
                    tok.ValStr()));
    }
  }
}
}  // namespace

ParseState::ParseState(std::string_view input_str, ParsePlugin& plugin)
  : lexer(input_str),
    plugin(plugin),
    eof(false) {}

bool ParseState::HasToken(TokenType tp)
{
  return lexer.LookaheadType() == tp;
}

void ParseState::ParseToken(TokenType tp)
{
  AssemblerToken tok = lexer.LookaheadRef();
  if (tok.token_type == tp)
  {
    lexer.Eat();
  }
  else
  {
    EmitErrorHere(
      fmt::format("Expected '{}' but found '{}'", TokenTypeToStr(tp), tok.ValStr()));
  }
}

void ParseState::EmitErrorHere(std::string&& message)
{
  AssemblerToken cur_token = lexer.Lookahead();
  if (cur_token.token_type == TokenType::kInvalid)
  {
    error = AssemblerError {
      std::string(cur_token.invalid_reason),
      lexer.CurrentLine(),
      lexer.LineNumber(),
      lexer.ColNumber() + cur_token.invalid_region.begin,
      cur_token.invalid_region.len,
    };
  }
  else
  {
    error = AssemblerError {
      std::move(message),
      lexer.CurrentLine(),
      lexer.LineNumber(),
      lexer.ColNumber(),
      cur_token.token_val.size(),
    };
  }
}

void ParseWithPlugin(ParsePlugin* plugin, std::string_view input)
{
  ParseState parse_state = ParseState(input, *plugin);
  plugin->SetOwner(&parse_state);
  ParseProgram(parse_state);

  if (parse_state.error)
  {
    plugin->OnError();
    plugin->ForwardError(std::move(*parse_state.error));
  }
  else
  {
    plugin->PostParseAction();
    if (parse_state.error)
    {
      plugin->OnError();
      plugin->ForwardError(std::move(*parse_state.error));
    }
  }

  plugin->SetOwner(nullptr);
}
}  // namespace Common::GekkoAssembler::detail
