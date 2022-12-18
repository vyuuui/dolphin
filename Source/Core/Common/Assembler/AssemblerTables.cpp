#include "Common/Assembler/AssemblerTables.h"

#include <map>
#include <string>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/Assembler/GekkoParser.h"
#include "Common/CommonTypes.h"

namespace Common::GekkoAssembler::detail
{
namespace
{
// Compile-time helpers for mnemonic generation
// Generate inclusive mask [left, right] -- MSB=0 LSB=31
constexpr u32 mask(u32 left, u32 right)
{
  return static_cast<u32>(((u64{1} << (32 - left)) - 1) & ~((u64{1} << (31 - right)) - 1));
}
constexpr u32 insert_val(u32 val, u32 left, u32 right)
{
  return val << (31 - right) & mask(left, right);
}
constexpr u32 insert_opcode(u32 opcode)
{
  return insert_val(opcode, 0, 5);
}
constexpr u32 spr_bitswap(u32 spr)
{
  return ((spr & 0b0000011111) << 5) | ((spr & 0b1111100000) >> 5);
}

constexpr MnemonicDesc kInvalidMnemonic = {0, 0, {}};
constexpr ExtendedMnemonicDesc kInvalidExtMnemonic = {0, nullptr};

// All operands as referenced by the Gekko/Broadway user manual
// See section 12.1.2 under Chapter 12
constexpr OperandDesc _A = OperandDesc {mask(11, 15), {16, false}};
constexpr OperandDesc _B = OperandDesc {mask(16, 20), {11, false}};
constexpr OperandDesc _BD = OperandDesc {mask(16, 29), {0, true}};
constexpr OperandDesc _BI = OperandDesc {mask(11, 15), {16, false}};
constexpr OperandDesc _BO = OperandDesc {mask(6, 10), {21, false}};
constexpr OperandDesc _C = OperandDesc {mask(21, 25), {6, false}};
constexpr OperandDesc _Crba = OperandDesc {mask(11, 15), {16, false}};
constexpr OperandDesc _Crbb = OperandDesc {mask(16, 20), {11, false}};
constexpr OperandDesc _Crbd = OperandDesc {mask(6, 10), {21, false}};
constexpr OperandDesc _Crfd = OperandDesc {mask(6, 8), {23, false}};
constexpr OperandDesc _Crfs = OperandDesc {mask(11, 13), {18, false}};
constexpr OperandDesc _CRM = OperandDesc {mask(12, 19), {12, false}};
constexpr OperandDesc _D = OperandDesc {mask(6, 10), {21, false}};
constexpr OperandDesc _FM = OperandDesc {mask(7, 14), {17, false}};
constexpr OperandDesc _I1 = OperandDesc {mask(16, 16), {15, false}};
constexpr OperandDesc _I2 = OperandDesc {mask(21, 21), {10, false}};
constexpr OperandDesc _IMM = OperandDesc {mask(16, 19), {12, false}};
constexpr OperandDesc _L = OperandDesc {mask(10, 10), {21, false}};
constexpr OperandDesc _LI = OperandDesc {mask(6, 29), {0, true}};
constexpr OperandDesc _MB = OperandDesc {mask(21, 25), {6, false}};
constexpr OperandDesc _ME = OperandDesc {mask(26, 30), {1, false}};
constexpr OperandDesc _NB = OperandDesc {mask(16, 20), {11, false}};
constexpr OperandDesc _Offd = OperandDesc {mask(16, 31), {0, true}};
constexpr OperandDesc _OffdPs = OperandDesc {mask(19, 31), {0, true}};
constexpr OperandDesc _S = OperandDesc {mask(6, 10), {21, false}};
constexpr OperandDesc _SH = OperandDesc {mask(16, 20), {11, false}};
constexpr OperandDesc _SIMM = OperandDesc {mask(16, 31), {0, true}};
constexpr OperandDesc _SPR = OperandDesc {mask(11, 20), {11, false}};
constexpr OperandDesc _SR = OperandDesc {mask(12, 15), {16, false}};
constexpr OperandDesc _TO = OperandDesc {mask(6, 10), {21, false}};
constexpr OperandDesc _TPR = OperandDesc {mask(11, 20), {11, false}};
constexpr OperandDesc _UIMM = OperandDesc {mask(16, 31), {0, false}};
constexpr OperandDesc _W1 = OperandDesc {mask(17, 19), {12, false}};
constexpr OperandDesc _W2 = OperandDesc {mask(22, 24), {7, false}};
}  // namespace

void OperandList::Insert(size_t before, u32 val)
{
  overfill = count == kMaxOperands;
  for (size_t i = before + 1; i <= count && i < kMaxOperands; i++)
  {
    auto tmp = list[i];
    list[i] = list[before];
    list[before] = tmp;
  }

  list[before] = Tagged<Interval, u32>({0, 0}, val);
  if (!overfill)
  {
    count++;
  }
}

// OperandDesc holds the shift position for an operand, as well as the mask
// Whether the user provided a valid input for an operand can be determined by the mask
u32 OperandDesc::MaxVal() const
{
  const u32 mask_sh = mask >> shift;
  if (is_signed)
  {
    u32 mask_hibit = (mask_sh & (mask_sh ^ (mask_sh >> 1)));
    return mask_hibit - 1;
  }
  return mask_sh;
}

u32 OperandDesc::MinVal() const
{
  if (is_signed)
  {
    return ~MaxVal();
  }
  return 0;
}

u32 OperandDesc::TruncBits() const
{
  const u32 mask_sh = mask >> shift;
  const u32 mask_lobit = mask_sh & (mask_sh ^ (mask_sh << 1));
  return mask_lobit - 1;
}

bool OperandDesc::Fits(u32 val) const
{
  const u32 mask_sh = mask >> shift;
  if (is_signed)
  {
    // Get high bit and low bit from a range mask
    const u32 mask_hibit = mask_sh & (mask_sh ^ (mask_sh >> 1));
    const u32 mask_lobit = mask_sh & (mask_sh ^ (mask_sh << 1));
    // Positive max is (signbit - 1)
    // Negative min is ~(Positive Max)
    const u32 positive_max = mask_hibit - 1;
    const u32 negative_max = ~positive_max;
    // Truncated bits are any bits right of the mask that are 0 after shifting
    const u32 truncate_bits = mask_lobit - 1;
    return (val <= positive_max || val >= negative_max) &&
           !(val & truncate_bits);
  }
  return (mask_sh & val) == val;
}

u32 OperandDesc::Fit(u32 val) const
{
  return (val << shift) & mask;
}

///////////////////
// PARSER TABLES //
///////////////////

std::map<std::string, u32, std::less<>> sprg_map =
{
  {"xer", 1}, {"lr", 8}, {"ctr", 9}, {"dsisr", 18},
  {"dar", 19}, {"dec", 22}, {"sdr1", 25}, {"srr0", 26},
  {"srr1", 27}, {"sprg0", 272}, {"sprg1", 273}, {"sprg2", 274},
  {"sprg3", 275}, {"ear", 282}, {"tbl", 284}, {"tbu", 285},
  {"ibat0u", 528}, {"ibat0l", 529}, {"ibat1u", 530}, {"ibat1l", 531},
  {"ibat2u", 532}, {"ibat2l", 533}, {"ibat3u", 534}, {"ibat3l", 535},
  {"dbat0u", 536}, {"dbat0l", 537}, {"dbat1u", 538}, {"dbat1l", 539},
  {"dbat2u", 540}, {"dbat2l", 541}, {"dbat3u", 542}, {"dbat3l", 543},
  {"gqr0", 912}, {"gqr1", 913}, {"gqr2", 914}, {"gqr3", 915},
  {"gqr4", 916}, {"gqr5", 917}, {"gqr6", 918}, {"gqr7", 919},
  {"hid2", 920}, {"wpar", 921}, {"dma_u", 922}, {"dma_l", 923},
  {"ummcr0", 936}, {"upmc1", 937}, {"upmc2", 938}, {"usia", 939},
  {"ummcr1", 940}, {"upmc3", 941}, {"upmc4", 942}, {"usda", 943},
  {"mmcr0", 952}, {"pmc1", 953}, {"pmc2", 954}, {"sia", 955},
  {"mmcr1", 956}, {"pmc3", 957}, {"pmc4", 958}, {"sda", 959},
  {"hid0", 1008}, {"hid1", 1009}, {"iabr", 1010}, {"dabr", 1013},
  {"l2cr", 1017}, {"ictc", 1019}, {"thrm1", 1020}, {"thrm2", 1021},
  {"thrm3", 1022}
};

std::map<std::string, GekkoDirective, std::less<>> directives_map =
{
  {"byte", GekkoDirective::kByte},
  {"2byte", GekkoDirective::k2byte},
  {"4byte", GekkoDirective::k4byte},
  {"8byte", GekkoDirective::k8byte},
  {"float", GekkoDirective::kFloat},
  {"double", GekkoDirective::kDouble},
  {"locate", GekkoDirective::kLocate},
  {"padalign", GekkoDirective::kPadAlign},
  {"align", GekkoDirective::kAlign},
  {"zeros", GekkoDirective::kZeros},
  {"skip", GekkoDirective::kSkip},
  {"defvar", GekkoDirective::kDefVar},
  {"ascii", GekkoDirective::kAscii},
  {"asciz", GekkoDirective::kAsciz},
};

#define MNEMONIC(mnemonic_str, mnemonic_enum, variant_bits, alg) \
  {mnemonic_str, {static_cast<size_t>(mnemonic_enum) * kVariantPermutations + (variant_bits), alg}}
#define PLAIN_MNEMONIC(mnemonic_str, mnemonic_enum, alg) \
  MNEMONIC(mnemonic_str, mnemonic_enum, kPlainMnemonic, alg)
#define RC_MNEMONIC(mnemonic_str, mnemonic_enum, alg) \
  MNEMONIC(mnemonic_str, mnemonic_enum, kPlainMnemonic, alg), \
  MNEMONIC(mnemonic_str ".", mnemonic_enum, kRecordBit, alg)
#define OERC_MNEMONIC(mnemonic_str, mnemonic_enum, alg) \
  MNEMONIC(mnemonic_str, mnemonic_enum, kPlainMnemonic, alg), \
  MNEMONIC(mnemonic_str ".", mnemonic_enum, kRecordBit, alg), \
  MNEMONIC(mnemonic_str "o", mnemonic_enum, kOverflowExceptionBit, alg), \
  MNEMONIC(mnemonic_str "o.", mnemonic_enum, (kRecordBit | kOverflowExceptionBit), alg)
#define LK_MNEMONIC(mnemonic_str, mnemonic_enum, alg) \
  MNEMONIC(mnemonic_str, mnemonic_enum, kPlainMnemonic, alg), \
  MNEMONIC(mnemonic_str "l", mnemonic_enum, kLinkBit, alg)
#define AALK_MNEMONIC(mnemonic_str, mnemonic_enum, alg) \
  MNEMONIC(mnemonic_str, mnemonic_enum, kPlainMnemonic, alg), \
  MNEMONIC(mnemonic_str "l", mnemonic_enum, kLinkBit, alg), \
  MNEMONIC(mnemonic_str "a", mnemonic_enum, kAbsoluteAddressBit, alg), \
  MNEMONIC(mnemonic_str "la", mnemonic_enum, (kLinkBit | kAbsoluteAddressBit), alg)

std::map<std::string, ParseInfo, std::less<>> mnemonic_tokens =
{
  OERC_MNEMONIC("add", GekkoMnemonic::kAdd, ParseAlg::Op3),
  OERC_MNEMONIC("addc", GekkoMnemonic::kAddc, ParseAlg::Op3),
  OERC_MNEMONIC("adde", GekkoMnemonic::kAdde, ParseAlg::Op3),
  PLAIN_MNEMONIC("addi", GekkoMnemonic::kAddi, ParseAlg::Op3),
  PLAIN_MNEMONIC("addic", GekkoMnemonic::kAddic, ParseAlg::Op3),
  PLAIN_MNEMONIC("addic.", GekkoMnemonic::kAddicDot, ParseAlg::Op3),
  PLAIN_MNEMONIC("addis", GekkoMnemonic::kAddis, ParseAlg::Op3),
  OERC_MNEMONIC("addme", GekkoMnemonic::kAddme, ParseAlg::Op2),
  OERC_MNEMONIC("addze", GekkoMnemonic::kAddze, ParseAlg::Op2),
  RC_MNEMONIC("and", GekkoMnemonic::kAnd, ParseAlg::Op3),
  RC_MNEMONIC("andc", GekkoMnemonic::kAndc, ParseAlg::Op3),
  PLAIN_MNEMONIC("andi.", GekkoMnemonic::kAndiDot, ParseAlg::Op3),
  PLAIN_MNEMONIC("andis.", GekkoMnemonic::kAndisDot, ParseAlg::Op3),
  AALK_MNEMONIC("b", GekkoMnemonic::kB, ParseAlg::Op1),
  AALK_MNEMONIC("bc", GekkoMnemonic::kBc, ParseAlg::Op3),
  LK_MNEMONIC("bcctr", GekkoMnemonic::kBcctr, ParseAlg::Op2),
  LK_MNEMONIC("bclr", GekkoMnemonic::kBclr, ParseAlg::Op2),
  PLAIN_MNEMONIC("cmp", GekkoMnemonic::kCmp, ParseAlg::Op4),
  PLAIN_MNEMONIC("cmpi", GekkoMnemonic::kCmpi, ParseAlg::Op4),
  PLAIN_MNEMONIC("cmpl", GekkoMnemonic::kCmpl, ParseAlg::Op4),
  PLAIN_MNEMONIC("cmpli", GekkoMnemonic::kCmpli, ParseAlg::Op4),
  RC_MNEMONIC("cntlzw", GekkoMnemonic::kCntlzw, ParseAlg::Op2),
  PLAIN_MNEMONIC("crand", GekkoMnemonic::kCrand, ParseAlg::Op3),
  PLAIN_MNEMONIC("crandc", GekkoMnemonic::kCrandc, ParseAlg::Op3),
  PLAIN_MNEMONIC("creqv", GekkoMnemonic::kCreqv, ParseAlg::Op3),
  PLAIN_MNEMONIC("crnand", GekkoMnemonic::kCrnand, ParseAlg::Op3),
  PLAIN_MNEMONIC("crnor", GekkoMnemonic::kCrnor, ParseAlg::Op3),
  PLAIN_MNEMONIC("cror", GekkoMnemonic::kCror, ParseAlg::Op3),
  PLAIN_MNEMONIC("crorc", GekkoMnemonic::kCrorc, ParseAlg::Op3),
  PLAIN_MNEMONIC("crxor", GekkoMnemonic::kCrxor, ParseAlg::Op3),
  PLAIN_MNEMONIC("dcbf", GekkoMnemonic::kDcbf, ParseAlg::Op2),
  PLAIN_MNEMONIC("dcbi", GekkoMnemonic::kDcbi, ParseAlg::Op2),
  PLAIN_MNEMONIC("dcbst", GekkoMnemonic::kDcbst, ParseAlg::Op2),
  PLAIN_MNEMONIC("dcbt", GekkoMnemonic::kDcbt, ParseAlg::Op2),
  PLAIN_MNEMONIC("dcbtst", GekkoMnemonic::kDcbtst, ParseAlg::Op2),
  PLAIN_MNEMONIC("dcbz", GekkoMnemonic::kDcbz, ParseAlg::Op2),
  PLAIN_MNEMONIC("dcbz_l", GekkoMnemonic::kDcbz_l, ParseAlg::Op2),
  OERC_MNEMONIC("divw", GekkoMnemonic::kDivw, ParseAlg::Op3),
  OERC_MNEMONIC("divwu", GekkoMnemonic::kDivwu, ParseAlg::Op3),
  PLAIN_MNEMONIC("eciwx", GekkoMnemonic::kEciwx, ParseAlg::Op3),
  PLAIN_MNEMONIC("ecowx", GekkoMnemonic::kEcowx, ParseAlg::Op3),
  PLAIN_MNEMONIC("eieio", GekkoMnemonic::kEieio, ParseAlg::None),
  RC_MNEMONIC("eqv", GekkoMnemonic::kEqv, ParseAlg::Op3),
  RC_MNEMONIC("extsb", GekkoMnemonic::kExtsb, ParseAlg::Op2),
  RC_MNEMONIC("extsh", GekkoMnemonic::kExtsh, ParseAlg::Op2),
  RC_MNEMONIC("fabs", GekkoMnemonic::kFabs, ParseAlg::Op2),
  RC_MNEMONIC("fadd", GekkoMnemonic::kFadd, ParseAlg::Op3),
  RC_MNEMONIC("fadds", GekkoMnemonic::kFadds, ParseAlg::Op3),
  PLAIN_MNEMONIC("fcmpo", GekkoMnemonic::kFcmpo, ParseAlg::Op3),
  PLAIN_MNEMONIC("fcmpu", GekkoMnemonic::kFcmpu, ParseAlg::Op3),
  RC_MNEMONIC("fctiw", GekkoMnemonic::kFctiw, ParseAlg::Op2),
  RC_MNEMONIC("fctiwz", GekkoMnemonic::kFctiwz, ParseAlg::Op2),
  RC_MNEMONIC("fdiv", GekkoMnemonic::kFdiv, ParseAlg::Op3),
  RC_MNEMONIC("fdivs", GekkoMnemonic::kFdivs, ParseAlg::Op3),
  RC_MNEMONIC("fmadd", GekkoMnemonic::kFmadd, ParseAlg::Op4),
  RC_MNEMONIC("fmadds", GekkoMnemonic::kFmadds, ParseAlg::Op4),
  RC_MNEMONIC("fmr", GekkoMnemonic::kFmr, ParseAlg::Op2),
  RC_MNEMONIC("fmsub", GekkoMnemonic::kFmsub, ParseAlg::Op4),
  RC_MNEMONIC("fmsubs", GekkoMnemonic::kFmsubs, ParseAlg::Op4),
  RC_MNEMONIC("fmul", GekkoMnemonic::kFmul, ParseAlg::Op3),
  RC_MNEMONIC("fmuls", GekkoMnemonic::kFmuls, ParseAlg::Op3),
  RC_MNEMONIC("fnabs", GekkoMnemonic::kFnabs, ParseAlg::Op2),
  RC_MNEMONIC("fneg", GekkoMnemonic::kFneg, ParseAlg::Op2),
  RC_MNEMONIC("fnmadd", GekkoMnemonic::kFnmadd, ParseAlg::Op4),
  RC_MNEMONIC("fnmadds", GekkoMnemonic::kFnmadds, ParseAlg::Op4),
  RC_MNEMONIC("fnmsub", GekkoMnemonic::kFnmsub, ParseAlg::Op4),
  RC_MNEMONIC("fnmsubs", GekkoMnemonic::kFnmsubs, ParseAlg::Op4),
  RC_MNEMONIC("fres", GekkoMnemonic::kFres, ParseAlg::Op2),
  RC_MNEMONIC("frsp", GekkoMnemonic::kFrsp, ParseAlg::Op2),
  RC_MNEMONIC("frsqrte", GekkoMnemonic::kFrsqrte, ParseAlg::Op2),
  RC_MNEMONIC("fsel", GekkoMnemonic::kFsel, ParseAlg::Op4),
  RC_MNEMONIC("fsub", GekkoMnemonic::kFsub, ParseAlg::Op3),
  RC_MNEMONIC("fsubs", GekkoMnemonic::kFsubs, ParseAlg::Op3),
  PLAIN_MNEMONIC("icbi", GekkoMnemonic::kIcbi, ParseAlg::Op2),
  PLAIN_MNEMONIC("isync", GekkoMnemonic::kIsync, ParseAlg::None),
  PLAIN_MNEMONIC("lbz", GekkoMnemonic::kLbz, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("lbzu", GekkoMnemonic::kLbzu, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("lbzux", GekkoMnemonic::kLbzux, ParseAlg::Op3),
  PLAIN_MNEMONIC("lbzx", GekkoMnemonic::kLbzx, ParseAlg::Op3),
  PLAIN_MNEMONIC("lfd", GekkoMnemonic::kLfd, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("lfdu", GekkoMnemonic::kLfdu, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("lfdux", GekkoMnemonic::kLfdux, ParseAlg::Op3),
  PLAIN_MNEMONIC("lfdx", GekkoMnemonic::kLfdx, ParseAlg::Op3),
  PLAIN_MNEMONIC("lfs", GekkoMnemonic::kLfs, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("lfsu", GekkoMnemonic::kLfsu, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("lfsux", GekkoMnemonic::kLfsux, ParseAlg::Op3),
  PLAIN_MNEMONIC("lfsx", GekkoMnemonic::kLfsx, ParseAlg::Op3),
  PLAIN_MNEMONIC("lha", GekkoMnemonic::kLha, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("lhau", GekkoMnemonic::kLhau, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("lhaux", GekkoMnemonic::kLhaux, ParseAlg::Op3),
  PLAIN_MNEMONIC("lhax", GekkoMnemonic::kLhax, ParseAlg::Op3),
  PLAIN_MNEMONIC("lhbrx", GekkoMnemonic::kLhbrx, ParseAlg::Op3),
  PLAIN_MNEMONIC("lhz", GekkoMnemonic::kLhz, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("lhzu", GekkoMnemonic::kLhzu, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("lhzux", GekkoMnemonic::kLhzux, ParseAlg::Op3),
  PLAIN_MNEMONIC("lhzx", GekkoMnemonic::kLhzx, ParseAlg::Op3),
  PLAIN_MNEMONIC("lmw", GekkoMnemonic::kLmw, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("lswi", GekkoMnemonic::kLswi, ParseAlg::Op3),
  PLAIN_MNEMONIC("lswx", GekkoMnemonic::kLswx, ParseAlg::Op3),
  PLAIN_MNEMONIC("lwarx", GekkoMnemonic::kLwarx, ParseAlg::Op3),
  PLAIN_MNEMONIC("lwbrx", GekkoMnemonic::kLwbrx, ParseAlg::Op3),
  PLAIN_MNEMONIC("lwz", GekkoMnemonic::kLwz, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("lwzu", GekkoMnemonic::kLwzu, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("lwzux", GekkoMnemonic::kLwzux, ParseAlg::Op3),
  PLAIN_MNEMONIC("lwzx", GekkoMnemonic::kLwzx, ParseAlg::Op3),
  PLAIN_MNEMONIC("mcrf", GekkoMnemonic::kMcrf, ParseAlg::Op2),
  PLAIN_MNEMONIC("mcrfs", GekkoMnemonic::kMcrfs, ParseAlg::Op2),
  PLAIN_MNEMONIC("mcrxr", GekkoMnemonic::kMcrxr, ParseAlg::Op1),
  PLAIN_MNEMONIC("mfcr", GekkoMnemonic::kMfcr, ParseAlg::Op1),
  RC_MNEMONIC("mffs", GekkoMnemonic::kMffs, ParseAlg::Op1),
  PLAIN_MNEMONIC("mfmsr", GekkoMnemonic::kMfmsr, ParseAlg::Op1),
  PLAIN_MNEMONIC("mfspr_nobitswap", GekkoMnemonic::kMfspr_nobitswap, ParseAlg::Op2),
  PLAIN_MNEMONIC("mfsr", GekkoMnemonic::kMfsr, ParseAlg::Op2),
  PLAIN_MNEMONIC("mfsrin", GekkoMnemonic::kMfsrin, ParseAlg::Op2),
  PLAIN_MNEMONIC("mftb_nobitswap", GekkoMnemonic::kMftb_nobitswap, ParseAlg::Op2),
  PLAIN_MNEMONIC("mtcrf", GekkoMnemonic::kMtcrf, ParseAlg::Op2),
  RC_MNEMONIC("mtfsb0", GekkoMnemonic::kMtfsb0, ParseAlg::Op1),
  RC_MNEMONIC("mtfsb1", GekkoMnemonic::kMtfsb1, ParseAlg::Op1),
  RC_MNEMONIC("mtfsf", GekkoMnemonic::kMtfsf, ParseAlg::Op2),
  RC_MNEMONIC("mtfsfi", GekkoMnemonic::kMtfsfi, ParseAlg::Op2),
  PLAIN_MNEMONIC("mtmsr", GekkoMnemonic::kMtmsr, ParseAlg::Op1),
  PLAIN_MNEMONIC("mtspr_nobitswap", GekkoMnemonic::kMtspr_nobitswap, ParseAlg::Op2),
  PLAIN_MNEMONIC("mtsr", GekkoMnemonic::kMtsr, ParseAlg::Op2),
  PLAIN_MNEMONIC("mtsrin", GekkoMnemonic::kMtsrin, ParseAlg::Op2),
  RC_MNEMONIC("mulhw", GekkoMnemonic::kMulhw, ParseAlg::Op3),
  RC_MNEMONIC("mulhwu", GekkoMnemonic::kMulhwu, ParseAlg::Op3),
  PLAIN_MNEMONIC("mulli", GekkoMnemonic::kMulli, ParseAlg::Op3),
  OERC_MNEMONIC("mullw", GekkoMnemonic::kMullw, ParseAlg::Op3),
  RC_MNEMONIC("nand", GekkoMnemonic::kNand, ParseAlg::Op3),
  OERC_MNEMONIC("neg", GekkoMnemonic::kNeg, ParseAlg::Op2),
  RC_MNEMONIC("nor", GekkoMnemonic::kNor, ParseAlg::Op3),
  RC_MNEMONIC("or", GekkoMnemonic::kOr, ParseAlg::Op3),
  RC_MNEMONIC("orc", GekkoMnemonic::kOrc, ParseAlg::Op3),
  PLAIN_MNEMONIC("ori", GekkoMnemonic::kOri, ParseAlg::Op3),
  PLAIN_MNEMONIC("oris", GekkoMnemonic::kOris, ParseAlg::Op3),
  PLAIN_MNEMONIC("psq_l", GekkoMnemonic::kPsq_l, ParseAlg::Op1Off1Op2),
  PLAIN_MNEMONIC("psq_lu", GekkoMnemonic::kPsq_lu, ParseAlg::Op1Off1Op2),
  PLAIN_MNEMONIC("psq_lux", GekkoMnemonic::kPsq_lux, ParseAlg::Op5),
  PLAIN_MNEMONIC("psq_lx", GekkoMnemonic::kPsq_lx, ParseAlg::Op5),
  PLAIN_MNEMONIC("psq_st", GekkoMnemonic::kPsq_st, ParseAlg::Op1Off1Op2),
  PLAIN_MNEMONIC("psq_stu", GekkoMnemonic::kPsq_stu, ParseAlg::Op1Off1Op2),
  PLAIN_MNEMONIC("psq_stux", GekkoMnemonic::kPsq_stux, ParseAlg::Op5),
  PLAIN_MNEMONIC("psq_stx", GekkoMnemonic::kPsq_stx, ParseAlg::Op5),
  PLAIN_MNEMONIC("ps_abs", GekkoMnemonic::kPs_abs, ParseAlg::Op2),
  RC_MNEMONIC("ps_add", GekkoMnemonic::kPs_add, ParseAlg::Op3),
  PLAIN_MNEMONIC("ps_cmpo0", GekkoMnemonic::kPs_cmpo0, ParseAlg::Op3),
  PLAIN_MNEMONIC("ps_cmpo1", GekkoMnemonic::kPs_cmpo1, ParseAlg::Op3),
  PLAIN_MNEMONIC("ps_cmpu0", GekkoMnemonic::kPs_cmpu0, ParseAlg::Op3),
  PLAIN_MNEMONIC("ps_cmpu1", GekkoMnemonic::kPs_cmpu1, ParseAlg::Op3),
  RC_MNEMONIC("ps_div", GekkoMnemonic::kPs_div, ParseAlg::Op3),
  RC_MNEMONIC("ps_madd", GekkoMnemonic::kPs_madd, ParseAlg::Op4),
  RC_MNEMONIC("ps_madds0", GekkoMnemonic::kPs_madds0, ParseAlg::Op4),
  RC_MNEMONIC("ps_madds1", GekkoMnemonic::kPs_madds1, ParseAlg::Op4),
  RC_MNEMONIC("ps_merge00", GekkoMnemonic::kPs_merge00, ParseAlg::Op3),
  RC_MNEMONIC("ps_merge01", GekkoMnemonic::kPs_merge01, ParseAlg::Op3),
  RC_MNEMONIC("ps_merge10", GekkoMnemonic::kPs_merge10, ParseAlg::Op3),
  RC_MNEMONIC("ps_merge11", GekkoMnemonic::kPs_merge11, ParseAlg::Op3),
  RC_MNEMONIC("ps_mr", GekkoMnemonic::kPs_mr, ParseAlg::Op2),
  RC_MNEMONIC("ps_msub", GekkoMnemonic::kPs_msub, ParseAlg::Op4),
  RC_MNEMONIC("ps_mul", GekkoMnemonic::kPs_mul, ParseAlg::Op3),
  RC_MNEMONIC("ps_muls0", GekkoMnemonic::kPs_muls0, ParseAlg::Op3),
  RC_MNEMONIC("ps_muls1", GekkoMnemonic::kPs_muls1, ParseAlg::Op3),
  RC_MNEMONIC("ps_nabs", GekkoMnemonic::kPs_nabs, ParseAlg::Op2),
  RC_MNEMONIC("ps_neg", GekkoMnemonic::kPs_neg, ParseAlg::Op2),
  RC_MNEMONIC("ps_nmadd", GekkoMnemonic::kPs_nmadd, ParseAlg::Op4),
  RC_MNEMONIC("ps_nmsub", GekkoMnemonic::kPs_nmsub, ParseAlg::Op4),
  RC_MNEMONIC("ps_res", GekkoMnemonic::kPs_res, ParseAlg::Op2),
  RC_MNEMONIC("ps_rsqrte", GekkoMnemonic::kPs_rsqrte, ParseAlg::Op2),
  RC_MNEMONIC("ps_sel", GekkoMnemonic::kPs_sel, ParseAlg::Op4),
  RC_MNEMONIC("ps_sub", GekkoMnemonic::kPs_sub, ParseAlg::Op3),
  RC_MNEMONIC("ps_sum0", GekkoMnemonic::kPs_sum0, ParseAlg::Op4),
  RC_MNEMONIC("ps_sum1", GekkoMnemonic::kPs_sum1, ParseAlg::Op4),
  PLAIN_MNEMONIC("rfi", GekkoMnemonic::kRfi, ParseAlg::None),
  RC_MNEMONIC("rlwimi", GekkoMnemonic::kRlwimi, ParseAlg::Op5),
  RC_MNEMONIC("rlwinm", GekkoMnemonic::kRlwinm, ParseAlg::Op5),
  RC_MNEMONIC("rlwnm", GekkoMnemonic::kRlwnm, ParseAlg::Op5),
  PLAIN_MNEMONIC("sc", GekkoMnemonic::kSc, ParseAlg::None),
  RC_MNEMONIC("slw", GekkoMnemonic::kSlw, ParseAlg::Op3),
  RC_MNEMONIC("sraw", GekkoMnemonic::kSraw, ParseAlg::Op3),
  RC_MNEMONIC("srawi", GekkoMnemonic::kSrawi, ParseAlg::Op3),
  RC_MNEMONIC("srw", GekkoMnemonic::kSrw, ParseAlg::Op3),
  PLAIN_MNEMONIC("stb", GekkoMnemonic::kStb, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("stbu", GekkoMnemonic::kStbu, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("stbux", GekkoMnemonic::kStbux, ParseAlg::Op3),
  PLAIN_MNEMONIC("stbx", GekkoMnemonic::kStbx, ParseAlg::Op3),
  PLAIN_MNEMONIC("stfd", GekkoMnemonic::kStfd, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("stfdu", GekkoMnemonic::kStfdu, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("stfdux", GekkoMnemonic::kStfdux, ParseAlg::Op3),
  PLAIN_MNEMONIC("stfdx", GekkoMnemonic::kStfdx, ParseAlg::Op3),
  PLAIN_MNEMONIC("stfiwx", GekkoMnemonic::kStfiwx, ParseAlg::Op3),
  PLAIN_MNEMONIC("stfs", GekkoMnemonic::kStfs, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("stfsu", GekkoMnemonic::kStfsu, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("stfsux", GekkoMnemonic::kStfsux, ParseAlg::Op3),
  PLAIN_MNEMONIC("stfsx", GekkoMnemonic::kStfsx, ParseAlg::Op3),
  PLAIN_MNEMONIC("sth", GekkoMnemonic::kSth, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("sthbrx", GekkoMnemonic::kSthbrx, ParseAlg::Op3),
  PLAIN_MNEMONIC("sthu", GekkoMnemonic::kSthu, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("sthux", GekkoMnemonic::kSthux, ParseAlg::Op3),
  PLAIN_MNEMONIC("sthx", GekkoMnemonic::kSthx, ParseAlg::Op3),
  PLAIN_MNEMONIC("stmw", GekkoMnemonic::kStmw, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("stswi", GekkoMnemonic::kStswi, ParseAlg::Op3),
  PLAIN_MNEMONIC("stswx", GekkoMnemonic::kStswx, ParseAlg::Op3),
  PLAIN_MNEMONIC("stw", GekkoMnemonic::kStw, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("stwbrx", GekkoMnemonic::kStwbrx, ParseAlg::Op3),
  PLAIN_MNEMONIC("stwcx.", GekkoMnemonic::kStwcxDot, ParseAlg::Op3),
  PLAIN_MNEMONIC("stwu", GekkoMnemonic::kStwu, ParseAlg::Op1Off1),
  PLAIN_MNEMONIC("stwux", GekkoMnemonic::kStwux, ParseAlg::Op3),
  PLAIN_MNEMONIC("stwx", GekkoMnemonic::kStwx, ParseAlg::Op3),
  OERC_MNEMONIC("subf", GekkoMnemonic::kSubf, ParseAlg::Op3),
  OERC_MNEMONIC("subfc", GekkoMnemonic::kSubfc, ParseAlg::Op3),
  OERC_MNEMONIC("subfe", GekkoMnemonic::kSubfe, ParseAlg::Op3),
  PLAIN_MNEMONIC("subfic", GekkoMnemonic::kSubfic, ParseAlg::Op3),
  OERC_MNEMONIC("subfme", GekkoMnemonic::kSubfme, ParseAlg::Op2),
  OERC_MNEMONIC("subfze", GekkoMnemonic::kSubfze, ParseAlg::Op2),
  PLAIN_MNEMONIC("sync", GekkoMnemonic::kSync, ParseAlg::None),
  PLAIN_MNEMONIC("tlbie", GekkoMnemonic::kTlbie, ParseAlg::Op1),
  PLAIN_MNEMONIC("tlbsync", GekkoMnemonic::kTlbsync, ParseAlg::None),
  PLAIN_MNEMONIC("tw", GekkoMnemonic::kTw, ParseAlg::Op3),
  PLAIN_MNEMONIC("twi", GekkoMnemonic::kTwi, ParseAlg::Op3),
  RC_MNEMONIC("xor", GekkoMnemonic::kXor, ParseAlg::Op3),
  PLAIN_MNEMONIC("xori", GekkoMnemonic::kXori, ParseAlg::Op3),
  PLAIN_MNEMONIC("xoris", GekkoMnemonic::kXoris, ParseAlg::Op3),
};

#define PSEUDO(mnemonic, base, variant_bits, alg) \
  {mnemonic, {static_cast<size_t>(base) * kVariantPermutations + (variant_bits), alg}}
#define PLAIN_PSEUDO(mnemonic, base, alg) \
  PSEUDO(mnemonic, base, kPlainMnemonic, alg)
#define RC_PSEUDO(mnemonic, base, alg) \
  PSEUDO(mnemonic, base, kPlainMnemonic, alg), \
  PSEUDO(mnemonic ".", base, kRecordBit, alg)
#define OERC_PSEUDO(mnemonic, base, alg) \
  PSEUDO(mnemonic, base, kPlainMnemonic, alg), \
  PSEUDO(mnemonic ".", base, kRecordBit, alg), \
  PSEUDO(mnemonic "o", base, kOverflowExceptionBit, alg), \
  PSEUDO(mnemonic "o.", base, (kRecordBit | kOverflowExceptionBit), alg)
#define LK_PSEUDO(mnemonic, base, alg) \
  PSEUDO(mnemonic, base, kPlainMnemonic, alg), \
  PSEUDO(mnemonic "l", base, kLinkBit, alg)
#define LKAA_PSEUDO(mnemonic, base, alg) \
  PSEUDO(mnemonic, base, kPlainMnemonic, alg), \
  PSEUDO(mnemonic "l", base, kLinkBit, alg), \
  PSEUDO(mnemonic "a", base, kAbsoluteAddressBit, alg), \
  PSEUDO(mnemonic "la", base, (kLinkBit | kAbsoluteAddressBit), alg)

std::map<std::string, ParseInfo, std::less<>> extended_mnemonic_tokens =
{
  PLAIN_PSEUDO("subi", ExtendedGekkoMnemonic::kSubi, ParseAlg::Op3),
  PLAIN_PSEUDO("subis", ExtendedGekkoMnemonic::kSubis, ParseAlg::Op3),
  PLAIN_PSEUDO("subic", ExtendedGekkoMnemonic::kSubic, ParseAlg::Op3),
  PLAIN_PSEUDO("subic.", ExtendedGekkoMnemonic::kSubicDot, ParseAlg::Op3),
  OERC_PSEUDO("sub", ExtendedGekkoMnemonic::kSub, ParseAlg::Op3),
  OERC_PSEUDO("subc", ExtendedGekkoMnemonic::kSubc, ParseAlg::Op3),
  PLAIN_PSEUDO("cmpwi", ExtendedGekkoMnemonic::kCmpwi, ParseAlg::Op2Or3),
  PLAIN_PSEUDO("cmpw", ExtendedGekkoMnemonic::kCmpw, ParseAlg::Op2Or3),
  PLAIN_PSEUDO("cmplwi", ExtendedGekkoMnemonic::kCmplwi, ParseAlg::Op2Or3),
  PLAIN_PSEUDO("cmplw", ExtendedGekkoMnemonic::kCmplw, ParseAlg::Op2Or3),
  RC_PSEUDO("extlwi", ExtendedGekkoMnemonic::kExtlwi, ParseAlg::Op4),
  RC_PSEUDO("extrwi", ExtendedGekkoMnemonic::kExtrwi, ParseAlg::Op4),
  RC_PSEUDO("inslwi", ExtendedGekkoMnemonic::kInslwi, ParseAlg::Op4),
  RC_PSEUDO("insrwi", ExtendedGekkoMnemonic::kInsrwi, ParseAlg::Op4),
  RC_PSEUDO("rotlwi", ExtendedGekkoMnemonic::kRotlwi, ParseAlg::Op3),
  RC_PSEUDO("rotrwi", ExtendedGekkoMnemonic::kRotrwi, ParseAlg::Op3),
  RC_PSEUDO("rotlw", ExtendedGekkoMnemonic::kRotlw, ParseAlg::Op3),
  RC_PSEUDO("slwi", ExtendedGekkoMnemonic::kSlwi, ParseAlg::Op3),
  RC_PSEUDO("srwi", ExtendedGekkoMnemonic::kSrwi, ParseAlg::Op3),
  RC_PSEUDO("clrlwi", ExtendedGekkoMnemonic::kClrlwi, ParseAlg::Op3),
  RC_PSEUDO("clrrwi", ExtendedGekkoMnemonic::kClrrwi, ParseAlg::Op3),
  RC_PSEUDO("clrlslwi", ExtendedGekkoMnemonic::kClrlslwi, ParseAlg::Op4),
  LKAA_PSEUDO("bt", ExtendedGekkoMnemonic::kBt, ParseAlg::Op2),
  LKAA_PSEUDO("bf", ExtendedGekkoMnemonic::kBf, ParseAlg::Op2),
  LKAA_PSEUDO("bdnz", ExtendedGekkoMnemonic::kBdnz, ParseAlg::Op1),
  LKAA_PSEUDO("bdnzt", ExtendedGekkoMnemonic::kBdnzt, ParseAlg::Op2),
  LKAA_PSEUDO("bdnzf", ExtendedGekkoMnemonic::kBdnzf, ParseAlg::Op2),
  LKAA_PSEUDO("bdz", ExtendedGekkoMnemonic::kBdz, ParseAlg::Op1),
  LKAA_PSEUDO("bdzt", ExtendedGekkoMnemonic::kBdzt, ParseAlg::Op2),
  LKAA_PSEUDO("bdzf", ExtendedGekkoMnemonic::kBdzf, ParseAlg::Op2),
  LKAA_PSEUDO("bt-", ExtendedGekkoMnemonic::kBt, ParseAlg::Op2),
  LKAA_PSEUDO("bf-", ExtendedGekkoMnemonic::kBf, ParseAlg::Op2),
  LKAA_PSEUDO("bdnz-", ExtendedGekkoMnemonic::kBdnz, ParseAlg::Op1),
  LKAA_PSEUDO("bdnzt-", ExtendedGekkoMnemonic::kBdnzt, ParseAlg::Op2),
  LKAA_PSEUDO("bdnzf-", ExtendedGekkoMnemonic::kBdnzf, ParseAlg::Op2),
  LKAA_PSEUDO("bdz-", ExtendedGekkoMnemonic::kBdz, ParseAlg::Op1),
  LKAA_PSEUDO("bdzt-", ExtendedGekkoMnemonic::kBdzt, ParseAlg::Op2),
  LKAA_PSEUDO("bdzf-", ExtendedGekkoMnemonic::kBdzf, ParseAlg::Op2),
  LKAA_PSEUDO("bt+", ExtendedGekkoMnemonic::kBtPredict, ParseAlg::Op2),
  LKAA_PSEUDO("bf+", ExtendedGekkoMnemonic::kBfPredict, ParseAlg::Op2),
  LKAA_PSEUDO("bdnz+", ExtendedGekkoMnemonic::kBdnzPredict, ParseAlg::Op1),
  LKAA_PSEUDO("bdnzt+", ExtendedGekkoMnemonic::kBdnztPredict, ParseAlg::Op2),
  LKAA_PSEUDO("bdnzf+", ExtendedGekkoMnemonic::kBdnzfPredict, ParseAlg::Op2),
  LKAA_PSEUDO("bdz+", ExtendedGekkoMnemonic::kBdzPredict, ParseAlg::Op1),
  LKAA_PSEUDO("bdzt+", ExtendedGekkoMnemonic::kBdztPredict, ParseAlg::Op2),
  LKAA_PSEUDO("bdzf+", ExtendedGekkoMnemonic::kBdzfPredict, ParseAlg::Op2),
  LK_PSEUDO("blr", ExtendedGekkoMnemonic::kBlr, ParseAlg::None),
  LK_PSEUDO("bctr", ExtendedGekkoMnemonic::kBctr, ParseAlg::None),
  LK_PSEUDO("btlr", ExtendedGekkoMnemonic::kBtlr, ParseAlg::Op1),
  LK_PSEUDO("btctr", ExtendedGekkoMnemonic::kBtctr, ParseAlg::Op1),
  LK_PSEUDO("bflr", ExtendedGekkoMnemonic::kBflr, ParseAlg::Op1),
  LK_PSEUDO("bfctr", ExtendedGekkoMnemonic::kBfctr, ParseAlg::Op1),
  LK_PSEUDO("bdnzlr", ExtendedGekkoMnemonic::kBdnzlr, ParseAlg::None),
  LK_PSEUDO("bdnztlr", ExtendedGekkoMnemonic::kBdnztlr, ParseAlg::Op1),
  LK_PSEUDO("bdnzflr", ExtendedGekkoMnemonic::kBdnzflr, ParseAlg::Op1),
  LK_PSEUDO("bdzlr", ExtendedGekkoMnemonic::kBdzlr, ParseAlg::None),
  LK_PSEUDO("bdztlr", ExtendedGekkoMnemonic::kBdztlr, ParseAlg::Op1),
  LK_PSEUDO("bdzflr", ExtendedGekkoMnemonic::kBdzflr, ParseAlg::Op1),
  LK_PSEUDO("btlr-", ExtendedGekkoMnemonic::kBtlr, ParseAlg::Op1),
  LK_PSEUDO("btctr-", ExtendedGekkoMnemonic::kBtctr, ParseAlg::Op1),
  LK_PSEUDO("bflr-", ExtendedGekkoMnemonic::kBflr, ParseAlg::Op1),
  LK_PSEUDO("bfctr-", ExtendedGekkoMnemonic::kBfctr, ParseAlg::Op1),
  LK_PSEUDO("bdnzlr-", ExtendedGekkoMnemonic::kBdnzlr, ParseAlg::None),
  LK_PSEUDO("bdnztlr-", ExtendedGekkoMnemonic::kBdnztlr, ParseAlg::Op1),
  LK_PSEUDO("bdnzflr-", ExtendedGekkoMnemonic::kBdnzflr, ParseAlg::Op1),
  LK_PSEUDO("bdzlr-", ExtendedGekkoMnemonic::kBdzlr, ParseAlg::None),
  LK_PSEUDO("bdztlr-", ExtendedGekkoMnemonic::kBdztlr, ParseAlg::Op1),
  LK_PSEUDO("bdzflr-", ExtendedGekkoMnemonic::kBdzflr, ParseAlg::Op1),
  LK_PSEUDO("btlr+", ExtendedGekkoMnemonic::kBtlrPredict, ParseAlg::Op1),
  LK_PSEUDO("btctr+", ExtendedGekkoMnemonic::kBtctrPredict, ParseAlg::Op1),
  LK_PSEUDO("bflr+", ExtendedGekkoMnemonic::kBflrPredict, ParseAlg::Op1),
  LK_PSEUDO("bfctr+", ExtendedGekkoMnemonic::kBfctrPredict, ParseAlg::Op1),
  LK_PSEUDO("bdnzlr+", ExtendedGekkoMnemonic::kBdnzlrPredict, ParseAlg::None),
  LK_PSEUDO("bdnztlr+", ExtendedGekkoMnemonic::kBdnztlrPredict, ParseAlg::Op1),
  LK_PSEUDO("bdnzflr+", ExtendedGekkoMnemonic::kBdnzflrPredict, ParseAlg::Op1),
  LK_PSEUDO("bdzlr+", ExtendedGekkoMnemonic::kBdzlrPredict, ParseAlg::None),
  LK_PSEUDO("bdztlr+", ExtendedGekkoMnemonic::kBdztlrPredict, ParseAlg::Op1),
  LK_PSEUDO("bdzflr+", ExtendedGekkoMnemonic::kBdzflrPredict, ParseAlg::Op1),
  LKAA_PSEUDO("blt", ExtendedGekkoMnemonic::kBlt, ParseAlg::Op1Or2),
  LKAA_PSEUDO("ble", ExtendedGekkoMnemonic::kBle, ParseAlg::Op1Or2),
  LKAA_PSEUDO("beq", ExtendedGekkoMnemonic::kBeq, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bge", ExtendedGekkoMnemonic::kBge, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bgt", ExtendedGekkoMnemonic::kBgt, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bnl", ExtendedGekkoMnemonic::kBnl, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bne", ExtendedGekkoMnemonic::kBne, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bng", ExtendedGekkoMnemonic::kBng, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bso", ExtendedGekkoMnemonic::kBso, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bns", ExtendedGekkoMnemonic::kBns, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bun", ExtendedGekkoMnemonic::kBun, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bnu", ExtendedGekkoMnemonic::kBnu, ParseAlg::Op1Or2),
  LKAA_PSEUDO("blt-", ExtendedGekkoMnemonic::kBlt, ParseAlg::Op1Or2),
  LKAA_PSEUDO("ble-", ExtendedGekkoMnemonic::kBle, ParseAlg::Op1Or2),
  LKAA_PSEUDO("beq-", ExtendedGekkoMnemonic::kBeq, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bge-", ExtendedGekkoMnemonic::kBge, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bgt-", ExtendedGekkoMnemonic::kBgt, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bnl-", ExtendedGekkoMnemonic::kBnl, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bne-", ExtendedGekkoMnemonic::kBne, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bng-", ExtendedGekkoMnemonic::kBng, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bso-", ExtendedGekkoMnemonic::kBso, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bns-", ExtendedGekkoMnemonic::kBns, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bun-", ExtendedGekkoMnemonic::kBun, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bnu-", ExtendedGekkoMnemonic::kBnu, ParseAlg::Op1Or2),
  LKAA_PSEUDO("blt+", ExtendedGekkoMnemonic::kBltPredict, ParseAlg::Op1Or2),
  LKAA_PSEUDO("ble+", ExtendedGekkoMnemonic::kBlePredict, ParseAlg::Op1Or2),
  LKAA_PSEUDO("beq+", ExtendedGekkoMnemonic::kBeqPredict, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bge+", ExtendedGekkoMnemonic::kBgePredict, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bgt+", ExtendedGekkoMnemonic::kBgtPredict, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bnl+", ExtendedGekkoMnemonic::kBnlPredict, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bne+", ExtendedGekkoMnemonic::kBnePredict, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bng+", ExtendedGekkoMnemonic::kBngPredict, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bso+", ExtendedGekkoMnemonic::kBsoPredict, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bns+", ExtendedGekkoMnemonic::kBnsPredict, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bun+", ExtendedGekkoMnemonic::kBunPredict, ParseAlg::Op1Or2),
  LKAA_PSEUDO("bnu+", ExtendedGekkoMnemonic::kBnuPredict, ParseAlg::Op1Or2),
  LK_PSEUDO("bltlr", ExtendedGekkoMnemonic::kBltlr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bltctr", ExtendedGekkoMnemonic::kBltctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("blelr", ExtendedGekkoMnemonic::kBlelr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("blectr", ExtendedGekkoMnemonic::kBlectr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("beqlr", ExtendedGekkoMnemonic::kBeqlr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("beqctr", ExtendedGekkoMnemonic::kBeqctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bgelr", ExtendedGekkoMnemonic::kBgelr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bgectr", ExtendedGekkoMnemonic::kBgectr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bgtlr", ExtendedGekkoMnemonic::kBgtlr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bgtctr", ExtendedGekkoMnemonic::kBgtctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnllr", ExtendedGekkoMnemonic::kBnllr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnlctr", ExtendedGekkoMnemonic::kBnlctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnelr", ExtendedGekkoMnemonic::kBnelr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnectr", ExtendedGekkoMnemonic::kBnectr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnglr", ExtendedGekkoMnemonic::kBnglr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bngctr", ExtendedGekkoMnemonic::kBngctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bsolr", ExtendedGekkoMnemonic::kBsolr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bsoctr", ExtendedGekkoMnemonic::kBsoctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnslr", ExtendedGekkoMnemonic::kBnslr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnsctr", ExtendedGekkoMnemonic::kBnsctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bunlr", ExtendedGekkoMnemonic::kBunlr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bunctr", ExtendedGekkoMnemonic::kBunctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnulr", ExtendedGekkoMnemonic::kBnulr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnuctr", ExtendedGekkoMnemonic::kBnuctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bltlr-", ExtendedGekkoMnemonic::kBltlr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bltctr-", ExtendedGekkoMnemonic::kBltctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("blelr-", ExtendedGekkoMnemonic::kBlelr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("blectr-", ExtendedGekkoMnemonic::kBlectr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("beqlr-", ExtendedGekkoMnemonic::kBeqlr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("beqctr-", ExtendedGekkoMnemonic::kBeqctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bgelr-", ExtendedGekkoMnemonic::kBgelr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bgectr-", ExtendedGekkoMnemonic::kBgectr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bgtlr-", ExtendedGekkoMnemonic::kBgtlr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bgtctr-", ExtendedGekkoMnemonic::kBgtctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnllr-", ExtendedGekkoMnemonic::kBnllr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnlctr-", ExtendedGekkoMnemonic::kBnlctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnelr-", ExtendedGekkoMnemonic::kBnelr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnectr-", ExtendedGekkoMnemonic::kBnectr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnglr-", ExtendedGekkoMnemonic::kBnglr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bngctr-", ExtendedGekkoMnemonic::kBngctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bsolr-", ExtendedGekkoMnemonic::kBsolr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bsoctr-", ExtendedGekkoMnemonic::kBsoctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnslr-", ExtendedGekkoMnemonic::kBnslr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnsctr-", ExtendedGekkoMnemonic::kBnsctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bunlr-", ExtendedGekkoMnemonic::kBunlr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bunctr-", ExtendedGekkoMnemonic::kBunctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnulr-", ExtendedGekkoMnemonic::kBnulr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnuctr-", ExtendedGekkoMnemonic::kBnuctr, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bltlr+", ExtendedGekkoMnemonic::kBltlrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bltctr+", ExtendedGekkoMnemonic::kBltctrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("blelr+", ExtendedGekkoMnemonic::kBlelrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("blectr+", ExtendedGekkoMnemonic::kBlectrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("beqlr+", ExtendedGekkoMnemonic::kBeqlrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("beqctr+", ExtendedGekkoMnemonic::kBeqctrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bgelr+", ExtendedGekkoMnemonic::kBgelrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bgectr+", ExtendedGekkoMnemonic::kBgectrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bgtlr+", ExtendedGekkoMnemonic::kBgtlrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bgtctr+", ExtendedGekkoMnemonic::kBgtctrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnllr+", ExtendedGekkoMnemonic::kBnllrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnlctr+", ExtendedGekkoMnemonic::kBnlctrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnelr+", ExtendedGekkoMnemonic::kBnelrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnectr+", ExtendedGekkoMnemonic::kBnectrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnglr+", ExtendedGekkoMnemonic::kBnglrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bngctr+", ExtendedGekkoMnemonic::kBngctrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bsolr+", ExtendedGekkoMnemonic::kBsolrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bsoctr+", ExtendedGekkoMnemonic::kBsoctrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnslr+", ExtendedGekkoMnemonic::kBnslrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnsctr+", ExtendedGekkoMnemonic::kBnsctrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bunlr+", ExtendedGekkoMnemonic::kBunlrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bunctr+", ExtendedGekkoMnemonic::kBunctrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnulr+", ExtendedGekkoMnemonic::kBnulrPredict, ParseAlg::NoneOrOp1),
  LK_PSEUDO("bnuctr+", ExtendedGekkoMnemonic::kBnuctrPredict, ParseAlg::NoneOrOp1),
  PLAIN_PSEUDO("crset", ExtendedGekkoMnemonic::kCrset, ParseAlg::Op1),
  PLAIN_PSEUDO("crclr", ExtendedGekkoMnemonic::kCrclr, ParseAlg::Op1),
  PLAIN_PSEUDO("crmove", ExtendedGekkoMnemonic::kCrmove, ParseAlg::Op2),
  PLAIN_PSEUDO("crnot", ExtendedGekkoMnemonic::kCrnot, ParseAlg::Op2),
  PLAIN_PSEUDO("twlt", ExtendedGekkoMnemonic::kTwlt, ParseAlg::Op2),
  PLAIN_PSEUDO("twlti", ExtendedGekkoMnemonic::kTwlti, ParseAlg::Op2),
  PLAIN_PSEUDO("twle", ExtendedGekkoMnemonic::kTwle, ParseAlg::Op2),
  PLAIN_PSEUDO("twlei", ExtendedGekkoMnemonic::kTwlei, ParseAlg::Op2),
  PLAIN_PSEUDO("tweq", ExtendedGekkoMnemonic::kTweq, ParseAlg::Op2),
  PLAIN_PSEUDO("tweqi", ExtendedGekkoMnemonic::kTweqi, ParseAlg::Op2),
  PLAIN_PSEUDO("twge", ExtendedGekkoMnemonic::kTwge, ParseAlg::Op2),
  PLAIN_PSEUDO("twgei", ExtendedGekkoMnemonic::kTwgei, ParseAlg::Op2),
  PLAIN_PSEUDO("twgt", ExtendedGekkoMnemonic::kTwgt, ParseAlg::Op2),
  PLAIN_PSEUDO("twgti", ExtendedGekkoMnemonic::kTwgti, ParseAlg::Op2),
  PLAIN_PSEUDO("twnl", ExtendedGekkoMnemonic::kTwnl, ParseAlg::Op2),
  PLAIN_PSEUDO("twnli", ExtendedGekkoMnemonic::kTwnli, ParseAlg::Op2),
  PLAIN_PSEUDO("twne", ExtendedGekkoMnemonic::kTwne, ParseAlg::Op2),
  PLAIN_PSEUDO("twnei", ExtendedGekkoMnemonic::kTwnei, ParseAlg::Op2),
  PLAIN_PSEUDO("twng", ExtendedGekkoMnemonic::kTwng, ParseAlg::Op2),
  PLAIN_PSEUDO("twngi", ExtendedGekkoMnemonic::kTwngi, ParseAlg::Op2),
  PLAIN_PSEUDO("twllt", ExtendedGekkoMnemonic::kTwllt, ParseAlg::Op2),
  PLAIN_PSEUDO("twllti", ExtendedGekkoMnemonic::kTwllti, ParseAlg::Op2),
  PLAIN_PSEUDO("twlle", ExtendedGekkoMnemonic::kTwlle, ParseAlg::Op2),
  PLAIN_PSEUDO("twllei", ExtendedGekkoMnemonic::kTwllei, ParseAlg::Op2),
  PLAIN_PSEUDO("twlge", ExtendedGekkoMnemonic::kTwlge, ParseAlg::Op2),
  PLAIN_PSEUDO("twlgei", ExtendedGekkoMnemonic::kTwlgei, ParseAlg::Op2),
  PLAIN_PSEUDO("twlgt", ExtendedGekkoMnemonic::kTwlgt, ParseAlg::Op2),
  PLAIN_PSEUDO("twlgti", ExtendedGekkoMnemonic::kTwlgti, ParseAlg::Op2),
  PLAIN_PSEUDO("twlnl", ExtendedGekkoMnemonic::kTwlnl, ParseAlg::Op2),
  PLAIN_PSEUDO("twlnli", ExtendedGekkoMnemonic::kTwlnli, ParseAlg::Op2),
  PLAIN_PSEUDO("twlng", ExtendedGekkoMnemonic::kTwlng, ParseAlg::Op2),
  PLAIN_PSEUDO("twlngi", ExtendedGekkoMnemonic::kTwlngi, ParseAlg::Op2),
  PLAIN_PSEUDO("trap", ExtendedGekkoMnemonic::kTrap, ParseAlg::None),
  PLAIN_PSEUDO("mtxer", ExtendedGekkoMnemonic::kMtxer, ParseAlg::Op1),
  PLAIN_PSEUDO("mfxer", ExtendedGekkoMnemonic::kMfxer, ParseAlg::Op1),
  PLAIN_PSEUDO("mtlr", ExtendedGekkoMnemonic::kMtlr, ParseAlg::Op1),
  PLAIN_PSEUDO("mflr", ExtendedGekkoMnemonic::kMflr, ParseAlg::Op1),
  PLAIN_PSEUDO("mtctr", ExtendedGekkoMnemonic::kMtctr, ParseAlg::Op1),
  PLAIN_PSEUDO("mfctr", ExtendedGekkoMnemonic::kMfctr, ParseAlg::Op1),
  PLAIN_PSEUDO("mtdsisr", ExtendedGekkoMnemonic::kMtdsisr, ParseAlg::Op1),
  PLAIN_PSEUDO("mfdsisr", ExtendedGekkoMnemonic::kMfdsisr, ParseAlg::Op1),
  PLAIN_PSEUDO("mtdar", ExtendedGekkoMnemonic::kMtdar, ParseAlg::Op1),
  PLAIN_PSEUDO("mfdar", ExtendedGekkoMnemonic::kMfdar, ParseAlg::Op1),
  PLAIN_PSEUDO("mtdec", ExtendedGekkoMnemonic::kMtdec, ParseAlg::Op1),
  PLAIN_PSEUDO("mfdec", ExtendedGekkoMnemonic::kMfdec, ParseAlg::Op1),
  PLAIN_PSEUDO("mtsdr1", ExtendedGekkoMnemonic::kMtsdr1, ParseAlg::Op1),
  PLAIN_PSEUDO("mfsdr1", ExtendedGekkoMnemonic::kMfsdr1, ParseAlg::Op1),
  PLAIN_PSEUDO("mtsrr0", ExtendedGekkoMnemonic::kMtsrr0, ParseAlg::Op1),
  PLAIN_PSEUDO("mfsrr0", ExtendedGekkoMnemonic::kMfsrr0, ParseAlg::Op1),
  PLAIN_PSEUDO("mtsrr1", ExtendedGekkoMnemonic::kMtsrr1, ParseAlg::Op1),
  PLAIN_PSEUDO("mfsrr1", ExtendedGekkoMnemonic::kMfsrr1, ParseAlg::Op1),
  PLAIN_PSEUDO("mtasr", ExtendedGekkoMnemonic::kMtasr, ParseAlg::Op1),
  PLAIN_PSEUDO("mfasr", ExtendedGekkoMnemonic::kMfasr, ParseAlg::Op1),
  PLAIN_PSEUDO("mtear", ExtendedGekkoMnemonic::kMtear, ParseAlg::Op1),
  PLAIN_PSEUDO("mfear", ExtendedGekkoMnemonic::kMfear, ParseAlg::Op1),
  PLAIN_PSEUDO("mttbl", ExtendedGekkoMnemonic::kMttbl, ParseAlg::Op1),
  PLAIN_PSEUDO("mftbl", ExtendedGekkoMnemonic::kMftb, ParseAlg::Op1),
  PLAIN_PSEUDO("mttbu", ExtendedGekkoMnemonic::kMttbu, ParseAlg::Op1),
  PLAIN_PSEUDO("mftbu", ExtendedGekkoMnemonic::kMftbu, ParseAlg::Op1),
  PLAIN_PSEUDO("mtsprg", ExtendedGekkoMnemonic::kMtsprg, ParseAlg::Op2),
  PLAIN_PSEUDO("mfsprg", ExtendedGekkoMnemonic::kMfsprg, ParseAlg::Op2),
  PLAIN_PSEUDO("mtibatu", ExtendedGekkoMnemonic::kMtibatu, ParseAlg::Op2),
  PLAIN_PSEUDO("mfibatu", ExtendedGekkoMnemonic::kMfibatu, ParseAlg::Op2),
  PLAIN_PSEUDO("mtibatl", ExtendedGekkoMnemonic::kMtibatl, ParseAlg::Op2),
  PLAIN_PSEUDO("mfibatl", ExtendedGekkoMnemonic::kMfibatl, ParseAlg::Op2),
  PLAIN_PSEUDO("mtdbatu", ExtendedGekkoMnemonic::kMtdbatu, ParseAlg::Op2),
  PLAIN_PSEUDO("mfdbatu", ExtendedGekkoMnemonic::kMfdbatu, ParseAlg::Op2),
  PLAIN_PSEUDO("mtdbatl", ExtendedGekkoMnemonic::kMtdbatl, ParseAlg::Op2),
  PLAIN_PSEUDO("mfdbatl", ExtendedGekkoMnemonic::kMfdbatl, ParseAlg::Op2),
  PLAIN_PSEUDO("nop", ExtendedGekkoMnemonic::kNop, ParseAlg::None),
  PLAIN_PSEUDO("li", ExtendedGekkoMnemonic::kLi, ParseAlg::Op2),
  PLAIN_PSEUDO("lis", ExtendedGekkoMnemonic::kLis, ParseAlg::Op2),
  PLAIN_PSEUDO("la", ExtendedGekkoMnemonic::kLa, ParseAlg::Op1Off1),
  RC_PSEUDO("mr", ExtendedGekkoMnemonic::kMr, ParseAlg::Op2),
  RC_PSEUDO("not", ExtendedGekkoMnemonic::kNot, ParseAlg::Op2),
  PLAIN_PSEUDO("mtcr", ExtendedGekkoMnemonic::kMtcr, ParseAlg::Op1),
  PLAIN_PSEUDO("mfspr", ExtendedGekkoMnemonic::kMfspr, ParseAlg::Op2),
  PLAIN_PSEUDO("mftb", ExtendedGekkoMnemonic::kMftb, ParseAlg::Op2),
  PLAIN_PSEUDO("mtspr", ExtendedGekkoMnemonic::kMtspr, ParseAlg::Op2),
};

#undef MNEMONIC
#undef PLAIN_MNEMONIC
#undef RC_MNEMONIC
#undef OERC_MNEMONIC
#undef LK_MNEMONIC
#undef AALK_MNEMONIC
#undef PSEUDO
#undef PLAIN_PSEUDO
#undef RC_PSEUDO
#undef OERC_PSEUDO
#undef LK_PSEUDO
#undef LKAA_PSEUDO

//////////////////////
// ASSEMBLER TABLES //
//////////////////////
#define EMIT_MNEMONIC_ENTRY(opcode_val, extra_bits, ...) \
  MnemonicDesc {insert_opcode(opcode_val) | (extra_bits), \
                std::initializer_list<OperandDesc>{__VA_ARGS__}.size(), {__VA_ARGS__}}
#define MNEMONIC(opcode_val, extra_bits, ...) \
  EMIT_MNEMONIC_ENTRY(opcode_val, extra_bits, __VA_ARGS__), \
  kInvalidMnemonic, \
  kInvalidMnemonic, \
  kInvalidMnemonic
#define BASIC_MNEMONIC(opcode_val, ...) MNEMONIC(opcode_val, 0, __VA_ARGS__)
#define RC_MNEMONIC(opcode_val, extra_bits, ...) \
  EMIT_MNEMONIC_ENTRY(opcode_val, extra_bits, __VA_ARGS__), \
  EMIT_MNEMONIC_ENTRY(opcode_val, ((extra_bits) | insert_val(1, 31, 31)), __VA_ARGS__), \
  kInvalidMnemonic, \
  kInvalidMnemonic
#define OERC_MNEMONIC(opcode_val, extra_bits, ...) \
  EMIT_MNEMONIC_ENTRY(opcode_val, extra_bits, __VA_ARGS__), \
  EMIT_MNEMONIC_ENTRY(opcode_val, ((extra_bits) | insert_val(1, 31, 31)), __VA_ARGS__), \
  EMIT_MNEMONIC_ENTRY(opcode_val, ((extra_bits) | insert_val(1, 21, 21)), __VA_ARGS__), \
  EMIT_MNEMONIC_ENTRY(opcode_val, ((extra_bits) | insert_val(1, 31, 31) | insert_val(1, 21, 21)), __VA_ARGS__)
#define LK_MNEMONIC(opcode_val, extra_bits, ...) \
  EMIT_MNEMONIC_ENTRY(opcode_val, extra_bits, __VA_ARGS__), \
  EMIT_MNEMONIC_ENTRY(opcode_val, ((extra_bits) | insert_val(1, 31, 31)), __VA_ARGS__), \
  kInvalidMnemonic, \
  kInvalidMnemonic
#define AALK_MNEMONIC(opcode_val, extra_bits, ...) \
  EMIT_MNEMONIC_ENTRY(opcode_val, extra_bits, __VA_ARGS__), \
  EMIT_MNEMONIC_ENTRY(opcode_val, ((extra_bits) | insert_val(0b01, 30, 31)), __VA_ARGS__), \
  EMIT_MNEMONIC_ENTRY(opcode_val, ((extra_bits) | insert_val(0b10, 30, 31)), __VA_ARGS__), \
  EMIT_MNEMONIC_ENTRY(opcode_val, ((extra_bits) | insert_val(0b11, 30, 31)), __VA_ARGS__)

// Defines all basic mnemonics that Broadway/Gekko supports
std::array<MnemonicDesc, kNumMnemonics * kVariantPermutations> mnemonics =
{
  // A-2
  OERC_MNEMONIC(31, insert_val(266, 22, 30), _D, _A, _B),  // add
  OERC_MNEMONIC(31, insert_val(10, 22, 30), _D, _A, _B),  // addc
  OERC_MNEMONIC(31, insert_val(138, 22, 30), _D, _A, _B),  // adde
  BASIC_MNEMONIC(14, _D, _A, _SIMM),  // addi
  BASIC_MNEMONIC(12, _D, _A, _SIMM),  // addic
  BASIC_MNEMONIC(13, _D, _A, _SIMM),  // addic.
  BASIC_MNEMONIC(15, _D, _A, _SIMM),  // addis
  OERC_MNEMONIC(31, insert_val(234, 22, 30), _D, _A),  // addme
  OERC_MNEMONIC(31, insert_val(202, 22, 30), _D, _A),  // addze
  OERC_MNEMONIC(31, insert_val(491, 22, 30), _D, _A, _B),  // divw
  OERC_MNEMONIC(31, insert_val(459, 22, 30), _D, _A, _B),  // divwu
  RC_MNEMONIC(31, insert_val(75, 22, 30), _D, _A, _B),  // mulhw
  RC_MNEMONIC(31, insert_val(11, 22, 30), _D, _A, _B),  // mulhwu
  BASIC_MNEMONIC(7, _D, _A, _SIMM),  // mulli
  OERC_MNEMONIC(31, insert_val(235, 22, 30), _D, _A, _B),  // mullw
  OERC_MNEMONIC(31, insert_val(104, 22, 30), _D, _A),  // neg
  OERC_MNEMONIC(31, insert_val(40, 22, 30), _D, _A, _B),  // subf
  OERC_MNEMONIC(31, insert_val(8, 22, 30), _D, _A, _B),  // subfc
  OERC_MNEMONIC(31, insert_val(136, 22, 30), _D, _A, _B),  // subfe
  BASIC_MNEMONIC(8, _D, _A, _SIMM),  // subfic
  OERC_MNEMONIC(31, insert_val(232, 22, 30), _D, _A),  // subfme
  OERC_MNEMONIC(31, insert_val(200, 22, 30), _D, _A),  // subfze

  // A-3
  MNEMONIC(31, insert_val(0, 21, 30), _Crfd, _L, _A, _B),  // cmp
  BASIC_MNEMONIC(11, _Crfd, _L, _A, _SIMM),  // cmpi
  MNEMONIC(31, insert_val(32, 21, 30), _Crfd, _L, _A, _B),  // cmpl
  BASIC_MNEMONIC(10, _Crfd, _L, _A, _UIMM),  // cmpli

  // A-4
  RC_MNEMONIC(31, insert_val(28, 21, 30), _A, _S, _B),  // and
  RC_MNEMONIC(31, insert_val(60, 21, 30), _A, _S, _B),  // andc
  BASIC_MNEMONIC(28, _A, _S, _UIMM),  // andi.
  BASIC_MNEMONIC(29, _A, _S, _UIMM),  // andis.
  RC_MNEMONIC(31, insert_val(26, 21, 30), _A, _S),  // cntlzw
  RC_MNEMONIC(31, insert_val(284, 21, 30), _A, _S, _B),  // eqv
  RC_MNEMONIC(31, insert_val(954, 21, 30), _A, _S),  // extsb
  RC_MNEMONIC(31, insert_val(922, 21, 30), _A, _S),  // extsh
  RC_MNEMONIC(31, insert_val(476, 21, 30), _A, _S, _B),  // nand
  RC_MNEMONIC(31, insert_val(124, 21, 30), _A, _S, _B),  // nor
  RC_MNEMONIC(31, insert_val(444, 21, 30), _A, _S, _B),  // or
  RC_MNEMONIC(31, insert_val(412, 21, 30), _A, _S, _B),  // orc
  BASIC_MNEMONIC(24, _A, _S, _UIMM),  // ori
  BASIC_MNEMONIC(25, _A, _S, _UIMM),  // oris
  RC_MNEMONIC(31, insert_val(316, 21, 30), _A, _S, _B),  // xor
  BASIC_MNEMONIC(26, _A, _S, _UIMM),  // xori
  BASIC_MNEMONIC(27, _A, _S, _UIMM),  // xoris

  // A-5
  RC_MNEMONIC(20, 0, _A, _S, _SH, _MB, _ME),  // rlwimi
  RC_MNEMONIC(21, 0, _A, _S, _SH, _MB, _ME),  // rlwinm
  RC_MNEMONIC(23, 0, _A, _S, _B, _MB, _ME),  // rlwnm

  // A-6
  RC_MNEMONIC(31, insert_val(24, 21, 30), _A, _S, _B),  // slw
  RC_MNEMONIC(31, insert_val(792, 21, 30), _A, _S, _B),  // sraw
  RC_MNEMONIC(31, insert_val(824, 21, 30), _A, _S, _SH),  // srawi
  RC_MNEMONIC(31, insert_val(536, 21, 30), _A, _S, _B),  // srw

  // A-7
  RC_MNEMONIC(63, insert_val(21, 26, 30), _D, _A, _B),  // fadd
  RC_MNEMONIC(59, insert_val(21, 26, 30), _D, _A, _B),  // fadds
  RC_MNEMONIC(63, insert_val(18, 26, 30), _D, _A, _B),  // fdiv
  RC_MNEMONIC(59, insert_val(18, 26, 30), _D, _A, _B),  // fdivs
  RC_MNEMONIC(63, insert_val(25, 26, 30), _D, _A, _C),  // fmul
  RC_MNEMONIC(59, insert_val(25, 26, 30), _D, _A, _C),  // fmuls
  RC_MNEMONIC(59, insert_val(24, 26, 30), _D, _B),  // fres
  RC_MNEMONIC(63, insert_val(26, 26, 30), _D, _B),  // frsqrte
  RC_MNEMONIC(63, insert_val(20, 26, 30), _D, _A, _B),  // fsub
  RC_MNEMONIC(59, insert_val(20, 26, 30), _D, _A, _B),  // fsubs
  RC_MNEMONIC(63, insert_val(23, 26, 30), _D, _A, _C, _B),  // fsel

  // A-8
  RC_MNEMONIC(63, insert_val(29, 26, 30), _D, _A, _C, _B),  // fmadd
  RC_MNEMONIC(59, insert_val(29, 26, 30), _D, _A, _C, _B),  // fmadds
  RC_MNEMONIC(63, insert_val(28, 26, 30), _D, _A, _C, _B),  // fmsub
  RC_MNEMONIC(59, insert_val(28, 26, 30), _D, _A, _C, _B),  // fmsubs
  RC_MNEMONIC(63, insert_val(31, 26, 30), _D, _A, _C, _B),  // fnmadd
  RC_MNEMONIC(59, insert_val(31, 26, 30), _D, _A, _C, _B),  // fnmadds
  RC_MNEMONIC(63, insert_val(30, 26, 30), _D, _A, _C, _B),  // fnmsub
  RC_MNEMONIC(59, insert_val(30, 26, 30), _D, _A, _C, _B),  // fnmsubs

  // A-9
  RC_MNEMONIC(63, insert_val(14, 21, 30), _D, _B),  // fctiw
  RC_MNEMONIC(63, insert_val(15, 21, 30), _D, _B),  // fctiwz
  RC_MNEMONIC(63, insert_val(12, 21, 30), _D, _B),  // frsp

  // A-10
  MNEMONIC(63, insert_val(32, 21, 30), _Crfd, _A, _B),  // fcmpo
  MNEMONIC(63, insert_val(0, 21, 30), _Crfd, _A, _B),  // fcmpu

  // A-11
  MNEMONIC(63, insert_val(64, 21, 30), _Crfd, _Crfs),  // mcrfs
  RC_MNEMONIC(63, insert_val(583, 21, 30), _D),  // mffs
  RC_MNEMONIC(63, insert_val(70, 21, 30), _Crbd),  // mtfsb0
  RC_MNEMONIC(63, insert_val(38, 21, 30), _Crbd),  // mtfsb1
  RC_MNEMONIC(63, insert_val(711, 21, 30), _FM, _B),  // mtfsf
  RC_MNEMONIC(63, insert_val(134, 21, 30), _Crfd, _IMM),  // mtfsfi

  // A-12
  BASIC_MNEMONIC(34, _D, _Offd, _A),  // lbz
  BASIC_MNEMONIC(35, _D, _Offd, _A),  // lbzu
  MNEMONIC(31, insert_val(119, 21, 30), _D, _A, _B),  // lbzux
  MNEMONIC(31, insert_val(87, 21, 30), _D, _A, _B),  // lbzx
  BASIC_MNEMONIC(42, _D, _Offd, _A),  // lha
  BASIC_MNEMONIC(43, _D, _Offd, _A),  // lhau
  MNEMONIC(31, insert_val(375, 21, 30), _D, _A, _B),  // lhaux
  MNEMONIC(31, insert_val(343, 21, 30), _D, _A, _B),  // lhax
  BASIC_MNEMONIC(40, _D, _Offd, _A),  // lhz
  BASIC_MNEMONIC(41, _D, _Offd, _A),  // lhzu
  MNEMONIC(31, insert_val(311, 21, 30), _D, _A, _B),  // lhzux
  MNEMONIC(31, insert_val(279, 21, 30), _D, _A, _B),  // lhzx
  BASIC_MNEMONIC(32, _D, _Offd, _A),  // lwz
  BASIC_MNEMONIC(33, _D, _Offd, _A),  // lwzu
  MNEMONIC(31, insert_val(55, 21, 30), _D, _A, _B),  // lwzux
  MNEMONIC(31, insert_val(23, 21, 30), _D, _A, _B),  // lwzx

  // A-13
  BASIC_MNEMONIC(38, _S, _Offd, _A),  // stb
  BASIC_MNEMONIC(39, _S, _Offd, _A),  // stbu
  MNEMONIC(31, insert_val(247, 21, 30), _S, _A, _B),  // stbux
  MNEMONIC(31, insert_val(215, 21, 30), _S, _A, _B),  // stbx
  BASIC_MNEMONIC(44, _S, _Offd, _A),  // sth
  BASIC_MNEMONIC(45, _S, _Offd, _A),  // sthu
  MNEMONIC(31, insert_val(439, 21, 30), _S, _A, _B),  // sthux
  MNEMONIC(31, insert_val(407, 21, 30), _S, _A, _B),  // sthx
  BASIC_MNEMONIC(36, _S, _Offd, _A),  // stw
  BASIC_MNEMONIC(37, _S, _Offd, _A),  // stwu
  MNEMONIC(31, insert_val(183, 21, 30), _S, _A, _B),  // stwux
  MNEMONIC(31, insert_val(151, 21, 30), _S, _A, _B),  // stwx

  // A-14
  MNEMONIC(31, insert_val(790, 21, 30), _D, _A, _B),  // lhbrx
  MNEMONIC(31, insert_val(534, 21, 30), _D, _A, _B),  // lwbrx
  MNEMONIC(31, insert_val(918, 21, 30), _S, _A, _B),  // sthbrx
  MNEMONIC(31, insert_val(662, 21, 30), _S, _A, _B),  // stwbrx

  // A-15
  BASIC_MNEMONIC(46, _D, _Offd, _A),  // lmw
  BASIC_MNEMONIC(47, _S, _Offd, _A),  // stmw

  // A-16
  MNEMONIC(31, insert_val(597, 21, 30), _D, _A, _NB),  // lswi
  MNEMONIC(31, insert_val(533, 21, 30), _D, _A, _B),  // lswx
  MNEMONIC(31, insert_val(725, 21, 30), _S, _A, _NB),  // stswi
  MNEMONIC(31, insert_val(661, 21, 30), _S, _A, _B),  // stswx

  // A-17
  MNEMONIC(31, insert_val(854, 21, 30)),  // eieio
  MNEMONIC(19, insert_val(150, 21, 30)),  // isync
  MNEMONIC(31, insert_val(20, 21, 30), _D, _A, _B),  // lwarx
  MNEMONIC(31, insert_val(150, 21, 30) | insert_val(1, 31, 31), _S, _A, _B),  // stwcx.
  MNEMONIC(31, insert_val(598, 21, 30)),  // sync

  // A-18
  BASIC_MNEMONIC(50, _D, _Offd, _A),  // lfd
  BASIC_MNEMONIC(51, _D, _Offd, _A),  // lfdu
  MNEMONIC(31, insert_val(631, 21, 30), _D, _A, _B),  // lfdux
  MNEMONIC(31, insert_val(599, 21, 30), _D, _A, _B),  // lfdx
  BASIC_MNEMONIC(48, _D, _Offd, _A),  // lfs
  BASIC_MNEMONIC(49, _D, _Offd, _A),  // lfsu
  MNEMONIC(31, insert_val(567, 21, 30), _D, _A, _B),  // lfsux
  MNEMONIC(31, insert_val(535, 21, 30), _D, _A, _B),  // lfsx

  // A-19
  BASIC_MNEMONIC(54, _S, _Offd, _A),  // stfd
  BASIC_MNEMONIC(55, _S, _Offd, _A),  // stfdu
  MNEMONIC(31, insert_val(759, 21, 30), _S, _A, _B),  // stfdux
  MNEMONIC(31, insert_val(727, 21, 30), _S, _A, _B),  // stfdx
  MNEMONIC(31, insert_val(983, 21, 30), _S, _A, _B),  // stfiwx
  BASIC_MNEMONIC(52, _S, _Offd, _A),  // stfs
  BASIC_MNEMONIC(53, _S, _Offd, _A),  // stfsu
  MNEMONIC(31, insert_val(695, 21, 30), _S, _A, _B),  // stfsux
  MNEMONIC(31, insert_val(663, 21, 30), _S, _A, _B),  // stfsx

  // A-20
  RC_MNEMONIC(63, insert_val(264, 21, 30), _D, _B),  // fabs
  RC_MNEMONIC(63, insert_val(72, 21, 30), _D, _B),  // fmr
  RC_MNEMONIC(63, insert_val(136, 21, 30), _D, _B),  // fnabs
  RC_MNEMONIC(63, insert_val(40, 21, 30), _D, _B),  // fneg

  // A-21
  AALK_MNEMONIC(18, 0, _LI),  // b
  AALK_MNEMONIC(16, 0, _BO, _BI, _BD),  // bc
  LK_MNEMONIC(19, insert_val(528, 21, 30), _BO, _BI),  // bcctr
  LK_MNEMONIC(19, insert_val(16, 21, 30), _BO, _BI),  // bclr

  // A-22
  MNEMONIC(19, insert_val(257, 21, 30), _Crbd, _Crba, _Crbb),  // crand
  MNEMONIC(19, insert_val(129, 21, 30), _Crbd, _Crba, _Crbb),  // crandc
  MNEMONIC(19, insert_val(289, 21, 30), _Crbd, _Crba, _Crbb),  // creqv
  MNEMONIC(19, insert_val(225, 21, 30), _Crbd, _Crba, _Crbb),  // crnand
  MNEMONIC(19, insert_val(33, 21, 30), _Crbd, _Crba, _Crbb),  // crnor
  MNEMONIC(19, insert_val(449, 21, 30), _Crbd, _Crba, _Crbb),  // cror
  MNEMONIC(19, insert_val(417, 21, 30), _Crbd, _Crba, _Crbb),  // crorc
  MNEMONIC(19, insert_val(193, 21, 30), _Crbd, _Crba, _Crbb),  // crxor
  MNEMONIC(19, insert_val(0, 21, 30), _Crfd, _Crfs),  // mcrf

  // A-23
  MNEMONIC(19, insert_val(50, 21, 30)),  // rfi
  MNEMONIC(17, insert_val(1, 30, 30)),  // sc

  // A-24
  MNEMONIC(31, insert_val(4, 21, 30), _TO, _A, _B),  // tw
  BASIC_MNEMONIC(3, _TO, _A, _SIMM),  // twi

  // A-25
  MNEMONIC(31, insert_val(512, 21, 30), _Crfd),  // mcrxr
  MNEMONIC(31, insert_val(19, 21, 30), _D),  // mfcr
  MNEMONIC(31, insert_val(83, 21, 30), _D),  // mfmsr
  MNEMONIC(31, insert_val(339, 21, 30), _D, _SPR),  // mfspr
  MNEMONIC(31, insert_val(371, 21, 30), _D, _TPR),  // mftb
  MNEMONIC(31, insert_val(144, 21, 30), _CRM, _S),  // mtcrf
  MNEMONIC(31, insert_val(146, 21, 30), _S),  // mtmsr
  MNEMONIC(31, insert_val(467, 21, 30), _SPR, _D),  // mtspr

  // A-26
  MNEMONIC(31, insert_val(86, 21, 30), _A, _B),  // dcbf
  MNEMONIC(31, insert_val(470, 21, 30), _A, _B),  // dcbi
  MNEMONIC(31, insert_val(54, 21, 30), _A, _B),  // dcbst
  MNEMONIC(31, insert_val(278, 21, 30), _A, _B),  // dcbt
  MNEMONIC(31, insert_val(246, 21, 30), _A, _B),  // dcbtst
  MNEMONIC(31, insert_val(1014, 21, 30), _A, _B),  // dcbz
  MNEMONIC(31, insert_val(982, 21, 30), _A, _B),  // icbi

  // A-27
  MNEMONIC(31, insert_val(595, 21, 30), _D, _SR),  // mfsr
  MNEMONIC(31, insert_val(659, 21, 30), _D, _B),  // mfsrin
  MNEMONIC(31, insert_val(210, 21, 30), _SR, _S),  // mtsr
  MNEMONIC(31, insert_val(242, 21, 30), _S, _B),  // mtsrin

  // A-28
  MNEMONIC(31, insert_val(306, 21, 30), _B),  // tlbie
  MNEMONIC(31, insert_val(566, 21, 30)),  // tlbsync

  // A-29
  MNEMONIC(31, insert_val(310, 21, 30), _D, _A, _B),  // eciwx
  MNEMONIC(31, insert_val(438, 21, 30), _S, _A, _B),  // ecowx

  // A-30
  MNEMONIC(4, insert_val(6, 25, 30), _D, _A, _B, _I2, _W2),  // psq_lx
  MNEMONIC(4, insert_val(7, 25, 30), _S, _A, _B, _I2, _W2),  // psq_stx
  MNEMONIC(4, insert_val(38, 25, 30), _D, _A, _B, _I2, _W2),  // psq_lux
  MNEMONIC(4, insert_val(39, 25, 30), _S, _A, _B, _I2, _W2),  // psq_stux
  BASIC_MNEMONIC(56, _D, _OffdPs, _A, _I1, _W1),  // psq_l
  BASIC_MNEMONIC(57, _D, _OffdPs, _A, _I1, _W1),  // psq_lu
  BASIC_MNEMONIC(60, _S, _OffdPs, _A, _I1, _W1),  // psq_st
  BASIC_MNEMONIC(61, _S, _OffdPs, _A, _I1, _W1),  // psq_stu

  // A-31
  RC_MNEMONIC(4, insert_val(18, 26, 30), _D, _A, _B),  // ps_div
  RC_MNEMONIC(4, insert_val(20, 26, 30), _D, _A, _B),  // ps_sub
  RC_MNEMONIC(4, insert_val(21, 26, 30), _D, _A, _B),  // ps_add
  RC_MNEMONIC(4, insert_val(23, 26, 30), _D, _A, _C, _B),  // ps_sel
  RC_MNEMONIC(4, insert_val(24, 26, 30), _D, _B),  // ps_res
  RC_MNEMONIC(4, insert_val(25, 26, 30), _D, _A, _C),  // ps_mul
  RC_MNEMONIC(4, insert_val(26, 26, 30), _D, _B),  // ps_rsqrte
  RC_MNEMONIC(4, insert_val(28, 26, 30), _D, _A, _C, _B),  // ps_msub
  RC_MNEMONIC(4, insert_val(29, 26, 30), _D, _A, _C, _B),  // ps_madd
  RC_MNEMONIC(4, insert_val(30, 26, 30), _D, _A, _C, _B),  // ps_nmsub
  RC_MNEMONIC(4, insert_val(31, 26, 30), _D, _A, _C, _B),  // ps_nmadd
  RC_MNEMONIC(4, insert_val(40, 21, 30), _D, _B),  // ps_neg
  RC_MNEMONIC(4, insert_val(72, 21, 30), _D, _B),  // ps_mr
  RC_MNEMONIC(4, insert_val(136, 21, 30), _D, _B),  // ps_nabs
  RC_MNEMONIC(4, insert_val(264, 21, 30), _D, _B),  // ps_abs

  // A-32
  RC_MNEMONIC(4, insert_val(10, 26, 30), _D, _A, _C, _B),  // ps_sum0
  RC_MNEMONIC(4, insert_val(11, 26, 30), _D, _A, _C, _B),  // ps_sum1
  RC_MNEMONIC(4, insert_val(12, 26, 30), _D, _A, _C),  // ps_muls0
  RC_MNEMONIC(4, insert_val(13, 26, 30), _D, _A, _C),  // ps_muls1
  RC_MNEMONIC(4, insert_val(14, 26, 30), _D, _A, _C, _B),  // ps_madds0
  RC_MNEMONIC(4, insert_val(15, 26, 30), _D, _A, _C, _B),  // ps_madds1
  MNEMONIC(4, insert_val(0, 21, 30), _Crfd, _A, _B),  // ps_cmpu0
  MNEMONIC(4, insert_val(32, 21, 30), _Crfd, _A, _B),  // ps_cmpo0
  MNEMONIC(4, insert_val(64, 21, 30), _Crfd, _A, _B),  // ps_cmpu1
  MNEMONIC(4, insert_val(96, 21, 30), _Crfd, _A, _B),  // ps_cmpo1
  RC_MNEMONIC(4, insert_val(528, 21, 30), _D, _A, _B),  // ps_merge00
  RC_MNEMONIC(4, insert_val(560, 21, 30), _D, _A, _B),  // ps_merge01
  RC_MNEMONIC(4, insert_val(592, 21, 30), _D, _A, _B),  // ps_merge10
  RC_MNEMONIC(4, insert_val(624, 21, 30), _D, _A, _B),  // ps_merge11
  MNEMONIC(4, insert_val(1014, 21, 30), _A, _B),  // dcbz_l
};

namespace
{
// Reused operand translators for extended mnemonics
void negate_SIMM(OperandList& operands)
{
  operands[2] = -operands[2];
}

void swap_ops_1_2(OperandList& operands)
{
  operands[2] ^= operands[1];
  operands[1] ^= operands[2];
  operands[2] ^= operands[1];
}

void set_compare_word_mode(OperandList& operands)
{
  if (operands.count == 2)
  {
    operands.Insert(0, 0);
  }
  operands.Insert(1, 0);
}

template <u32 BO, u32 BI>
void fill_bo_bi(OperandList& operands)
{
  operands.Insert(0, BO);
  operands.Insert(1, BI);
}

template <size_t Idx>
void bitswap_idx(OperandList& operands)
{
  operands[Idx] = spr_bitswap(operands[Idx]);
}

template <u32 BO, u32 Cond, u32 ParamCount>
void fill_bo_bicond(OperandList& operands)
{
  if (operands.count < ParamCount)
  {
    operands.Insert(0, 0);
  }
  operands[0] = (operands[0] << 2) | Cond;
  operands.Insert(0, BO);
}

template <u32 BO>
void fill_bo(OperandList& operands)
{
  operands.Insert(0, BO);
}

template <u32 TO>
void trap_set_TO(OperandList& operands)
{
  operands.Insert(0, TO);
}

template <u32 SPRG>
void fill_mtspr(OperandList& operands)
{
  operands.Insert(0, SPRG);
}

template <u32 SPRG>
void fill_mfspr(OperandList& operands)
{
  operands.Insert(1, SPRG);
}

template <u32 SPRG>
void fill_mtspr_bat(OperandList& operands)
{
  operands.Insert(0, 2 * operands[0] + SPRG);
}

template <u32 SPRG>
void fill_mfspr_bat(OperandList& operands)
{
  operands.Insert(1, 2 * operands[1] + SPRG);
}
}  // namespace

#define PSEUDO(base, variant_bits, cb) \
  ExtendedMnemonicDesc {static_cast<size_t>(base) * kVariantPermutations + variant_bits, cb}
#define PLAIN_PSEUDO(base, cb) \
  PSEUDO(base, kPlainMnemonic, cb), \
  kInvalidExtMnemonic, \
  kInvalidExtMnemonic, \
  kInvalidExtMnemonic
#define RC_PSEUDO(base, cb) \
  PSEUDO(base, kPlainMnemonic, cb), \
  PSEUDO(base, kRecordBit, cb), \
  kInvalidExtMnemonic, \
  kInvalidExtMnemonic
#define OERC_PSEUDO(base, cb) \
  PSEUDO(base, kPlainMnemonic, cb), \
  PSEUDO(base, kRecordBit, cb), \
  PSEUDO(base, kOverflowExceptionBit, cb), \
  PSEUDO(base, (kRecordBit | kOverflowExceptionBit), cb)
#define LK_PSEUDO(base, cb) \
  PSEUDO(base, kPlainMnemonic, cb), \
  PSEUDO(base, kLinkBit, cb), \
  kInvalidExtMnemonic, \
  kInvalidExtMnemonic
#define LKAA_PSEUDO(base, cb) \
  PSEUDO(base, kPlainMnemonic, cb), \
  PSEUDO(base, kLinkBit, cb), \
  PSEUDO(base, kAbsoluteAddressBit, cb), \
  PSEUDO(base, (kLinkBit | kAbsoluteAddressBit), cb)

std::array<ExtendedMnemonicDesc, kNumExtMnemonics * kVariantPermutations> extended_mnemonics =
{
  // E.2.1
  PLAIN_PSEUDO(GekkoMnemonic::kAddi, negate_SIMM),  // subi
  PLAIN_PSEUDO(GekkoMnemonic::kAddis, negate_SIMM),  // subis
  PLAIN_PSEUDO(GekkoMnemonic::kAddic, negate_SIMM),  // subic
  PLAIN_PSEUDO(GekkoMnemonic::kAddicDot, negate_SIMM),  // subic.

  // E.2.2
  OERC_PSEUDO(GekkoMnemonic::kSubf, swap_ops_1_2),  // sub
  OERC_PSEUDO(GekkoMnemonic::kSubfc, swap_ops_1_2),  // subc

  // E.3.2
  PLAIN_PSEUDO(GekkoMnemonic::kCmpi, set_compare_word_mode),  // cmpwi
  PLAIN_PSEUDO(GekkoMnemonic::kCmp, set_compare_word_mode),  // cmpw
  PLAIN_PSEUDO(GekkoMnemonic::kCmpli, set_compare_word_mode),  // cmplwi
  PLAIN_PSEUDO(GekkoMnemonic::kCmpl, set_compare_word_mode),  // cmplw

  // E.4.2
  RC_PSEUDO(GekkoMnemonic::kRlwinm, ([](OperandList& operands)
    {
      const u32 n = operands[2], b = operands[3];
      operands[2] = b; operands[3] = 0; operands.Insert(4, n - 1);
    })),  // extlwi
  RC_PSEUDO(GekkoMnemonic::kRlwinm, ([](OperandList& operands)
    {
      const u32 n = operands[2], b = operands[3];
      operands[2] = b + n; operands[3] = 32 - n; operands.Insert(4, 31);
    })),  // extrwi
  RC_PSEUDO(GekkoMnemonic::kRlwimi, ([](OperandList& operands)
    {
      const u32 n = operands[2], b = operands[3];
      operands[2] = 32 - b; operands[3] = b; operands.Insert(4, b + n - 1);
    })),  // inslwi
  RC_PSEUDO(GekkoMnemonic::kRlwimi, ([](OperandList& operands)
    {
      const u32 n = operands[2], b = operands[3];
      operands[2] = 32 - (b + n); operands[3] = b; operands.Insert(4, b + n - 1);
    })),  // insrwi
  RC_PSEUDO(GekkoMnemonic::kRlwinm, ([](OperandList& operands)
    {
      operands.Insert(3, 0); operands.Insert(4, 31);
    })),  // rotlwi
  RC_PSEUDO(GekkoMnemonic::kRlwinm, ([](OperandList& operands)
    {
      const u32 n = operands[2];
      operands[2] = 32 - n; operands.Insert(3, 0); operands.Insert(4, 31);
    })),  // rotrwi
  RC_PSEUDO(GekkoMnemonic::kRlwnm, ([](OperandList& operands)
    {
      operands.Insert(3, 0); operands.Insert(4, 31);
    })),  // rotlw
  RC_PSEUDO(GekkoMnemonic::kRlwinm, ([](OperandList& operands)
    {
      const u32 n = operands[2];
      operands.Insert(3, 0); operands.Insert(4, 31 - n);
    })),  // slwi
  RC_PSEUDO(GekkoMnemonic::kRlwinm, ([](OperandList& operands)
    {
      const u32 n = operands[2];
      operands[2] = 32 - n; operands.Insert(3, n); operands.Insert(4, 31);
    })),  // srwi
  RC_PSEUDO(GekkoMnemonic::kRlwinm, ([](OperandList& operands)
    {
      const u32 n = operands[2];
      operands[2] = 0; operands.Insert(3, n); operands.Insert(4, 31);
    })),  // clrlwi
  RC_PSEUDO(GekkoMnemonic::kRlwinm, ([](OperandList& operands)
    {
      const u32 n = operands[2];
      operands[2] = 0; operands.Insert(3, 0); operands.Insert(4, 31 - n);
    })),  // clrrwi
  RC_PSEUDO(GekkoMnemonic::kRlwinm, ([](OperandList& operands)
    {
      const u32 b = operands[2], n = operands[3];
      operands[2] = n; operands[3] = b - n; operands.Insert(4, 31 - n);
    })),  // clrlslwi

  // E.5.2
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo<12>)),  // bt
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo<4>)),  // bf
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bi<16, 0>)),  // bdnz
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo<8>)),  // bdnzt
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo<0>)),  // bdnzf
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bi<18, 0>)),  // bdz
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo<10>)),  // bdzt
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo<2>)),  // bdzf
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo<13>)),  // bt+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo<5>)),  // bf+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bi<17, 0>)),  // bdnz+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo<9>)),  // bdnzt+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo<1>)),  // bdnzf+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bi<19, 0>)),  // bdz+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo<11>)),  // bdzt+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo<3>)),  // bdzf+

  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bi<20, 0>)),  // blr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo<12>)),  // btlr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo<4>)),  // bflr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bi<16, 0>)),  // bdnzlr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo<8>)),  // bdnztlr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo<0>)),  // bdnzflr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bi<18, 0>)),  // bdzlr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo<10>)),  // bdztlr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo<2>)),  // bdzflr

  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo<13>)),  // btlr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo<5>)),  // bflr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bi<17, 0>)),  // bdnzlr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo<9>)),  // bdnztlr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo<1>)),  // bdnzflr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bi<19, 0>)),  // bdzlr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo<11>)),  // bdztlr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo<3>)),  // bdzflr+

  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bi<20, 0>)),  // bctr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo<12>)),  // btctr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo<4>)),  // bfctr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo<13>)),  // btctr+
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo<5>)),  // bfctr+

  // E.5.3
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<12, 0, 2>)),  // blt
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<4, 1, 2>)),  // ble
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<12, 2, 2>)),  // beq
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<4, 0, 2>)),  // bge
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<12, 1, 2>)),  // bgt
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<4, 0, 2>)),  // bnl
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<4, 2, 2>)),  // bne
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<4, 1, 2>)),  // bng
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<12, 3, 2>)),  // bso
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<4, 3, 2>)),  // bns
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<12, 3, 2>)),  // bun
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<4, 3, 2>)),  // bnu

  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<13, 0, 2>)),  // blt+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<5, 1, 2>)),  // ble+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<13, 2, 2>)),  // beq+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<5, 0, 2>)),  // bge+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<13, 1, 2>)),  // bgt+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<5, 0, 2>)),  // bnl+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<5, 2, 2>)),  // bne+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<5, 1, 2>)),  // bng+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<13, 3, 2>)),  // bso+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<5, 3, 2>)),  // bns+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<13, 3, 2>)),  // bun+
  LKAA_PSEUDO(GekkoMnemonic::kBc, (fill_bo_bicond<5, 3, 2>)),  // bnu+

  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<12, 0, 1>)),  // bltlr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<4, 1, 1>)),  // blelr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<12, 2, 1>)),  // beqlr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<4, 0, 1>)),  // bgelr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<12, 1, 1>)),  // bgtlr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<4, 0, 1>)),  // bnllr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<4, 2, 1>)),  // bnelr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<4, 1, 1>)),  // bnglr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<12, 3, 1>)),  // bsolr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<4, 3, 1>)),  // bnslr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<12, 3, 1>)),  // bunlr
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<4, 3, 1>)),  // bnulr

  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<13, 0, 1>)),  // bltlr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<5, 1, 1>)),  // blelr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<13, 2, 1>)),  // beqlr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<5, 0, 1>)),  // bgelr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<13, 1, 1>)),  // bgtlr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<5, 0, 1>)),  // bnllr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<5, 2, 1>)),  // bnelr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<5, 1, 1>)),  // bnglr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<13, 3, 1>)),  // bsolr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<5, 3, 1>)),  // bnslr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<13, 3, 1>)),  // bunlr+
  LK_PSEUDO(GekkoMnemonic::kBclr, (fill_bo_bicond<5, 3, 1>)),  // bnulr+

  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<12, 0, 1>)),  // bltctr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<4, 1, 1>)),  // blectr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<12, 2, 1>)),  // beqctr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<4, 0, 1>)),  // bgectr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<12, 1, 1>)),  // bgtctr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<4, 0, 1>)),  // bnlctr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<4, 2, 1>)),  // bnectr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<4, 1, 1>)),  // bngctr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<12, 3, 1>)),  // bsoctr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<4, 3, 1>)),  // bnsctr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<12, 3, 1>)),  // bunctr
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<4, 3, 1>)),  // bnuctr

  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<13, 0, 1>)),  // bltctr+
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<5, 1, 1>)),  // blectr+
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<13, 2, 1>)),  // beqctr+
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<5, 0, 1>)),  // bgectr+
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<13, 1, 1>)),  // bgtctr+
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<5, 0, 1>)),  // bnlctr+
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<5, 2, 1>)),  // bnectr+
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<5, 1, 1>)),  // bngctr+
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<13, 3, 1>)),  // bsoctr+
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<5, 3, 1>)),  // bnsctr+
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<13, 3, 1>)),  // bunctr+
  LK_PSEUDO(GekkoMnemonic::kBcctr, (fill_bo_bicond<5, 3, 1>)),  // bnuctr+

  // E.6
  PLAIN_PSEUDO(GekkoMnemonic::kCreqv, [](OperandList& operands)
    {
      operands.Insert(1, operands[0]);
      operands.Insert(2, operands[0]);
    }),  // crset
  PLAIN_PSEUDO(GekkoMnemonic::kCrxor, [](OperandList& operands)
    {
      operands.Insert(1, operands[0]);
      operands.Insert(2, operands[0]);
    }),  // crclr
  PLAIN_PSEUDO(GekkoMnemonic::kCror, [](OperandList& operands)
    {
      operands.Insert(2, operands[1]);
    }),  // crmove
  PLAIN_PSEUDO(GekkoMnemonic::kCrnor, [](OperandList& operands)
    {
      operands.Insert(2, operands[1]);
    }),  // crnot

  // E.7
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<16>),  // twlt
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<16>),  // twlti
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<20>),  // twle
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<20>),  // twlei
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<4>),  // tweq
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<4>),  // tweqi
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<12>),  // twge
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<12>),  // twgei
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<8>),  // twgt
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<8>),  // twgti
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<12>),  // twnl
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<12>),  // twnli
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<24>),  // twne
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<24>),  // twnei
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<20>),  // twng
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<20>),  // twngi
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<2>),  // twllt
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<2>),  // twllti
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<6>),  // twlle
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<6>),  // twllei
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<5>),  // twlge
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<5>),  // twlgei
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<1>),  // twlgt
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<1>),  // twlgti
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<5>),  // twlnl
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<5>),  // twlnli
  PLAIN_PSEUDO(GekkoMnemonic::kTw, trap_set_TO<6>),  // twlng
  PLAIN_PSEUDO(GekkoMnemonic::kTwi, trap_set_TO<6>),  // twlngi
  PLAIN_PSEUDO(GekkoMnemonic::kTw, [](OperandList& operands)
    {
      operands.Insert(0, 31);
      operands.Insert(1, 0);
      operands.Insert(2, 0);
    }),  // trap

  // E.8
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr<spr_bitswap(1)>),  // mtxer
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr<spr_bitswap(1)>),  // mfxer
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr<spr_bitswap(8)>),  // mtlr
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr<spr_bitswap(8)>),  // mflr
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr<spr_bitswap(9)>),  // mtctr
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr<spr_bitswap(9)>),  // mfctr
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr<spr_bitswap(18)>),  // mtdsisr
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr<spr_bitswap(18)>),  // mfdsisr
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr<spr_bitswap(19)>),  // mtdar
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr<spr_bitswap(19)>),  // mfdar
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr<spr_bitswap(22)>),  // mtdec
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr<spr_bitswap(22)>),  // mfdec
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr<spr_bitswap(25)>),  // mtsdr1
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr<spr_bitswap(25)>),  // mfsdr1
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr<spr_bitswap(26)>),  // mtsrr0
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr<spr_bitswap(26)>),  // mfsrr0
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr<spr_bitswap(27)>),  // mtsrr1
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr<spr_bitswap(27)>),  // mfsrr1
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr<spr_bitswap(280)>),  // mtasr
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr<spr_bitswap(280)>),  // mfasr
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr<spr_bitswap(282)>),  // mtear
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr<spr_bitswap(282)>),  // mfear
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr<spr_bitswap(284)>),  // mttbl
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr<spr_bitswap(268)>),  // mftbl
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr<spr_bitswap(285)>),  // mttbu
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr<spr_bitswap(269)>),  // mftbu
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, [](OperandList& operands)
    {
      operands[0] = spr_bitswap(operands[0] + 272);
    }),  // mtsprg
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, [](OperandList& operands)
    {
      operands[1] = spr_bitswap(operands[1] + 272);
    }),  // mfsprg
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr_bat<spr_bitswap(528)>),  // mtibatu
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr_bat<spr_bitswap(528)>),  // mfibatu
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr_bat<spr_bitswap(529)>),  // mtibatl
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr_bat<spr_bitswap(529)>),  // mfibatl
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr_bat<spr_bitswap(536)>),  // mtdbatu
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr_bat<spr_bitswap(536)>),  // mfdbatu
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, fill_mtspr_bat<spr_bitswap(537)>),  // mtdbatl
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, fill_mfspr_bat<spr_bitswap(537)>),  // mfdbatl

  // E.9
  PLAIN_PSEUDO(GekkoMnemonic::kOri, [](OperandList& operands)
    {
      operands.Insert(0, 0);
      operands.Insert(1, 0);
      operands.Insert(2, 0);
    }),  // nop
  PLAIN_PSEUDO(GekkoMnemonic::kAddi, [](OperandList& operands)
    {
      operands.Insert(1, 0);
    }),  // li
  PLAIN_PSEUDO(GekkoMnemonic::kAddis, [](OperandList& operands)
    {
      operands.Insert(1, 0);
    }),  // lis
  PLAIN_PSEUDO(GekkoMnemonic::kAddi, [](OperandList& operands)
    {
      operands[2] ^= operands[1];
      operands[1] ^= operands[2];
      operands[2] ^= operands[1];
    }),  // la
  RC_PSEUDO(GekkoMnemonic::kOr, ([](OperandList& operands)
    {
      operands.Insert(2, operands[1]);
    })),  // mr
  RC_PSEUDO(GekkoMnemonic::kNor, ([](OperandList& operands)
    {
      operands.Insert(2, operands[1]);
    })),  // not
  PLAIN_PSEUDO(GekkoMnemonic::kMtcrf, [](OperandList& operands)
    {
      operands.Insert(0, 0xff);
    }),  // mtcr

  // Additional mnemonics
  PLAIN_PSEUDO(GekkoMnemonic::kMfspr_nobitswap, bitswap_idx<1>),  // mfspr
  PLAIN_PSEUDO(GekkoMnemonic::kMftb_nobitswap, bitswap_idx<1>),  // mfspr
  PLAIN_PSEUDO(GekkoMnemonic::kMtspr_nobitswap, bitswap_idx<0>),  // mtspr
};

#undef EMIT_MNEMONIC_ENTRY
#undef MNEMONIC
#undef BASIC_MNEMONIC
#undef RC_MNEMONIC
#undef OERC_MNEMONIC
#undef LK_MNEMONIC
#undef AALK_MNEMONIC
#undef PSEUDO
#undef PLAIN_PSEUDO
#undef RC_PSEUDO
#undef OERC_PSEUDO
#undef LK_PSEUDO
#undef LKAA_PSEUDO


//////////////////
// LEXER TABLES //
//////////////////


namespace
{
constexpr TransitionF _plus_or_minus = [](char c) { return c == '+' || c == '-'; };
constexpr TransitionF _digit = [](char c) -> bool { return std::isdigit(c); };
constexpr TransitionF _e = [](char c) { return c == 'e'; };
constexpr TransitionF _dot = [](char c) { return c == '.'; };

// Normal string characters
constexpr TransitionF _normal = [](char c) { return c != '\n' && c != '"' && c != '\\'; };
// Invalid characters in string
constexpr TransitionF _invalid = [](char c) { return c == '\n'; };
// Octal digits
constexpr TransitionF _octal = [](char c) { return c >= '0' && c <= '7'; };
// Hex digits
constexpr TransitionF _hex = [](char c) -> bool { return std::isxdigit(c); };
// Normal - octal
constexpr TransitionF _normal_minus_octal = [](char c) { return _normal(c) && !_octal(c); };
// Normal - hex
constexpr TransitionF _normal_minus_hex = [](char c) { return _normal(c) && !_hex(c); };
// Escape start
constexpr TransitionF _escape = [](char c) { return c == '\\'; };
// All single-character escapes
constexpr TransitionF _sce = [](char c) { return !_octal(c) && c != 'x' && c != '\n'; };
// Hex escape
constexpr TransitionF _hexstart = [](char c) { return c == 'x'; };
constexpr TransitionF _quote = [](char c) { return c == '"'; };
}  // namespace

std::vector<DfaNode> float_dfa =
{
  /* 0 */{{DfaEdge(_plus_or_minus, 1), DfaEdge(_digit, 2), DfaEdge(_dot, 5)}, "Invalid float: No numeric value"},

  /* 1 */{{DfaEdge(_digit, 2), DfaEdge(_dot, 5)}, "Invalid float: No numeric value"},

  /* 2 */{{DfaEdge(_digit, 2), DfaEdge(_dot, 3)}, std::nullopt},
  /* 3 */{{DfaEdge(_digit, 4)}, "Invalid float: No numeric value after decimal point"},
  /* 4 */{{DfaEdge(_digit, 4), DfaEdge(_e, 7)}, std::nullopt},

  /* 5 */{{DfaEdge(_digit, 6)}, "Invalid float: No numeric value after decimal point"},
  /* 6 */{{DfaEdge(_digit, 6), DfaEdge(_e, 7)}, std::nullopt},

  /* 7 */{{DfaEdge(_digit, 9), DfaEdge(_plus_or_minus, 8)},
          "Invalid float: No numeric value following exponent signifier"},
  /* 8 */{{DfaEdge(_digit, 9)}, "Invalid float: No numeric value following exponent signifier"},
  /* 9 */{{DfaEdge(_digit, 9)}, std::nullopt},
};

std::vector<DfaNode> string_dfa =
{
  // Base character check
  /* 0 */{{DfaEdge(_normal, 0), DfaEdge(_invalid, 1), DfaEdge(_quote, 2),
           DfaEdge(_escape, 3)}, "Invalid string: No terminating \""},

  // Invalid (unescaped newline)
  /* 1 */{{}, "Invalid string: No terminating \""},
  // String end
  /* 2 */{{}, std::nullopt},

  // Escape character breakout
  /* 3 */{{DfaEdge(_sce, 0), DfaEdge(_invalid, 1), DfaEdge(_octal, 4), DfaEdge(_hexstart, 6)},
          "Invalid string: No terminating \""},

  // Octal characters, at most 3
  /* 4 */{{DfaEdge(_normal_minus_octal, 0), DfaEdge(_invalid, 1), DfaEdge(_quote, 2),
           DfaEdge(_escape, 3), DfaEdge(_octal, 5)}, "Invalid string: No terminating \""},
  /* 5 */{{DfaEdge(_normal, 0), DfaEdge(_invalid, 1), DfaEdge(_quote, 2),
           DfaEdge(_escape, 3)}, "Invalid string: No terminating \""},

  // Hex characters, 1 or more
  /* 6 */{{DfaEdge(_hex, 7)}, "Invalid string: bad hex escape"},
  /* 7 */{{DfaEdge(_normal_minus_hex, 0), DfaEdge(_invalid, 1), DfaEdge(_quote, 2),
           DfaEdge(_escape, 3), DfaEdge(_hex, 7)}, "Invalid string: No terminating \""},
};
}  // namespace Common::GekkoAssembler::detail
