#include "Common/Assembler/GekkoIRGen.h"

#include <concepts>
#include <functional>
#include <numeric>
#include <set>
#include <stack>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/Assembler/GekkoParser.h"
#include "Common/Assert.h"

namespace Common::GekkoAssembler::detail
{
namespace
{
template <typename... Ts> struct overload : Ts... { using Ts::operator()...; };
template <typename... Ts> overload(Ts...) -> overload<Ts...>;

class GekkoIRPlugin : public ParsePlugin
{
private:
  enum class EvalMode
  {
    RelAddrDoublePass,
    AbsAddrSinglePass,
  };
  GekkoIR& output_result;

  IRBlock* active_block;
  GekkoInstruction build_inst;
  u64* active_var;
  size_t operand_scan_begin;

  std::map<std::string, u32, std::less<>> labels;
  std::map<std::string, u64, std::less<>> constants;
  std::set<std::string, std::less<>> symset;

  EvalMode evaluation_mode;

  // For operand parsing
  std::stack<std::function<u32()>> fixup_stack;
  std::vector<std::function<u32()>> operand_fixups;
  size_t operand_str_start;

  using EvalStack =
    std::variant<std::vector<u8>,
                 std::vector<u16>,
                 std::vector<u32>,
                 std::vector<u64>,
                 std::vector<float>,
                 std::vector<double>>;
  // For directive parsing
  EvalStack eval_stack;
  std::string_view string_lit;
  GekkoDirective active_directive;

public:
  GekkoIRPlugin(GekkoIR& result, u32 base_addr)
    : output_result(result),
      active_var(nullptr),
      operand_scan_begin(0)
  {
    active_block = &output_result.blocks.emplace_back(base_addr);
  }

  void OnDirectivePre(GekkoDirective directive) override;
  void OnDirectivePost(GekkoDirective directive) override;
  void OnInstructionPre(ParseInfo const& mnemonic_info, bool extended) override;
  void OnInstructionPost(ParseInfo const& mnemonic_info, bool extended) override;
  void OnOperandPre() override;
  void OnOperandPost() override;
  void OnResolvedExprPost() override;
  void OnOperator(AsmOp operation) override;
  void OnTerminal(Terminal type, AssemblerToken const& val) override;
  void OnHiaddr(std::string_view id) override;
  void OnLoaddr(std::string_view id) override;
  void OnCloseParen(ParenType type) override;
  void OnLabelDecl(std::string_view name) override;
  void OnVarDecl(std::string_view name) override;
  void PostParseAction() override;

  u32 CurrentAddress() const;
  std::optional<u64> LookupVar(std::string_view lab);
  std::optional<u32> LookupLabel(std::string_view lab);

  template <typename T>
  T& GetChunk();

  template <TokenConvertable T>
  void AddBytes(T val);

  void AddStringBytes(std::string_view str, bool null_term);

  void PadAlign(u32 bits);
  void PadSpace(size_t space);

  void StartBlock(u32 address);
  void StartBlockAlign(u32 bits);
  void StartInstruction(size_t mnemonic_index, bool extended);
  void FinishInstruction();
  void SaveOperandFixup(size_t str_left, size_t str_right);

  void AddBinaryEvaluator(u32(*evaluator)(u32, u32));
  void AddUnaryEvaluator(u32(*evaluator)(u32));
  void AddAbsoluteAddressConv();
  void AddLiteral(u32 lit);
  void AddSymbolResolve(std::string_view sym, bool absolute);

  void RunFixups();

  void EvalOperatorRel(AsmOp operation);
  void EvalOperatorAbs(AsmOp operation);
  void EvalTerminalRel(Terminal type, AssemblerToken const& tok);
  void EvalTerminalAbs(Terminal type, AssemblerToken const& tok);

  template <TokenConvertable T>
  void PushCast(T val);
  template <TokenConvertable T>
  void EvalTerminalAbsGeneric(Terminal type, AssemblerToken const& tok,
                              std::vector<T>& out_stack);
};


///////////////
// OVERRIDES //
///////////////


void GekkoIRPlugin::OnDirectivePre(GekkoDirective directive)
{
  evaluation_mode = EvalMode::AbsAddrSinglePass;
  active_directive = directive;

  switch (directive)
  {
    case GekkoDirective::kByte:
      eval_stack = std::vector<u8>{};
      break;

    case GekkoDirective::k2byte:
      eval_stack = std::vector<u16>{};
      break;

    case GekkoDirective::k4byte:
    case GekkoDirective::kLocate:
    case GekkoDirective::kPadAlign:
    case GekkoDirective::kAlign:
    case GekkoDirective::kZeros:
    case GekkoDirective::kSkip:
      eval_stack = std::vector<u32>{};
      break;

    case GekkoDirective::k8byte:
    case GekkoDirective::kDefVar:
      eval_stack = std::vector<u64>{};
      break;

    case GekkoDirective::kFloat:
      eval_stack = std::vector<float>{};
      break;

    case GekkoDirective::kDouble:
      eval_stack = std::vector<double>{};
      break;

    default:
      eval_stack = {};
      break;
  }
}

void GekkoIRPlugin::OnDirectivePost(GekkoDirective directive)
{
  switch (directive)
  {
    // .nbyte and .float/double directives are handled by OnResolvedExprPost
    default:
      break;

    case GekkoDirective::kDefVar:
      ASSERT(active_var != nullptr);
      *active_var = std::get<std::vector<u64>>(eval_stack).back();
      active_var = nullptr;
      break;

    case GekkoDirective::kLocate:
      StartBlock(std::get<std::vector<u32>>(eval_stack).back());
      break;

    case GekkoDirective::kZeros:
      PadSpace(std::get<std::vector<u32>>(eval_stack).back());
      break;

    case GekkoDirective::kSkip:
      StartBlock(CurrentAddress() + std::get<std::vector<u32>>(eval_stack).back());
      break;

    case GekkoDirective::kPadAlign:
      PadAlign(std::get<std::vector<u32>>(eval_stack).back());
      break;

    case GekkoDirective::kAlign:
      StartBlockAlign(std::get<std::vector<u32>>(eval_stack).back());
      break;

    case GekkoDirective::kAscii:
      AddStringBytes(string_lit, false);
      break;

    case GekkoDirective::kAsciz:
      AddStringBytes(string_lit, true);
      break;
  }
  eval_stack = {};
}

void GekkoIRPlugin::OnInstructionPre(ParseInfo const& mnemonic_info, bool extended)
{
  evaluation_mode = EvalMode::RelAddrDoublePass;
  StartInstruction(mnemonic_info.mnemonic_index, extended);
}

void GekkoIRPlugin::OnInstructionPost(ParseInfo const&, bool)
{
  FinishInstruction();
}

void GekkoIRPlugin::OnOperandPre()
{
  operand_str_start = owner->lexer.ColNumber();
}

void GekkoIRPlugin::OnOperandPost()
{
  SaveOperandFixup(operand_str_start, owner->lexer.ColNumber());
}

void GekkoIRPlugin::OnResolvedExprPost()
{
  switch (active_directive) {
    case GekkoDirective::kByte:
    case GekkoDirective::k2byte:
    case GekkoDirective::k4byte:
    case GekkoDirective::k8byte:
    case GekkoDirective::kFloat:
    case GekkoDirective::kDouble:
      eval_stack = std::visit(
        [this](auto&& vec) -> EvalStack
        {
          for (auto&& val : vec)
          {
            AddBytes(val);
          }
          vec.clear();
          return vec;
        }, eval_stack);
      break;

    default:
      break;
  }
}

void GekkoIRPlugin::OnOperator(AsmOp operation)
{
  if (evaluation_mode == EvalMode::RelAddrDoublePass)
  {
    EvalOperatorRel(operation);
  }
  else
  {
    EvalOperatorAbs(operation);
  }
}

void GekkoIRPlugin::OnTerminal(Terminal type, AssemblerToken const& val)
{
  if (type == Terminal::kStr)
  {
    string_lit = val.token_val;
  }
  else if (evaluation_mode == EvalMode::RelAddrDoublePass)
  {
    EvalTerminalRel(type, val);
  }
  else
  {
    EvalTerminalAbs(type, val);
  }
}

void GekkoIRPlugin::OnHiaddr(std::string_view id)
{
  if (evaluation_mode == EvalMode::RelAddrDoublePass)
  {
    AddSymbolResolve(id, true);
    AddLiteral(16);
    AddBinaryEvaluator([](u32 lhs, u32 rhs) { return lhs >> rhs; });
    AddLiteral(0xffff);
    AddBinaryEvaluator([](u32 lhs, u32 rhs) { return lhs & rhs; });
  }
  else
  {
    if (auto lbl = LookupLabel(id); lbl)
    {
      PushCast(*lbl);
    }
    else if (auto var = LookupVar(id); var) 
    {
      PushCast(*var);
    }
    else
    {
      owner->EmitErrorHere(
          fmt::format("Undefined reference to Label/Constant '{}'", id));
      return;
    }

    PushCast<u8>(16);
    EvalOperatorAbs(AsmOp::kRsh);
    PushCast<u16>(0xffff);
    EvalOperatorAbs(AsmOp::kAnd);
  }
}

void GekkoIRPlugin::OnLoaddr(std::string_view id)
{
  if (evaluation_mode == EvalMode::RelAddrDoublePass)
  {
    AddSymbolResolve(id, true);
    AddLiteral(0xffff);
    AddBinaryEvaluator([](u32 lhs, u32 rhs) { return lhs & rhs; });
  }
  else
  {
    if (auto lbl = LookupLabel(id); lbl)
    {
      PushCast(*lbl);
    }
    else if (auto var = LookupVar(id); var) 
    {
      PushCast(*var);
    }
    else
    {
      owner->EmitErrorHere(
          fmt::format("Undefined reference to Label/Constant '{}'", id));
      return;
    }

    PushCast<u16>(0xffff);
    EvalOperatorAbs(AsmOp::kAnd);
  }
}

void GekkoIRPlugin::OnCloseParen(ParenType type)
{
  if (evaluation_mode == EvalMode::RelAddrDoublePass)
  {
    if (type == ParenType::kRelConv)
    {
      AddAbsoluteAddressConv();
    }
  }
  else
  {
    PushCast(CurrentAddress());
    EvalOperatorAbs(AsmOp::kSub);
  }
}

void GekkoIRPlugin::OnLabelDecl(std::string_view name)
{
  if (symset.contains(name))
  {
    owner->EmitErrorHere(fmt::format("Label/Constant {} is already defined", name));
    return;
  }

  labels[std::string(name)] = active_block->BlockEndAddress();
  symset.insert(std::string(name));
}

void GekkoIRPlugin::OnVarDecl(std::string_view name)
{
  if (symset.contains(name))
  {
    owner->EmitErrorHere(fmt::format("Label/Constant {} is already defined", name));
    return;
  }

  active_var = &constants[std::string(name)];
  symset.insert(std::string(name));
}

void GekkoIRPlugin::PostParseAction()
{
  RunFixups();
}


//////////////////////
// HELPER FUNCTIONS //
//////////////////////


u32 GekkoIRPlugin::CurrentAddress() const
{
  return active_block->BlockEndAddress();
}

std::optional<u64> GekkoIRPlugin::LookupVar(std::string_view var)
{
  auto var_it = constants.find(var);
  return var_it == constants.end() ? std::nullopt : std::optional(var_it->second);
}

std::optional<u32> GekkoIRPlugin::LookupLabel(std::string_view lab)
{
  auto label_it = labels.find(lab);
  return label_it == labels.end() ? std::nullopt : std::optional(label_it->second);
}

void GekkoIRPlugin::AddStringBytes(std::string_view str, bool null_term)
{
  ByteChunk& bytes = GetChunk<ByteChunk>();
  ConvertStringLiteral(str, bytes);
  if (null_term)
  {
    bytes.push_back('\0');
  }
}

template <typename T>
T& GekkoIRPlugin::GetChunk()
{
  if (!active_block->chunks.empty() &&
      std::holds_alternative<T>(active_block->chunks.back()))
  {
    return std::get<T>(active_block->chunks.back());
  }

  return std::get<T>(active_block->chunks.emplace_back(T{}));
}

template <TokenConvertable T>
void GekkoIRPlugin::AddBytes(T val)
{
  if constexpr (std::is_integral_v<T>)
  {
    ByteChunk& bytes = GetChunk<ByteChunk>();
    for (size_t i = sizeof(T) - 1; i > 0; i--)
    {
      bytes.push_back((val >> (8 * i)) & 0xff);
    }
    bytes.push_back(val & 0xff);
  }
  else if constexpr (std::is_same_v<T, float>)
  {
    static_assert(sizeof(double) == sizeof(u64));
    AddBytes(std::bit_cast<u32>(val));
  }
  else
  {
    // std::is_same_v<T, double>
    static_assert(sizeof(double) == sizeof(u64));
    AddBytes(std::bit_cast<u64>(val));
  }
}

void GekkoIRPlugin::PadAlign(u32 bits)
{
  const u32 align_mask = (1 << bits) - 1;
  u32 current_addr = active_block->BlockEndAddress();
  if (current_addr & align_mask)
  {
    PadChunk& current_pad = GetChunk<PadChunk>();
    current_pad += (1 << bits) - (current_addr & align_mask);
  }
}

void GekkoIRPlugin::PadSpace(size_t space)
{
  GetChunk<PadChunk>() += space;
}

void GekkoIRPlugin::StartBlock(u32 address)
{
  active_block = &output_result.blocks.emplace_back(address);
}

void GekkoIRPlugin::StartBlockAlign(u32 bits)
{
  const u32 align_mask = (1 << bits) - 1;
  u32 current_addr = active_block->BlockEndAddress();
  if (current_addr & align_mask)
  {
    StartBlock((1 << bits) + (current_addr & ~align_mask));
  }
}

void GekkoIRPlugin::StartInstruction(size_t mnemonic_index, bool extended)
{
  build_inst = GekkoInstruction {
    .mnemonic_index = mnemonic_index,
    .raw_text = owner->lexer.CurrentLine(),
    .line_number = owner->lexer.LineNumber(),
    .is_extended = extended,
  };
  operand_scan_begin = output_result.operand_pool.size();
}

void GekkoIRPlugin::AddBinaryEvaluator(u32(*evaluator)(u32, u32))
{
  std::function<u32()> rhs = std::move(fixup_stack.top());
  fixup_stack.pop();
  std::function<u32()> lhs = std::move(fixup_stack.top());
  fixup_stack.pop();
  fixup_stack.emplace([evaluator, lhs = std::move(lhs), rhs = std::move(rhs)]() { return evaluator(lhs(), rhs()); });
}

void GekkoIRPlugin::AddUnaryEvaluator(u32(*evaluator)(u32))
{
  std::function<u32()> sub = std::move(fixup_stack.top());
  fixup_stack.pop();
  fixup_stack.emplace([evaluator, sub = std::move(sub)]() { return evaluator(sub()); });
}

void GekkoIRPlugin::AddAbsoluteAddressConv()
{
  const u32 inst_address = active_block->BlockEndAddress();
  std::function<u32()> sub = std::move(fixup_stack.top());
  fixup_stack.pop();
  fixup_stack.emplace([inst_address, sub = std::move(sub)] { return sub() - inst_address; });
}

void GekkoIRPlugin::AddLiteral(u32 lit)
{
  fixup_stack.emplace([lit] { return lit; });
}

void GekkoIRPlugin::AddSymbolResolve(std::string_view sym, bool absolute)
{
  const u32 source_address = active_block->BlockEndAddress();
  AssemblerError err_on_fail = AssemblerError {
    fmt::format("Unresolved symbol '{}'", sym),
    owner->lexer.CurrentLine(),
    owner->lexer.LineNumber(),
    // Lexer should currently point to the label, as it hasn't been eaten yet
    owner->lexer.ColNumber(),
    sym.size(),
  };

  fixup_stack.emplace(
      [this, sym, absolute, source_address, err_on_fail = std::move(err_on_fail)]
      {
        auto label_it = labels.find(sym);
        if (label_it != labels.end())
        {
          if (absolute)
          {
            return label_it->second;
          }
          return label_it->second - source_address;
        }

        auto var_it = constants.find(sym);
        if (var_it != constants.end())
        {
          return static_cast<u32>(var_it->second);
        }

        owner->error = std::move(err_on_fail);
        return u32{0};
      });
}

void GekkoIRPlugin::SaveOperandFixup(size_t str_left, size_t str_right)
{
  operand_fixups.emplace_back(std::move(fixup_stack.top()));
  fixup_stack.pop();
  output_result.operand_pool.emplace_back(Interval {str_left, str_right - str_left}, 0);
}

void GekkoIRPlugin::RunFixups()
{
  for (size_t i = 0; i < operand_fixups.size(); i++)
  {
    value_of(output_result.operand_pool[i]) = operand_fixups[i]();
    if (owner->error) { return; }
  }
}

void GekkoIRPlugin::FinishInstruction()
{
  build_inst.op_index = operand_scan_begin;
  build_inst.op_count = output_result.operand_pool.size() - operand_scan_begin;
  GetChunk<InstChunk>().emplace_back(build_inst);
  operand_scan_begin = 0;
}

void GekkoIRPlugin::EvalOperatorAbs(AsmOp operation)
{
#define VISIT_BINARY_OP(OPERATOR) \
  std::visit(overload { \
      [](auto&& vec) { \
        auto rhs = vec.back(); \
        vec.pop_back(); \
        vec.back() = vec.back() OPERATOR rhs; \
      }, \
      []<std::floating_point T>(std::vector<T>& vec) { ASSERT(false); }\
    }, eval_stack); \

#define VISIT_UNARY_OP(OPERATOR) \
  std::visit(overload { \
      [](auto&& vec) { vec.back() = OPERATOR vec.back(); }, \
      []<std::floating_point T>(std::vector<T>& vec) { ASSERT(false); }\
    }, eval_stack); \

  switch (operation)
  {
    case AsmOp::kOr:
      VISIT_BINARY_OP(|);
      break;
    case AsmOp::kXor: 
      VISIT_BINARY_OP(^);
      break;
    case AsmOp::kAnd:
      VISIT_BINARY_OP(&);
      break;
    case AsmOp::kLsh:
      VISIT_BINARY_OP(<<);
      break;
    case AsmOp::kRsh:
      VISIT_BINARY_OP(>>);
      break;
    case AsmOp::kAdd:
      VISIT_BINARY_OP(+);
      break;
    case AsmOp::kSub:
      VISIT_BINARY_OP(-);
      break;
    case AsmOp::kMul:
      VISIT_BINARY_OP(*);
      break;
    case AsmOp::kDiv:
      VISIT_BINARY_OP(/);
      break;
    case AsmOp::kNeg:
      VISIT_UNARY_OP(-);
      break;
    case AsmOp::kNot:
      VISIT_UNARY_OP(~);
      break;
  }
#undef VISIT_BINARY_OP
#undef VISIT_UNARY_OP
}

void GekkoIRPlugin::EvalOperatorRel(AsmOp operation)
{
  switch (operation)
  {
    case AsmOp::kOr:
      AddBinaryEvaluator([](u32 lhs, u32 rhs) { return lhs | rhs; });
      break;
    case AsmOp::kXor:
      AddBinaryEvaluator([](u32 lhs, u32 rhs) { return lhs ^ rhs; });
      break;
    case AsmOp::kAnd:
      AddBinaryEvaluator([](u32 lhs, u32 rhs) { return lhs & rhs; });
      break;
    case AsmOp::kLsh:
      AddBinaryEvaluator([](u32 lhs, u32 rhs) { return lhs << rhs; });
      break;
    case AsmOp::kRsh:
      AddBinaryEvaluator([](u32 lhs, u32 rhs) { return lhs >> rhs; });
      break;
    case AsmOp::kAdd:
      AddBinaryEvaluator([](u32 lhs, u32 rhs) { return lhs + rhs; });
      break;
    case AsmOp::kSub:
      AddBinaryEvaluator([](u32 lhs, u32 rhs) { return lhs - rhs; });
      break;
    case AsmOp::kMul:
      AddBinaryEvaluator([](u32 lhs, u32 rhs) { return lhs * rhs; });
      break;
    case AsmOp::kDiv:
      AddBinaryEvaluator([](u32 lhs, u32 rhs) { return lhs / rhs; });
      break;
    case AsmOp::kNeg:
      AddUnaryEvaluator([](u32 val) { return -val; });
      break;
    case AsmOp::kNot:
      AddUnaryEvaluator([](u32 val) { return ~val; });
      break;
  }
}

void GekkoIRPlugin::EvalTerminalRel(Terminal type, AssemblerToken const& tok)
{
  switch (type)
  {
    case Terminal::kHex:
    case Terminal::kDec:
    case Terminal::kOct:
    case Terminal::kBin:
    case Terminal::kGPR:
    case Terminal::kFPR:
    case Terminal::kSPR:
    case Terminal::kCRField:
    case Terminal::kLt:
    case Terminal::kGt:
    case Terminal::kEq:
    case Terminal::kSo:
    {
      std::optional<u32> val = tok.EvalToken<u32>();
      ASSERT(val.has_value());
      AddLiteral(*val);
      break;
    }

    case Terminal::kDot:
      AddLiteral(CurrentAddress());
      break;

    case Terminal::kId:
    {
      if (auto label_it = labels.find(tok.token_val); label_it != labels.end())
      {
        AddLiteral(label_it->second);
      }
      else if (auto var_it = constants.find(tok.token_val); var_it != constants.end())
      {
        AddLiteral(var_it->second);
      }
      else
      {
        AddSymbolResolve(tok.token_val, false);
      }
      break;
    }

    // Parser should disallow this from happening
    default:
      ASSERT(false);
      break;
  }
}

void GekkoIRPlugin::EvalTerminalAbs(Terminal type, AssemblerToken const& tok)
{
  std::visit([this, &type, &tok](auto&& vec) { EvalTerminalAbsGeneric(type, tok, vec); }, eval_stack);
}

template <TokenConvertable T>
void GekkoIRPlugin::PushCast(T val)
{
  std::visit(
      [val](auto&& vec)
      {
        using val_type = typename std::decay_t<decltype(vec)>::value_type;
        vec.push_back(static_cast<val_type>(val));
      }, eval_stack);
}

template <TokenConvertable T>
void GekkoIRPlugin::EvalTerminalAbsGeneric(Terminal type, AssemblerToken const& tok,
                                              std::vector<T>& out_stack)
{
  switch (type)
  {
    case Terminal::kHex:
    case Terminal::kDec:
    case Terminal::kOct:
    case Terminal::kBin:
    case Terminal::kFlt:
    case Terminal::kGPR:
    case Terminal::kFPR:
    case Terminal::kSPR:
    case Terminal::kCRField:
    case Terminal::kLt:
    case Terminal::kGt:
    case Terminal::kEq:
    case Terminal::kSo:
    {
      std::optional<T> val = tok.EvalToken<T>();
      ASSERT(val.has_value());
      out_stack.push_back(*val);
      break;
    }

    case Terminal::kDot:
      out_stack.push_back(static_cast<T>(CurrentAddress()));
      break;

    case Terminal::kId:
    {
      if (auto label_it = labels.find(tok.token_val); label_it != labels.end())
      {
        out_stack.push_back(label_it->second);
      }
      else if (auto var_it = constants.find(tok.token_val); var_it != constants.end())
      {
        out_stack.push_back(var_it->second);
      }
      else
      {
        owner->EmitErrorHere(
            fmt::format("Undefined reference to Label/Constant '{}'", tok.ValStr()));
        return;
      }
      break;
    }

    // Parser should disallow this from happening
    default:
      ASSERT(false);
      break;
  }
}
}  // namespace

u32 IRBlock::BlockEndAddress() const
{
  return std::accumulate(
    chunks.begin(), chunks.end(), block_address,
    [](u32 acc, ChunkVariant const& chunk)
    {
      u32 size;
      if (std::holds_alternative<InstChunk>(chunk))
      {
        size = std::get<InstChunk>(chunk).size() * 4;
      }
      else if (std::holds_alternative<ByteChunk>(chunk))
      {
        size = std::get<ByteChunk>(chunk).size();
      }
      else if (std::holds_alternative<PadChunk>(chunk))
      {
        size = std::get<PadChunk>(chunk);
      }
      else
      {
        ASSERT(false);
        size = 0;
      }

      return acc + size;
     });
}

FailureOr<GekkoIR>
ParseToIR(std::string_view assembly, u32 base_virtual_address)
{
  GekkoIR ret;
  GekkoIRPlugin plugin(ret, base_virtual_address);

  ParseWithPlugin(&plugin, assembly);

  if (plugin.Error())
  {
    return FailureOr<GekkoIR>(std::move(*plugin.Error()));
  }

  return std::move(ret);
}

}  // Common::GekkoAssembler::detail
