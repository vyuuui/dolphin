#pragma once

#include <string_view>
#include <vector>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/Assembler/GekkoLexer.h"
#include "Common/CommonTypes.h"

namespace Common::GekkoAssembler::detail
{
struct GekkoInstruction
{
  // Combination of a mnemonic index and variant:
  // (<GekkoMnemonic> << 2) | (<variant bits>)
  size_t mnemonic_index = 0;
  // Below refers to GekkoParseResult::operand_pool
  size_t op_index = 0, op_count = 0;
  // Literal text of this instruction
  std::string_view raw_text;
  size_t line_number = 0;
  bool is_extended = false;
};

using InstChunk = std::vector<GekkoInstruction>;
using ByteChunk = std::vector<u8>;
using PadChunk = size_t;
using ChunkVariant = std::variant<InstChunk, ByteChunk, PadChunk>;

struct IRBlock
{
  IRBlock(u32 address) : block_address(address) {}

  std::vector<ChunkVariant> chunks;
  u32 block_address;

  u32 BlockEndAddress() const;
};

struct GekkoIR
{
  std::vector<IRBlock> blocks;
  std::vector<Tagged<Interval, u32>> operand_pool;
};

FailureOr<GekkoIR> ParseToIR(std::string_view assembly, u32 base_virtual_address);
}  // namespace Common::GekkoAssembler::detail
