#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/Assembler/GekkoLexer.h"
#include "Common/CommonTypes.h"

namespace Common::GekkoAssembler::detail
{
class ParsePlugin;

struct ParseState
{
  Lexer lexer;
  ParsePlugin& plugin; 

  std::optional<AssemblerError> error;
  bool eof;

  ParseState(std::string_view input_str, ParsePlugin& plugin);

  bool HasToken(TokenType tp);
  void ParseToken(TokenType tp);
  void EmitErrorHere(std::string&& message);
};

enum class AsmOp
{
  kOr,
  kXor,
  kAnd,
  kLsh, kRsh,
  kAdd, kSub,
  kMul, kDiv,
  kNeg, kNot
};

enum class Terminal
{
  kHex, kDec, kOct, kBin, kFlt, kStr,
  kId,
  kGPR, kFPR, kSPR, kCRField, kLt, kGt, kEq, kSo,
  kDot,
};

enum class ParenType
{
  kNormal, kRelConv,
};

// Overridable plugin class supporting a series of skeleton functions which get called when
// the parser parses a given point of interest
class ParsePlugin
{
protected:
  ParseState* owner;
  std::optional<AssemblerError> owner_error;

public:
  ParsePlugin() : owner(nullptr) {}

  void SetOwner(ParseState* o) { owner = o; }
  void ForwardError(AssemblerError&& err) { owner_error = std::move(err); }
  std::optional<AssemblerError>& Error() { return owner_error; }

  virtual void PostParseAction() {}

  // Nonterminal callouts
  // Pre occurs prior to the head nonterminal being parsed
  // Post occurs after the nonterminal has been fully parsed
  virtual void OnDirectivePre(GekkoDirective directive) {}
  virtual void OnDirectivePost(GekkoDirective directive) {}
  virtual void OnInstructionPre(ParseInfo const& mnemonic_info, bool extended) {}
  virtual void OnInstructionPost(ParseInfo const& mnemonic_info, bool extended) {}
  virtual void OnOperandPre() {}
  virtual void OnOperandPost() {}
  virtual void OnResolvedExprPre() {}
  virtual void OnResolvedExprPost() {}

  // Operator callouts
  // All occur after the relevant operands have been parsed
  virtual void OnOperator(AsmOp operation) {}

  // Individual token callouts
  // All occur prior to the token being parsed
  // Due to ambiguity of some tokens, an explicit operation is provided
  virtual void OnTerminal(Terminal type, AssemblerToken const& val) {}
  virtual void OnHiaddr(std::string_view id) {}
  virtual void OnLoaddr(std::string_view id) {}
  virtual void OnOpenParen(ParenType type) {}
  virtual void OnCloseParen(ParenType type) {}
  virtual void OnError() {}
  virtual void OnLabelDecl(std::string_view name) {}
  virtual void OnVarDecl(std::string_view name) {}
};

// Parse the provided input with a plugin to handle what to do with certain points of interest
// e.g. Convert to an IR for generating final machine code, picking up syntactical information
void ParseWithPlugin(ParsePlugin* plugin, std::string_view input);
}  // namespace Common::GekkoAssembler::detail
