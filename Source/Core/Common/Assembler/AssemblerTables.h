#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <vector>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/CommonTypes.h"

namespace Common::GekkoAssembler::detail
{
///////////////////
// PARSER TABLES //
///////////////////
enum class ParseAlg
{
  None, Op1, NoneOrOp1, Op1Off1, Op2, Op1Or2, Op3, Op2Or3, Op4, Op5, Op1Off1Op2,
};

struct ParseInfo
{
  size_t mnemonic_index;
  ParseAlg parse_algorithm;
};

// Mapping of SPRG names to values
extern std::map<std::string, u32, std::less<>> sprg_map;
// Mapping of directive names to an enumeration
extern std::map<std::string, GekkoDirective, std::less<>> directives_map;
// Mapping of normal Gekko mnemonics to their index and argument form
extern std::map<std::string, ParseInfo, std::less<>> mnemonic_tokens;
// Mapping of extended Gekko mnemonics to their index and argument form
extern std::map<std::string, ParseInfo, std::less<>> extended_mnemonic_tokens;

//////////////////////
// ASSEMBLER TABLES //
//////////////////////
constexpr size_t kMaxOperands = 5;

struct OperandList
{
  std::array<Tagged<Interval, u32>, kMaxOperands> list;
  u32 count;
  bool overfill;

  constexpr u32 operator[](size_t index) const { return value_of(list[index]); }
  constexpr u32& operator[](size_t index) { return value_of(list[index]); }

  void Insert(size_t before, u32 val);

  template <typename It>
  void Copy(It begin, It end)
  {
    count = 0;
    for (auto& i : list)
    {
      if (begin == end)
      {
        break;
      }
      i = *begin; begin++;
      count++;
    }
    overfill = begin != end;
  }
};

struct OperandDesc
{
  u32 mask;
  struct
  {
    u32 shift : 31;
    bool is_signed : 1;
  };
  u32 MaxVal() const;
  u32 MinVal() const;
  u32 TruncBits() const;

  bool Fits(u32 val) const;
  u32 Fit(u32 val) const;
};

// MnemonicDesc holds the machine-code template for mnemonics
struct MnemonicDesc
{
  // Initial value for a given mnemonic (opcode, func code, LK, AA, OE)
  const u32 initial_value;
  const u32 operand_count;
  // Masks for operands
  std::array<OperandDesc, kMaxOperands> operand_masks;
};

// ExtendedMnemonicDesc holds the name of the mnemonic it transforms to as well as a
// transformer callback to translate the operands into the correct form for the base mnemonic
struct ExtendedMnemonicDesc
{
  size_t mnemonic_index;
  void (*transform_operands)(OperandList&);
};

// Table for mapping mnemonic+variants to their descriptors
extern std::array<MnemonicDesc, kNumMnemonics * kVariantPermutations> mnemonics;
// Table for mapping extended mnemonic+variants to their descriptors
extern std::array<ExtendedMnemonicDesc, kNumExtMnemonics * kVariantPermutations> extended_mnemonics;

//////////////////
// LEXER TABLES //
//////////////////

// In place of the reliace on std::regex, DFAs will be defined for matching sufficiently complex tokens
// This gives an extra benefit of providing reasons for match failures
using TransitionF = bool(*)(char c);
using DfaEdge = std::pair<TransitionF, size_t>;
struct DfaNode
{
  std::vector<DfaEdge> edges;
  // If nullopt: this is a final node
  // If string: invalid reason
  std::optional<std::string_view> match_failure_reason;
};

// Floating point strings that will be accepted by std::stof/std::stod
// regex: [\+-]?(\d+(\.\d+)?|\.\d+)(e[\+-]?\d+)?
extern std::vector<DfaNode> float_dfa;
// C-style strings
// regex: "([^\\\n]|\\([0-7]{1,3}|x[0-9a-fA-F]+|[^x0-7\n]))*"
extern std::vector<DfaNode> string_dfa;
}  // namespace Common::GekkoAssembler::detail
