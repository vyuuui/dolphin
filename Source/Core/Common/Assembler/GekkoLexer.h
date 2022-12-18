#pragma once

#include <algorithm>
#include <array>
#include <deque>
#include <optional>
#include <string_view>
#include <vector>
#include <type_traits>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/Assembler/AssemblerTables.h"
#include "Common/CommonTypes.h"

namespace Common::GekkoAssembler::detail {
void ConvertStringLiteral(std::string_view literal, std::vector<u8>& out_vec);

// Could also use std::unsigned_integral || std::floating_point
template <typename T>
concept TokenConvertable = std::is_same_v<T, u8> || std::is_same_v<T, u16> || 
                           std::is_same_v<T, u32> || std::is_same_v<T, u64> || 
                           std::is_same_v<T, float> || std::is_same_v<T, double>;
enum class TokenType {
  kInvalid,
  kIdentifier,
  kStringLit,
  kHexadecimalLit,
  kDecimalLit,
  kOctalLit,
  kBinaryLit,
  kFloatLit,
  kGPR,
  kFPR,
  kCRField,
  kSPR,
  kLt,
  kGt,
  kEq,
  kSo,
  // EOL signifies boundaries between instructions, a la ';'
  kEol,
  kEof,

  kDot,
  kColon,
  kComma,
  kLparen,
  kRparen,
  kPipe,
  kCaret,
  kAmpersand,
  kLsh,
  kRsh,
  kPlus,
  kMinus,
  kStar,
  kSlash,
  kTilde,
  kGrave,
  kAt,

  kOperatorBegin = kDot,
  kLastToken = kAt,
};

struct AssemblerToken {
  TokenType token_type;
  std::string_view token_val;
  std::string_view invalid_reason;
  // Within an invalid token, specifies the erroneous region
  Interval invalid_region;

  std::string_view TypeStr() const;
  std::string_view ValStr() const;

  // Supported Templates:
  // u8, u16, u32, u64, float, double
  template <TokenConvertable T>
  std::optional<T> EvalToken() const;
};

std::string_view TokenTypeToStr(TokenType);

class Lexer {
public:
  enum class IdentifierMatchRule {
    kTypical,
    kMnemonic,  // Mnemonics can contain +, -, or . to specify branch prediction rules and link bit
    kDirective,  // Directives can start with a digit
  };

private:
  struct CursorPosition {
    CursorPosition() : index(0), line(0), col(0) {}
    size_t index;
    size_t line, col;
  };
  std::string_view lex_string;
  mutable CursorPosition pos, scan_pos;
  mutable std::deque<Tagged<CursorPosition, AssemblerToken>> lexed_tokens;
  IdentifierMatchRule match_rule;

public:
  Lexer(std::string_view str) : lex_string(str), match_rule(IdentifierMatchRule::kTypical) {}

  size_t LineNumber() const;
  size_t ColNumber() const;
  std::string_view CurrentLine() const;

  // Since there's only one place floats get lexed, it's 'okay' to have an explicit
  // "lex a float token" function
  void SetIdentifierMatchRule(IdentifierMatchRule set);
  Tagged<CursorPosition, AssemblerToken> const&
    LookaheadTagRef(size_t num_fwd) const;
  AssemblerToken Lookahead() const;
  AssemblerToken const& LookaheadRef() const;
  TokenType LookaheadType() const;
  // Since there's only one place floats get lexed, it's 'okay' to have an explicit
  // "lex a float token" function
  AssemblerToken LookaheadFloat() const;
  void Eat();
  void EatAndReset();

  template <size_t N>
  void LookaheadTaggedN(std::array<Tagged<CursorPosition, AssemblerToken>, N>& tokens_out) const {
    const size_t filled_amt = std::min(lexed_tokens.size(), N);

    std::copy_n(lexed_tokens.begin(), filled_amt, tokens_out.begin());

    std::generate_n(tokens_out.begin() + filled_amt, N - filled_amt,
                    [this] { auto p = pos; return lexed_tokens.emplace_back(p, LexSingle()); });
  }

  template <size_t N>
  void LookaheadN(std::array<AssemblerToken, N>& tokens_out) const {
    const size_t filled_amt = std::min(lexed_tokens.size(), N);

    auto _it = lexed_tokens.begin();
    std::generate_n(tokens_out.begin(), filled_amt,
                    [&_it] { return value_of(*_it++); });

    std::generate_n(tokens_out.begin() + filled_amt, N - filled_amt,
                    [this] { auto p = pos; return value_of(lexed_tokens.emplace_back(p, LexSingle())); });
  }

  template <size_t N>
  void EatN() {
    size_t consumed = 0;
    while (lexed_tokens.size() > 0 && consumed < N) {
      lexed_tokens.pop_front();
      consumed++;
    }
    for (size_t i = consumed; i < N; i++) {
      LexSingle();
    }
  }

private:
  std::optional<std::string_view> RunDfa(std::vector<DfaNode> const& dfa) const;
  void SkipWs() const;
  void FeedbackTokens() const;
  bool IdentifierHeadExtra(char h) const;
  bool IdentifierExtra(char c) const;
  void ScanStart() const;
  void ScanFinish() const;
  std::string_view ScanFinishOut() const;
  char Peek() const;
  Lexer const& Step() const;
  TokenType LexStringLit(std::string_view& invalid_reason, Interval& invalid_region) const;
  TokenType ClassifyAlnum() const;
  AssemblerToken LexSingle() const;
};
}
