#include "armrx/superscalar.hpp"
#include "armrx/instruction.hpp"
#include "armrx/blake2_generator.hpp"
#include "armrx/randomx_config.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace armrx {

constexpr bool trace = false;

static bool isMultiplication(SuperscalarInstructionType type) {
    return type == SuperscalarInstructionType::IMUL_R ||
           type == SuperscalarInstructionType::IMULH_R ||
           type == SuperscalarInstructionType::ISMULH_R ||
           type == SuperscalarInstructionType::IMUL_RCP;
}

namespace ExecutionPort {
    using type = int;
    constexpr type Null = 0;
    constexpr type P0 = 1;
    constexpr type P1 = 2;
    constexpr type P5 = 4;
    constexpr type P01 = P0 | P1;
    constexpr type P05 = P0 | P5;
    constexpr type P015 = P0 | P1 | P5;
}

class MacroOp {
public:
    MacroOp(const char* name, int size)
        : name_(name), size_(size), latency_(0), uop1_(ExecutionPort::Null), uop2_(ExecutionPort::Null) {}
    MacroOp(const char* name, int size, int latency, ExecutionPort::type uop)
        : name_(name), size_(size), latency_(latency), uop1_(uop), uop2_(ExecutionPort::Null) {}
    MacroOp(const char* name, int size, int latency, ExecutionPort::type uop1, ExecutionPort::type uop2)
        : name_(name), size_(size), latency_(latency), uop1_(uop1), uop2_(uop2) {}
    MacroOp(const MacroOp& parent, bool dependent)
        : name_(parent.name_), size_(parent.size_), latency_(parent.latency_), uop1_(parent.uop1_), uop2_(parent.uop2_), dependent_(dependent) {}
    
    [[nodiscard]] const char* getName() const { return name_; }
    [[nodiscard]] int getSize() const { return size_; }
    [[nodiscard]] int getLatency() const { return latency_; }
    [[nodiscard]] ExecutionPort::type getUop1() const { return uop1_; }
    [[nodiscard]] ExecutionPort::type getUop2() const { return uop2_; }
    [[nodiscard]] bool isSimple() const { return uop2_ == ExecutionPort::Null; }
    [[nodiscard]] bool isEliminated() const { return uop1_ == ExecutionPort::Null; }
    [[nodiscard]] bool isDependent() const { return dependent_; }

    static const MacroOp Add_rr;
    static const MacroOp Add_ri;
    static const MacroOp Lea_sib;
    static const MacroOp Sub_rr;
    static const MacroOp Imul_rr;
    static const MacroOp Imul_r;
    static const MacroOp Mul_r;
    static const MacroOp Mov_rr;
    static const MacroOp Mov_ri64;
    static const MacroOp Xor_rr;
    static const MacroOp Xor_ri;
    static const MacroOp Ror_rcl;
    static const MacroOp Ror_ri;
    static const MacroOp TestJz_fused;
    static const MacroOp Xor_self;
    static const MacroOp Cmp_ri;
    static const MacroOp Setcc_r;

private:
    const char* name_;
    int size_;
    int latency_;
    ExecutionPort::type uop1_;
    ExecutionPort::type uop2_;
    bool dependent_ = false;
};

const MacroOp MacroOp::Add_rr = MacroOp("add r,r", 3, 1, ExecutionPort::P015);
const MacroOp MacroOp::Sub_rr = MacroOp("sub r,r", 3, 1, ExecutionPort::P015);
const MacroOp MacroOp::Xor_rr = MacroOp("xor r,r", 3, 1, ExecutionPort::P015);
const MacroOp MacroOp::Imul_r = MacroOp("imul r", 3, 4, ExecutionPort::P1, ExecutionPort::P5);
const MacroOp MacroOp::Mul_r = MacroOp("mul r", 3, 4, ExecutionPort::P1, ExecutionPort::P5);
const MacroOp MacroOp::Mov_rr = MacroOp("mov r,r", 3);

const MacroOp MacroOp::Lea_sib = MacroOp("lea r,r+r*s", 4, 1, ExecutionPort::P01);
const MacroOp MacroOp::Imul_rr = MacroOp("imul r,r", 4, 3, ExecutionPort::P1);
const MacroOp MacroOp::Ror_ri = MacroOp("ror r,i", 4, 1, ExecutionPort::P05);

const MacroOp MacroOp::Add_ri = MacroOp("add r,i", 7, 1, ExecutionPort::P015);
const MacroOp MacroOp::Xor_ri = MacroOp("xor r,i", 7, 1, ExecutionPort::P015);

const MacroOp MacroOp::Mov_ri64 = MacroOp("mov rax,i64", 10, 1, ExecutionPort::P015);

// Unused:
const MacroOp MacroOp::Ror_rcl = MacroOp("ror r,cl", 3, 1, ExecutionPort::P0, ExecutionPort::P5);
const MacroOp MacroOp::Xor_self = MacroOp("xor rcx,rcx", 3);
const MacroOp MacroOp::Cmp_ri = MacroOp("cmp r,i", 7, 1, ExecutionPort::P015);
const MacroOp MacroOp::Setcc_r = MacroOp("setcc cl", 3, 1, ExecutionPort::P05);
const MacroOp MacroOp::TestJz_fused = MacroOp("testjz r,i", 13, 0, ExecutionPort::P5);

const MacroOp IMULH_R_ops_array[] = { MacroOp::Mov_rr, MacroOp(MacroOp::Mul_r, true), MacroOp::Mov_rr };
const MacroOp ISMULH_R_ops_array[] = { MacroOp::Mov_rr, MacroOp(MacroOp::Imul_r, true), MacroOp::Mov_rr };
const MacroOp IMUL_RCP_ops_array[] = { MacroOp::Mov_ri64, MacroOp(MacroOp::Imul_rr, true) };

class SuperscalarInstructionInfo {
public:
    [[nodiscard]] const char* getName() const { return name_; }
    [[nodiscard]] int getSize() const { return static_cast<int>(ops_.size()); }
    [[nodiscard]] bool isSimple() const { return getSize() == 1; }
    [[nodiscard]] int getLatency() const { return latency_; }
    [[nodiscard]] const MacroOp& getOp(int index) const { return ops_[static_cast<std::size_t>(index)]; }
    [[nodiscard]] SuperscalarInstructionType getType() const { return type_; }
    [[nodiscard]] int getResultOp() const { return resultOp_; }
    [[nodiscard]] int getDstOp() const { return dstOp_; }
    [[nodiscard]] int getSrcOp() const { return srcOp_; }

    static const SuperscalarInstructionInfo ISUB_R;
    static const SuperscalarInstructionInfo IXOR_R;
    static const SuperscalarInstructionInfo IADD_RS;
    static const SuperscalarInstructionInfo IMUL_R;
    static const SuperscalarInstructionInfo IROR_C;
    static const SuperscalarInstructionInfo IADD_C7;
    static const SuperscalarInstructionInfo IXOR_C7;
    static const SuperscalarInstructionInfo IADD_C8;
    static const SuperscalarInstructionInfo IXOR_C8;
    static const SuperscalarInstructionInfo IADD_C9;
    static const SuperscalarInstructionInfo IXOR_C9;
    static const SuperscalarInstructionInfo IMULH_R;
    static const SuperscalarInstructionInfo ISMULH_R;
    static const SuperscalarInstructionInfo IMUL_RCP;
    static const SuperscalarInstructionInfo NOP;

private:
    const char* name_;
    SuperscalarInstructionType type_;
    std::vector<MacroOp> ops_;
    int latency_;
    int resultOp_ = 0;
    int dstOp_ = 0;
    int srcOp_;

    SuperscalarInstructionInfo(const char* name)
        : name_(name), type_(SuperscalarInstructionType::INVALID), latency_(0), srcOp_(-1) {}
    SuperscalarInstructionInfo(const char* name, SuperscalarInstructionType type, const MacroOp& op, int srcOp)
        : name_(name), type_(type), latency_(op.getLatency()), srcOp_(srcOp) {
        ops_.push_back(MacroOp(op));
    }
    template <std::size_t N>
    SuperscalarInstructionInfo(const char* name, SuperscalarInstructionType type, const MacroOp(&arr)[N], int resultOp, int dstOp, int srcOp)
        : name_(name), type_(type), latency_(0), resultOp_(resultOp), dstOp_(dstOp), srcOp_(srcOp) {
        for (unsigned i = 0; i < N; ++i) {
            ops_.push_back(MacroOp(arr[i]));
            latency_ += ops_.back().getLatency();
        }
        static_assert(N > 1, "Invalid array size");
    }
};

const SuperscalarInstructionInfo SuperscalarInstructionInfo::ISUB_R = SuperscalarInstructionInfo("ISUB_R", SuperscalarInstructionType::ISUB_R, MacroOp::Sub_rr, 0);
const SuperscalarInstructionInfo SuperscalarInstructionInfo::IXOR_R = SuperscalarInstructionInfo("IXOR_R", SuperscalarInstructionType::IXOR_R, MacroOp::Xor_rr, 0);
const SuperscalarInstructionInfo SuperscalarInstructionInfo::IADD_RS = SuperscalarInstructionInfo("IADD_RS", SuperscalarInstructionType::IADD_RS, MacroOp::Lea_sib, 0);
const SuperscalarInstructionInfo SuperscalarInstructionInfo::IMUL_R = SuperscalarInstructionInfo("IMUL_R", SuperscalarInstructionType::IMUL_R, MacroOp::Imul_rr, 0);
const SuperscalarInstructionInfo SuperscalarInstructionInfo::IROR_C = SuperscalarInstructionInfo("IROR_C", SuperscalarInstructionType::IROR_C, MacroOp::Ror_ri, -1);

const SuperscalarInstructionInfo SuperscalarInstructionInfo::IADD_C7 = SuperscalarInstructionInfo("IADD_C7", SuperscalarInstructionType::IADD_C7, MacroOp::Add_ri, -1);
const SuperscalarInstructionInfo SuperscalarInstructionInfo::IXOR_C7 = SuperscalarInstructionInfo("IXOR_C7", SuperscalarInstructionType::IXOR_C7, MacroOp::Xor_ri, -1);
const SuperscalarInstructionInfo SuperscalarInstructionInfo::IADD_C8 = SuperscalarInstructionInfo("IADD_C8", SuperscalarInstructionType::IADD_C8, MacroOp::Add_ri, -1);
const SuperscalarInstructionInfo SuperscalarInstructionInfo::IXOR_C8 = SuperscalarInstructionInfo("IXOR_C8", SuperscalarInstructionType::IXOR_C8, MacroOp::Xor_ri, -1);
const SuperscalarInstructionInfo SuperscalarInstructionInfo::IADD_C9 = SuperscalarInstructionInfo("IADD_C9", SuperscalarInstructionType::IADD_C9, MacroOp::Add_ri, -1);
const SuperscalarInstructionInfo SuperscalarInstructionInfo::IXOR_C9 = SuperscalarInstructionInfo("IXOR_C9", SuperscalarInstructionType::IXOR_C9, MacroOp::Xor_ri, -1);

const SuperscalarInstructionInfo SuperscalarInstructionInfo::IMULH_R = SuperscalarInstructionInfo("IMULH_R", SuperscalarInstructionType::IMULH_R, IMULH_R_ops_array, 1, 0, 1);
const SuperscalarInstructionInfo SuperscalarInstructionInfo::ISMULH_R = SuperscalarInstructionInfo("ISMULH_R", SuperscalarInstructionType::ISMULH_R, ISMULH_R_ops_array, 1, 0, 1);
const SuperscalarInstructionInfo SuperscalarInstructionInfo::IMUL_RCP = SuperscalarInstructionInfo("IMUL_RCP", SuperscalarInstructionType::IMUL_RCP, IMUL_RCP_ops_array, 1, 1, -1);

const SuperscalarInstructionInfo SuperscalarInstructionInfo::NOP = SuperscalarInstructionInfo("NOP");

const int buffer0[] = { 4, 8, 4 };
const int buffer1[] = { 7, 3, 3, 3 };
const int buffer2[] = { 3, 7, 3, 3 };
const int buffer3[] = { 4, 9, 3 };
const int buffer4[] = { 4, 4, 4, 4 };
const int buffer5[] = { 3, 3, 10 };

class DecoderBuffer {
public:
    static const DecoderBuffer Default;
    template <std::size_t N>
    DecoderBuffer(const char* name, int index, const int(&arr)[N])
        : name_(name), index_(index), counts_(arr), opsCount_(static_cast<int>(N)) {}
    
    [[nodiscard]] const int* getCounts() const { return counts_; }
    [[nodiscard]] int getSize() const { return opsCount_; }
    [[nodiscard]] int getIndex() const { return index_; }
    [[nodiscard]] const char* getName() const { return name_; }

    [[nodiscard]] const DecoderBuffer* fetchNext(SuperscalarInstructionType instrType, int cycle, int mulCount, Blake2Generator& gen) const {
        if (instrType == SuperscalarInstructionType::IMULH_R || instrType == SuperscalarInstructionType::ISMULH_R)
            return &decodeBuffer3310;

        if (mulCount < cycle + 1)
            return &decodeBuffer4444;

        if (instrType == SuperscalarInstructionType::IMUL_RCP)
            return (gen.get_byte() & 1) ? &decodeBuffer484 : &decodeBuffer493;

        return fetchNextDefault(gen);
    }

private:
    const char* name_;
    int index_;
    const int* counts_;
    int opsCount_;
    DecoderBuffer() : name_("default"), index_(-1), counts_(nullptr), opsCount_(0) {}
    static const DecoderBuffer decodeBuffer484;
    static const DecoderBuffer decodeBuffer7333;
    static const DecoderBuffer decodeBuffer3733;
    static const DecoderBuffer decodeBuffer493;
    static const DecoderBuffer decodeBuffer4444;
    static const DecoderBuffer decodeBuffer3310;
    static const DecoderBuffer* decodeBuffers[4];
    
    [[nodiscard]] const DecoderBuffer* fetchNextDefault(Blake2Generator& gen) const {
        return decodeBuffers[gen.get_byte() & 3];
    }
};

const DecoderBuffer DecoderBuffer::decodeBuffer484 = DecoderBuffer("4,8,4", 0, buffer0);
const DecoderBuffer DecoderBuffer::decodeBuffer7333 = DecoderBuffer("7,3,3,3", 1, buffer1);
const DecoderBuffer DecoderBuffer::decodeBuffer3733 = DecoderBuffer("3,7,3,3", 2, buffer2);
const DecoderBuffer DecoderBuffer::decodeBuffer493 = DecoderBuffer("4,9,3", 3, buffer3);
const DecoderBuffer DecoderBuffer::decodeBuffer4444 = DecoderBuffer("4,4,4,4", 4, buffer4);
const DecoderBuffer DecoderBuffer::decodeBuffer3310 = DecoderBuffer("3,3,10", 5, buffer5);

const DecoderBuffer* DecoderBuffer::decodeBuffers[4] = {
    &DecoderBuffer::decodeBuffer484,
    &DecoderBuffer::decodeBuffer7333,
    &DecoderBuffer::decodeBuffer3733,
    &DecoderBuffer::decodeBuffer493,
};

const DecoderBuffer DecoderBuffer::Default = DecoderBuffer();

const SuperscalarInstructionInfo* slot_3[]  = { &SuperscalarInstructionInfo::ISUB_R, &SuperscalarInstructionInfo::IXOR_R };
const SuperscalarInstructionInfo* slot_3L[] = { &SuperscalarInstructionInfo::ISUB_R, &SuperscalarInstructionInfo::IXOR_R, &SuperscalarInstructionInfo::IMULH_R, &SuperscalarInstructionInfo::ISMULH_R };
const SuperscalarInstructionInfo* slot_4[]  = { &SuperscalarInstructionInfo::IROR_C, &SuperscalarInstructionInfo::IADD_RS };
const SuperscalarInstructionInfo* slot_7[]  = { &SuperscalarInstructionInfo::IXOR_C7, &SuperscalarInstructionInfo::IADD_C7 };
const SuperscalarInstructionInfo* slot_8[] = { &SuperscalarInstructionInfo::IXOR_C8, &SuperscalarInstructionInfo::IADD_C8 };
const SuperscalarInstructionInfo* slot_9[] = { &SuperscalarInstructionInfo::IXOR_C9, &SuperscalarInstructionInfo::IADD_C9 };
const SuperscalarInstructionInfo* slot_10   = &SuperscalarInstructionInfo::IMUL_RCP;

static bool selectRegister(int* availableRegisters, int numAvailable, Blake2Generator& gen, int& reg) {
    if (numAvailable == 0)
        return false;

    int index;
    if (numAvailable > 1) {
        index = static_cast<int>(gen.get_uint32() % numAvailable);
    }
    else {
        index = 0;
    }
    reg = availableRegisters[index];
    return true;
}

class RegisterInfo {
public:
    RegisterInfo() : latency(0), lastOpGroup(SuperscalarInstructionType::INVALID), lastOpPar(-1), value(0) {}
    int latency;
    SuperscalarInstructionType lastOpGroup;
    int lastOpPar;
    int value;
};

constexpr int RegisterNeedsDisplacement = 5;

static bool isZeroOrPowerOf2(std::uint32_t val) {
    return val == 0 || (val & (val - 1)) == 0;
}

class SuperscalarInstruction {
public:
    void toInstr(Instruction& instr) {
        instr.opcode = static_cast<std::uint8_t>(getType());
        instr.dst = static_cast<std::uint8_t>(dst_);
        instr.src = static_cast<std::uint8_t>(src_ >= 0 ? src_ : dst_);
        instr.setMod(static_cast<std::uint8_t>(mod_));
        instr.setImm32(imm32_);
    }

    void createForSlot(Blake2Generator& gen, int slotSize, int fetchType, bool isLast, bool isFirst) {
        (void)isFirst;
        switch (slotSize)
        {
        case 3:
            if (isLast) {
                create(slot_3L[gen.get_byte() & 3], gen);
            }
            else {
                create(slot_3[gen.get_byte() & 1], gen);
            }
            break;
        case 4:
            if (fetchType == 4 && !isLast) {
                create(&SuperscalarInstructionInfo::IMUL_R, gen);
            }
            else {
                create(slot_4[gen.get_byte() & 1], gen);
            }
            break;
        case 7:
            create(slot_7[gen.get_byte() & 1], gen);
            break;
        case 8:
            create(slot_8[gen.get_byte() & 1], gen);
            break;
        case 9:
            create(slot_9[gen.get_byte() & 1], gen);
            break;
        case 10:
            create(slot_10, gen);
            break;
        default:
            throw std::runtime_error("invalid superscalar slot size");
        }
    }

    void create(const SuperscalarInstructionInfo* info, Blake2Generator& gen) {
        info_ = info;
        reset();
        switch (info->getType())
        {
        case SuperscalarInstructionType::ISUB_R: {
            mod_ = 0;
            imm32_ = 0;
            opGroup_ = SuperscalarInstructionType::IADD_RS;
            groupParIsSource_ = true;
        } break;

        case SuperscalarInstructionType::IXOR_R: {
            mod_ = 0;
            imm32_ = 0;
            opGroup_ = SuperscalarInstructionType::IXOR_R;
            groupParIsSource_ = true;
        } break;

        case SuperscalarInstructionType::IADD_RS: {
            mod_ = gen.get_byte();
            imm32_ = 0;
            opGroup_ = SuperscalarInstructionType::IADD_RS;
            groupParIsSource_ = true;
        } break;

        case SuperscalarInstructionType::IMUL_R: {
            mod_ = 0;
            imm32_ = 0;
            opGroup_ = SuperscalarInstructionType::IMUL_R;
            groupParIsSource_ = true;
        } break;

        case SuperscalarInstructionType::IROR_C: {
            mod_ = 0;
            do {
                imm32_ = gen.get_byte() & 63;
            } while (imm32_ == 0);
            opGroup_ = SuperscalarInstructionType::IROR_C;
            opGroupPar_ = -1;
        } break;

        case SuperscalarInstructionType::IADD_C7:
        case SuperscalarInstructionType::IADD_C8:
        case SuperscalarInstructionType::IADD_C9: {
            mod_ = 0;
            imm32_ = gen.get_uint32();
            opGroup_ = SuperscalarInstructionType::IADD_C7;
            opGroupPar_ = -1;
        } break;

        case SuperscalarInstructionType::IXOR_C7:
        case SuperscalarInstructionType::IXOR_C8:
        case SuperscalarInstructionType::IXOR_C9: {
            mod_ = 0;
            imm32_ = gen.get_uint32();
            opGroup_ = SuperscalarInstructionType::IXOR_C7;
            opGroupPar_ = -1;
        } break;

        case SuperscalarInstructionType::IMULH_R: {
            canReuse_ = true;
            mod_ = 0;
            imm32_ = 0;
            opGroup_ = SuperscalarInstructionType::IMULH_R;
            opGroupPar_ = static_cast<int>(gen.get_uint32());
        } break;

        case SuperscalarInstructionType::ISMULH_R: {
            canReuse_ = true;
            mod_ = 0;
            imm32_ = 0;
            opGroup_ = SuperscalarInstructionType::ISMULH_R;
            opGroupPar_ = static_cast<int>(gen.get_uint32());
        } break;

        case SuperscalarInstructionType::IMUL_RCP: {
            mod_ = 0;
            do {
                imm32_ = gen.get_uint32();
            } while (isZeroOrPowerOf2(imm32_));
            opGroup_ = SuperscalarInstructionType::IMUL_RCP;
            opGroupPar_ = -1;
        } break;

        default:
            break;
        }
    }

    bool selectDestination(int cycle, bool allowChainedMul, RegisterInfo (&registers)[8], Blake2Generator& gen) {
        int availableRegisters[8];
        int numAvailable = 0;
        for (int i = 0; i < 8; ++i) {
            if (registers[i].latency <= cycle && (canReuse_ || i != src_) &&
                (allowChainedMul || opGroup_ != SuperscalarInstructionType::IMUL_R || registers[i].lastOpGroup != SuperscalarInstructionType::IMUL_R) &&
                (registers[i].lastOpGroup != opGroup_ || registers[i].lastOpPar != opGroupPar_) &&
                (info_->getType() != SuperscalarInstructionType::IADD_RS || i != RegisterNeedsDisplacement)) {
                availableRegisters[numAvailable++] = i;
            }
        }
        return selectRegister(availableRegisters, numAvailable, gen, dst_);
    }

    bool selectSource(int cycle, RegisterInfo(&registers)[8], Blake2Generator& gen) {
        int availableRegisters[8];
        int numAvailable = 0;
        for (int i = 0; i < 8; ++i) {
            if (registers[i].latency <= cycle)
                availableRegisters[numAvailable++] = i;
        }
        if (numAvailable == 2 && info_->getType() == SuperscalarInstructionType::IADD_RS) {
            if (availableRegisters[0] == RegisterNeedsDisplacement || availableRegisters[1] == RegisterNeedsDisplacement) {
                opGroupPar_ = src_ = RegisterNeedsDisplacement;
                return true;
            }
        }
        if (selectRegister(availableRegisters, numAvailable, gen, src_)) {
            if (groupParIsSource_)
                opGroupPar_ = src_;
            return true;
        }
        return false;
    }

    SuperscalarInstructionType getType() const { return info_->getType(); }
    int getSource() const { return src_; }
    int getDestination() const { return dst_; }
    SuperscalarInstructionType getGroup() const { return opGroup_; }
    int getGroupPar() const { return opGroupPar_; }
    const SuperscalarInstructionInfo& getInfo() const { return *info_; }

    static const SuperscalarInstruction Null;

private:
    const SuperscalarInstructionInfo* info_;
    int src_ = -1;
    int dst_ = -1;
    int mod_ = 0;
    std::uint32_t imm32_ = 0;
    SuperscalarInstructionType opGroup_ = SuperscalarInstructionType::INVALID;
    int opGroupPar_ = -1;
    bool canReuse_ = false;
    bool groupParIsSource_ = false;

    void reset() {
        src_ = dst_ = -1;
        canReuse_ = groupParIsSource_ = false;
    }

    explicit SuperscalarInstruction(const SuperscalarInstructionInfo* info) : info_(info) {}
};

const SuperscalarInstruction SuperscalarInstruction::Null = SuperscalarInstruction(&SuperscalarInstructionInfo::NOP);

constexpr int CYCLE_MAP_SIZE = kSuperscalarLatency + 4;
constexpr int LOOK_FORWARD_CYCLES = 4;
constexpr int MAX_THROWAWAY_COUNT = 256;

template<bool commit>
static int scheduleUop(ExecutionPort::type uop, ExecutionPort::type(&portBusy)[CYCLE_MAP_SIZE][3], int cycle) {
    for (; cycle < CYCLE_MAP_SIZE; ++cycle) {
        if ((uop & ExecutionPort::P5) != 0 && !portBusy[cycle][2]) {
            if (commit) {
                portBusy[cycle][2] = uop;
            }
            return cycle;
        }
        if ((uop & ExecutionPort::P0) != 0 && !portBusy[cycle][0]) {
            if (commit) {
                portBusy[cycle][0] = uop;
            }
            return cycle;
        }
        if ((uop & ExecutionPort::P1) != 0 && !portBusy[cycle][1]) {
            if (commit) {
                portBusy[cycle][1] = uop;
            }
            return cycle;
        }
    }
    return -1;
}

template<bool commit>
static int scheduleMop(const MacroOp& mop, ExecutionPort::type(&portBusy)[CYCLE_MAP_SIZE][3], int cycle, int depCycle) {
    if (mop.isDependent()) {
        cycle = std::max(cycle, depCycle);
    }
    if (mop.isEliminated()) {
        return cycle;
    } 
    else if (mop.isSimple()) {
        return scheduleUop<commit>(mop.getUop1(), portBusy, cycle);
    }
    else {
        for (; cycle < CYCLE_MAP_SIZE; ++cycle) {
            int cycle1 = scheduleUop<false>(mop.getUop1(), portBusy, cycle);
            int cycle2 = scheduleUop<false>(mop.getUop2(), portBusy, cycle);

            if (cycle1 >= 0 && cycle1 == cycle2) {
                if (commit) {
                    scheduleUop<true>(mop.getUop1(), portBusy, cycle1);
                    scheduleUop<true>(mop.getUop2(), portBusy, cycle2);
                }
                return cycle1;
            }
        }
    }
    return -1;
}

void generate_superscalar(SuperscalarProgram& prog, Blake2Generator& gen) {
    ExecutionPort::type portBusy[CYCLE_MAP_SIZE][3];
    std::memset(portBusy, 0, sizeof(portBusy));
    RegisterInfo registers[8];

    const DecoderBuffer* decodeBuffer = &DecoderBuffer::Default;
    SuperscalarInstruction currentInstruction = SuperscalarInstruction::Null;
    int macroOpIndex = 0;
    int codeSize = 0;
    int macroOpCount = 0;
    int cycle = 0;
    int depCycle = 0;
    int retireCycle = 0;
    bool portsSaturated = false;
    int programSize = 0;
    int mulCount = 0;
    int decodeCycle;
    int throwAwayCount = 0;

    for (decodeCycle = 0; decodeCycle < static_cast<int>(kSuperscalarLatency) && !portsSaturated && programSize < static_cast<int>(kSuperscalarMaxSize); ++decodeCycle) {
        decodeBuffer = decodeBuffer->fetchNext(currentInstruction.getType(), decodeCycle, mulCount, gen);
        int bufferIndex = 0;
        
        while (bufferIndex < decodeBuffer->getSize()) {
            int topCycle = cycle;

            if (macroOpIndex >= currentInstruction.getInfo().getSize()) {
                if (portsSaturated || programSize >= static_cast<int>(kSuperscalarMaxSize))
                    break;
                currentInstruction.createForSlot(gen, decodeBuffer->getCounts()[bufferIndex], decodeBuffer->getIndex(), decodeBuffer->getSize() == bufferIndex + 1, bufferIndex == 0);
                macroOpIndex = 0;
            }
            const MacroOp& mop = currentInstruction.getInfo().getOp(macroOpIndex);

            int scheduleCycle = scheduleMop<false>(mop, portBusy, cycle, depCycle);
            if (scheduleCycle < 0) {
                portsSaturated = true;
                break;
            }

            if (macroOpIndex == currentInstruction.getInfo().getSrcOp()) {
                int forward;
                for (forward = 0; forward < LOOK_FORWARD_CYCLES && !currentInstruction.selectSource(scheduleCycle, registers, gen); ++forward) {
                    ++scheduleCycle;
                    ++cycle;
                }
                if (forward == LOOK_FORWARD_CYCLES) {
                    if (throwAwayCount < MAX_THROWAWAY_COUNT) {
                        throwAwayCount++;
                        macroOpIndex = currentInstruction.getInfo().getSize();
                        continue;
                    }
                    currentInstruction = SuperscalarInstruction::Null;
                    break;
                }
            }
            if (macroOpIndex == currentInstruction.getInfo().getDstOp()) {
                int forward;
                for (forward = 0; forward < LOOK_FORWARD_CYCLES && !currentInstruction.selectDestination(scheduleCycle, throwAwayCount > 0, registers, gen); ++forward) {
                    ++scheduleCycle;
                    ++cycle;
                }
                if (forward == LOOK_FORWARD_CYCLES) {
                    if (throwAwayCount < MAX_THROWAWAY_COUNT) {
                        throwAwayCount++;
                        macroOpIndex = currentInstruction.getInfo().getSize();
                        continue;
                    }
                    currentInstruction = SuperscalarInstruction::Null;
                    break;
                }
            }
            throwAwayCount = 0;

            scheduleCycle = scheduleMop<true>(mop, portBusy, scheduleCycle, scheduleCycle);

            if (scheduleCycle < 0) {
                portsSaturated = true;
                break;
            }

            depCycle = scheduleCycle + mop.getLatency();

            if (macroOpIndex == currentInstruction.getInfo().getResultOp()) {
                int dst = currentInstruction.getDestination();
                RegisterInfo& ri = registers[dst];
                retireCycle = depCycle;
                ri.latency = retireCycle;
                ri.lastOpGroup = currentInstruction.getGroup();
                ri.lastOpPar = currentInstruction.getGroupPar();
            }
            codeSize += mop.getSize();
            bufferIndex++;
            macroOpIndex++;
            macroOpCount++;

            if (scheduleCycle >= static_cast<int>(kSuperscalarLatency)) {
                portsSaturated = true;
            }
            cycle = topCycle;

            if (macroOpIndex >= currentInstruction.getInfo().getSize()) {
                currentInstruction.toInstr(prog(static_cast<std::size_t>(programSize++)));
                mulCount += isMultiplication(currentInstruction.getType());
            }
        }
        ++cycle;
    }

    double ipc = (retireCycle > 0) ? (macroOpCount / static_cast<double>(retireCycle)) : 0.0;

    prog.asic_latencies.fill(0);

    for (int i = 0; i < programSize; ++i) {
        Instruction& instr = prog(static_cast<std::size_t>(i));
        int latDst = prog.asic_latencies[instr.dst] + 1;
        int latSrc = instr.dst != instr.src ? prog.asic_latencies[instr.src] + 1 : 0;
        prog.asic_latencies[instr.dst] = std::max(latDst, latSrc);
    }

    int asicLatencyMax = 0;
    int addressReg = 0;
    for (int i = 0; i < 8; ++i) {
        if (prog.asic_latencies[static_cast<std::size_t>(i)] > asicLatencyMax) {
            asicLatencyMax = prog.asic_latencies[static_cast<std::size_t>(i)];
            addressReg = i;
        }
        prog.cpu_latencies[static_cast<std::size_t>(i)] = registers[i].latency;
    }

    prog.set_size(static_cast<std::uint32_t>(programSize));
    prog.set_address_register(addressReg);

    prog.cpu_latency = retireCycle;
    prog.asic_latency = asicLatencyMax;
    prog.code_size = codeSize;
    prog.macro_ops = macroOpCount;
    prog.decode_cycles = decodeCycle;
    prog.ipc = ipc;
    prog.mul_count = mulCount;
}

static inline std::uint64_t signExtend2sCompl(std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

static inline std::uint64_t mulh(std::uint64_t a, std::uint64_t b) {
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b)) >> 64);
}

static inline std::uint64_t smulh(std::uint64_t a, std::uint64_t b) {
    return static_cast<std::uint64_t>((static_cast<__int128>(static_cast<std::int64_t>(a)) *
                                       static_cast<__int128>(static_cast<std::int64_t>(b))) >> 64);
}

uint64_t randomx_reciprocal(uint32_t divisor) {
    assert(divisor != 0);

    const uint64_t p2exp63 = 1ULL << 63;
    const uint64_t q = p2exp63 / divisor;
    const uint64_t r = p2exp63 % divisor;

#ifdef __GNUC__
    const uint32_t shift = 64 - static_cast<uint32_t>(__builtin_clzll(divisor));
#else
    uint32_t shift = 32;
    for (uint32_t k = 1U << 31; (k & divisor) == 0; k >>= 1)
        --shift;
#endif

    return (q << shift) + ((r << shift) / divisor);
}

void execute_superscalar(std::array<std::uint64_t, 8>& r, const SuperscalarProgram& prog,
                         const std::vector<std::uint64_t>* reciprocals) {
    for (std::uint32_t j = 0; j < prog.size(); ++j) {
        const Instruction& instr = prog(j);
        switch (static_cast<SuperscalarInstructionType>(instr.opcode))
        {
        case SuperscalarInstructionType::ISUB_R:
            r[instr.dst] -= r[instr.src];
            break;
        case SuperscalarInstructionType::IXOR_R:
            r[instr.dst] ^= r[instr.src];
            break;
        case SuperscalarInstructionType::IADD_RS:
            r[instr.dst] += r[instr.src] << instr.getModShift();
            break;
        case SuperscalarInstructionType::IMUL_R:
            r[instr.dst] *= r[instr.src];
            break;
        case SuperscalarInstructionType::IROR_C:
            r[instr.dst] = std::rotr(r[instr.dst], static_cast<int>(instr.getImm32()));
            break;
        case SuperscalarInstructionType::IADD_C7:
        case SuperscalarInstructionType::IADD_C8:
        case SuperscalarInstructionType::IADD_C9:
            r[instr.dst] += signExtend2sCompl(instr.getImm32());
            break;
        case SuperscalarInstructionType::IXOR_C7:
        case SuperscalarInstructionType::IXOR_C8:
        case SuperscalarInstructionType::IXOR_C9:
            r[instr.dst] ^= signExtend2sCompl(instr.getImm32());
            break;
        case SuperscalarInstructionType::IMULH_R:
            r[instr.dst] = mulh(r[instr.dst], r[instr.src]);
            break;
        case SuperscalarInstructionType::ISMULH_R:
            r[instr.dst] = smulh(r[instr.dst], r[instr.src]);
            break;
        case SuperscalarInstructionType::IMUL_RCP:
            if (reciprocals != nullptr) {
                r[instr.dst] *= (*reciprocals)[instr.getImm32()];
            } else {
                r[instr.dst] *= randomx_reciprocal(instr.getImm32());
            }
            break;
        default:
            throw std::runtime_error("unreachable instruction opcode in execute_superscalar");
        }
    }
}

#ifdef __aarch64__
void execute_superscalar_neon(uint64x2_t vr[8], const SuperscalarProgram& prog,
                              const std::vector<std::uint64_t>* reciprocals) {
    for (std::uint32_t j = 0; j < prog.size(); ++j) {
        const Instruction& instr = prog(j);
        switch (static_cast<SuperscalarInstructionType>(instr.opcode))
        {
        case SuperscalarInstructionType::ISUB_R:
            vr[instr.dst] = vsubq_u64(vr[instr.dst], vr[instr.src]);
            break;
        case SuperscalarInstructionType::IXOR_R:
            vr[instr.dst] = veorq_u64(vr[instr.dst], vr[instr.src]);
            break;
        case SuperscalarInstructionType::IADD_RS: {
            uint64x2_t shifted;
            switch (instr.getModShift()) {
                case 0: shifted = vr[instr.src]; break;
                case 1: shifted = vshlq_n_u64(vr[instr.src], 1); break;
                case 2: shifted = vshlq_n_u64(vr[instr.src], 2); break;
                case 3: shifted = vshlq_n_u64(vr[instr.src], 3); break;
            }
            vr[instr.dst] = vaddq_u64(vr[instr.dst], shifted);
            break;
        }
        case SuperscalarInstructionType::IMUL_R: {
            std::uint64_t dst0 = vgetq_lane_u64(vr[instr.dst], 0) * vgetq_lane_u64(vr[instr.src], 0);
            std::uint64_t dst1 = vgetq_lane_u64(vr[instr.dst], 1) * vgetq_lane_u64(vr[instr.src], 1);
            vr[instr.dst] = vcombine_u64(vcreate_u64(dst0), vcreate_u64(dst1));
            break;
        }
        case SuperscalarInstructionType::IROR_C: {
            std::uint64_t dst0 = std::rotr(vgetq_lane_u64(vr[instr.dst], 0), static_cast<int>(instr.getImm32()));
            std::uint64_t dst1 = std::rotr(vgetq_lane_u64(vr[instr.dst], 1), static_cast<int>(instr.getImm32()));
            vr[instr.dst] = vcombine_u64(vcreate_u64(dst0), vcreate_u64(dst1));
            break;
        }
        case SuperscalarInstructionType::IADD_C7:
        case SuperscalarInstructionType::IADD_C8:
        case SuperscalarInstructionType::IADD_C9: {
            std::uint64_t imm = signExtend2sCompl(instr.getImm32());
            vr[instr.dst] = vaddq_u64(vr[instr.dst], vmovq_n_u64(imm));
            break;
        }
        case SuperscalarInstructionType::IXOR_C7:
        case SuperscalarInstructionType::IXOR_C8:
        case SuperscalarInstructionType::IXOR_C9: {
            std::uint64_t imm = signExtend2sCompl(instr.getImm32());
            vr[instr.dst] = veorq_u64(vr[instr.dst], vmovq_n_u64(imm));
            break;
        }
        case SuperscalarInstructionType::IMULH_R: {
            std::uint64_t dst0 = mulh(vgetq_lane_u64(vr[instr.dst], 0), vgetq_lane_u64(vr[instr.src], 0));
            std::uint64_t dst1 = mulh(vgetq_lane_u64(vr[instr.dst], 1), vgetq_lane_u64(vr[instr.src], 1));
            vr[instr.dst] = vcombine_u64(vcreate_u64(dst0), vcreate_u64(dst1));
            break;
        }
        case SuperscalarInstructionType::ISMULH_R: {
            std::uint64_t dst0 = smulh(vgetq_lane_u64(vr[instr.dst], 0), vgetq_lane_u64(vr[instr.src], 0));
            std::uint64_t dst1 = smulh(vgetq_lane_u64(vr[instr.dst], 1), vgetq_lane_u64(vr[instr.src], 1));
            vr[instr.dst] = vcombine_u64(vcreate_u64(dst0), vcreate_u64(dst1));
            break;
        }
        case SuperscalarInstructionType::IMUL_RCP: {
            std::uint64_t rcp = reciprocals != nullptr ? (*reciprocals)[instr.getImm32()] : randomx_reciprocal(instr.getImm32());
            std::uint64_t dst0 = vgetq_lane_u64(vr[instr.dst], 0) * rcp;
            std::uint64_t dst1 = vgetq_lane_u64(vr[instr.dst], 1) * rcp;
            vr[instr.dst] = vcombine_u64(vcreate_u64(dst0), vcreate_u64(dst1));
            break;
        }
        default:
            throw std::runtime_error("unreachable instruction opcode in execute_superscalar_neon");
        }
    }
}
#endif

} // namespace armrx
