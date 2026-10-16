// x86-64 baseline tier for the WABT interpreter on Nocturne.
//
// A hot function's istream is translated instruction by instruction into
// machine code that works directly on the interpreter's own state: the value
// stack (Thread::values_), its frame, globals and memory 0. Compiled code keeps
//   rbx = values_ end (the stack pointer), r12 = Thread*, r13 = memory 0 base,
//   r14 = address of memory 0's byte length, r15 = frames_.size() at entry.
// Locals, constants, branches, integer/float arithmetic, comparisons,
// conversions and plain loads/stores are compiled. Every other instruction
// (calls, returns, throws, tables, atomics, most SIMD, references) and every
// unusual case of a compiled one (a bounds failure, a trapping division, a
// reference on the stack) goes to Thread::JitStep, which runs that one
// instruction with the interpreter and says where compiled code continues.
// Frames, traps and exceptions therefore stay exactly the interpreter's.

#include "wabt/interp/interp.h"

#include <stdio.h>
#include <string.h>
#include <nocturne.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "wasm-rt-nocturne.h"

namespace wabt {
namespace interp {

struct JitChunk {
  u8* base;
  size_t size;
  size_t used;
  size_t live;
};

struct JitCode {
  u8* code = nullptr;
  size_t size = 0;
  JitChunk* chunk = nullptr;
  // (istream offset << 32) | code offset, ascending by istream offset.
  std::vector<u64> map;

  void* entry() const { return code; }
  void* Find(u32 pc) const {
    u64 key = static_cast<u64>(pc) << 32;
    auto it = std::lower_bound(map.begin(), map.end(), key);
    if (it == map.end() || (*it >> 32) != pc) return nullptr;
    return code + static_cast<u32>(*it);
  }
};

namespace {

using O = Opcode;

enum Reg { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum Cond { CO, CNO, CB, CAE, CE, CNE, CBE, CA, CS, CNS, CP, CNP, CL, CGE, CLE, CG };
constexpr int kAlways = -1;

constexpr s32 kValue = sizeof(Value);
static_assert(sizeof(Value) == 16, "compiled code moves 16-byte stack slots");

// Backward branches between interrupt polls in compiled loops.
constexpr s32 kJitBudget = 256;
// Entries (calls, and interpreted loops asking) before a function is compiled.
constexpr u32 kJitHeat = 8;
// Native stack kept free below a compiled activation for the interpreter,
// host imports and nested agents; deeper Wasm recursion stays interpreted.
constexpr uintptr_t kJitStackReserve = 1024 * 1024;

struct Mem {
  int base;
  int index;
  int scale;
  s32 disp;
};

Mem At(int base, s32 disp) { return {base, -1, 0, disp}; }
Mem AtIndex(int base, int index, s32 disp) { return {base, index, 0, disp}; }

bool FitsS8(s64 v) { return v >= -128 && v <= 127; }
bool FitsS32(s64 v) { return v >= INT32_MIN && v <= INT32_MAX; }

class Asm {
 public:
  std::vector<u8> b;

  size_t pos() const { return b.size(); }
  void U8(u8 v) { b.push_back(v); }
  void U32(u32 v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
  }
  void U64(u64 v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
  }
  void Patch32(size_t at, u32 v) { memcpy(&b[at], &v, 4); }
  // The rel32 field at |at| now reaches buffer offset |target|.
  void Bind(size_t at, size_t target) {
    Patch32(at, static_cast<u32>(static_cast<s64>(target) -
                                 static_cast<s64>(at + 4)));
  }
  void Bind8(size_t at, size_t target) {
    b[at] = static_cast<u8>(static_cast<s64>(target) - static_cast<s64>(at + 1));
  }

  // [prefix] [REX] opcode ModRM(reg, memory) [SIB] [disp]
  void RM(u8 prefix, bool w, std::initializer_list<u8> opc, int r, Mem m) {
    if (prefix) U8(prefix);
    u8 rex = 0x40 | (w ? 8 : 0) | ((r & 8) ? 4 : 0) |
             ((m.index >= 0 && (m.index & 8)) ? 2 : 0) | ((m.base & 8) ? 1 : 0);
    if (rex != 0x40) U8(rex);
    for (u8 o : opc) U8(o);
    int mod = (m.disp == 0 && (m.base & 7) != RBP) ? 0 : FitsS8(m.disp) ? 1 : 2;
    if (m.index >= 0 || (m.base & 7) == RSP) {
      U8(static_cast<u8>(mod << 6 | (r & 7) << 3 | 4));
      int index = m.index >= 0 ? (m.index & 7) : 4;
      U8(static_cast<u8>(m.scale << 6 | index << 3 | (m.base & 7)));
    } else {
      U8(static_cast<u8>(mod << 6 | (r & 7) << 3 | (m.base & 7)));
    }
    if (mod == 1) U8(static_cast<u8>(m.disp));
    if (mod == 2) U32(static_cast<u32>(m.disp));
  }
  // [prefix] [REX] opcode ModRM(reg, register)
  void RR(u8 prefix, bool w, std::initializer_list<u8> opc, int r, int rm) {
    if (prefix) U8(prefix);
    u8 rex = 0x40 | (w ? 8 : 0) | ((r & 8) ? 4 : 0) | ((rm & 8) ? 1 : 0);
    if (rex != 0x40) U8(rex);
    for (u8 o : opc) U8(o);
    U8(static_cast<u8>(0xC0 | (r & 7) << 3 | (rm & 7)));
  }

  void Load32(int r, Mem m) { RM(0, false, {0x8B}, r, m); }
  void Load64(int r, Mem m) { RM(0, true, {0x8B}, r, m); }
  void Store32(Mem m, int r) { RM(0, false, {0x89}, r, m); }
  void Store64(Mem m, int r) { RM(0, true, {0x89}, r, m); }
  void Mov64(int dst, int src) { RR(0, true, {0x89}, src, dst); }
  void MovImm32(int r, u32 v) {
    if (r & 8) U8(0x41);
    U8(static_cast<u8>(0xB8 + (r & 7)));
    U32(v);
  }
  void MovImm64(int r, u64 v) {
    U8(static_cast<u8>(0x48 | ((r & 8) ? 1 : 0)));
    U8(static_cast<u8>(0xB8 + (r & 7)));
    U64(v);
  }
  void MovImmPtr(int r, const void* p) {
    MovImm64(r, reinterpret_cast<uintptr_t>(p));
  }
  void LoadV(Mem m) { RM(0, false, {0x0F, 0x10}, 0, m); }   // movups xmm0, m
  void StoreV(Mem m) { RM(0, false, {0x0F, 0x11}, 0, m); }  // movups m, xmm0
  void AddImm64(int r, s32 v) {
    if (v == 0) return;
    if (FitsS8(v)) {
      RR(0, true, {0x83}, 0, r);
      U8(static_cast<u8>(v));
    } else {
      RR(0, true, {0x81}, 0, r);
      U32(static_cast<u32>(v));
    }
  }
  void CmpImm(bool w, int r, s32 v) {
    if (FitsS8(v)) {
      RR(0, w, {0x83}, 7, r);
      U8(static_cast<u8>(v));
    } else {
      RR(0, w, {0x81}, 7, r);
      U32(static_cast<u32>(v));
    }
  }
  void Test(bool w, int x, int y) { RR(0, w, {0x85}, y, x); }
  void SetCC(int c, int r) { RR(0, false, {0x0F, static_cast<u8>(0x90 + c)}, 0, r); }
  void MovzxByte(int dst, int src) { RR(0, false, {0x0F, 0xB6}, dst, src); }
  // Each returns its rel32 (or rel8) field.
  size_t Jmp() {
    U8(0xE9);
    U32(0);
    return pos() - 4;
  }
  size_t Jcc(int c) {
    U8(0x0F);
    U8(static_cast<u8>(0x80 + c));
    U32(0);
    return pos() - 4;
  }
  size_t Jcc8(int c) {
    U8(static_cast<u8>(0x70 + c));
    U8(0);
    return pos() - 1;
  }
  void JmpReg(int r) { RR(0, false, {0xFF}, 4, r); }
  void CallReg(int r) { RR(0, false, {0xFF}, 2, r); }
  size_t LeaRip(int r) {
    U8(static_cast<u8>(0x48 | ((r & 8) ? 4 : 0)));
    U8(0x8D);
    U8(static_cast<u8>((r & 7) << 3 | 5));
    U32(0);
    return pos() - 4;
  }
  void Push(int r) {
    if (r & 8) U8(0x41);
    U8(static_cast<u8>(0x50 + (r & 7)));
  }
  void Pop(int r) {
    if (r & 8) U8(0x41);
    U8(static_cast<u8>(0x58 + (r & 7)));
  }
};

struct CpuFeatures {
  bool sse41 = false;
  bool popcnt = false;
};

CpuFeatures DetectCpu() {
  u32 a = 1, b = 0, c = 0, d = 0;
  __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
  CpuFeatures f;
  f.sse41 = (c >> 19) & 1;
  f.popcnt = (c >> 23) & 1;
  return f;
}

const CpuFeatures& Cpu() {
  static const CpuFeatures features = DetectCpu();
  return features;
}

// Executable memory: chunks mapped writable, filled, then made executable
// page by page. A chunk is unmapped once the last function in it is gone.
class JitArena {
 public:
  JitChunk* Place(const u8* bytes, size_t size, u8** out) {
    Lock();
    JitChunk* chunk = PlaceLocked(bytes, size, out);
    Unlock();
    return chunk;
  }

  void Release(JitChunk* chunk, size_t size) {
    Lock();
    chunk->live -= Rounded(size);
    if (chunk->live == 0 && chunk != current_) Unmap(chunk);
    Unlock();
  }

 private:
  static constexpr size_t kPage = 4096;
  static constexpr size_t kChunk = 1024 * 1024;

  static size_t Rounded(size_t size) { return (size + 15) & ~size_t(15); }

  JitChunk* PlaceLocked(const u8* bytes, size_t size, u8** out) {
    size_t need = Rounded(size);
    if (!current_ || current_->size - current_->used < need) {
      size_t bytes_needed = (need + kPage - 1) & ~(kPage - 1);
      size_t chunk_size = std::max(kChunk, bytes_needed);
      void* p = n_memory_reserve(chunk_size, N_MEMORY_READ_WRITE);
      if (!p) return nullptr;
      JitChunk* chunk = new JitChunk{static_cast<u8*>(p), chunk_size, 0, 0};
      if (current_ && current_->live == 0) Unmap(current_);
      current_ = chunk;
    }
    JitChunk* chunk = current_;
    u8* dst = chunk->base + chunk->used;
    uintptr_t first = reinterpret_cast<uintptr_t>(dst) & ~(kPage - 1);
    uintptr_t last =
        (reinterpret_cast<uintptr_t>(dst) + need + kPage - 1) & ~(kPage - 1);
    void* pages = reinterpret_cast<void*>(first);
    // W^X: the pages are writable only while the code is copied in.
    if (n_memory_protect(pages, last - first, N_MEMORY_READ_WRITE) != 0) return nullptr;
    memcpy(dst, bytes, size);
    if (n_memory_protect(pages, last - first, N_MEMORY_READ_EXECUTE) != 0) return nullptr;
    chunk->used += need;
    chunk->live += need;
    *out = dst;
    return chunk;
  }

  void Unmap(JitChunk* chunk) {
    n_memory_release(chunk->base, chunk->size);
    delete chunk;
  }
  void Lock() {
    while (lock_.test_and_set(std::memory_order_acquire)) {
    }
  }
  void Unlock() { lock_.clear(std::memory_order_release); }

  JitChunk* current_ = nullptr;
  std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};

JitArena& Arena() {
  static JitArena arena;
  return arena;
}

// enter(Thread*, target, entry stub, depth, values_ end)
using JitEnterFn = void (*)(Thread*, void*, void*, u64, Value*);

struct Trampoline {
  JitEnterFn enter = nullptr;
  void* exit = nullptr;
};

const Trampoline& Trampolines() {
  static const Trampoline trampoline = [] {
    Trampoline r;
    Asm a;
    a.Push(RBP);
    a.Push(RBX);
    a.Push(R12);
    a.Push(R13);
    a.Push(R14);
    a.Push(R15);
    a.AddImm64(RSP, -8);  // calls from compiled code see a 16-byte aligned rsp
    a.Mov64(R12, RDI);
    a.Mov64(R15, RCX);
    a.Mov64(RBX, R8);
    a.JmpReg(RDX);
    size_t exit = a.pos();
    a.AddImm64(RSP, 8);
    a.Pop(R15);
    a.Pop(R14);
    a.Pop(R13);
    a.Pop(R12);
    a.Pop(RBX);
    a.Pop(RBP);
    a.U8(0xC3);
    u8* code = nullptr;
    if (Arena().Place(a.b.data(), a.b.size(), &code)) {
      r.enter = reinterpret_cast<JitEnterFn>(code);
      r.exit = code + exit;
    }
    return r;
  }();
  return trampoline;
}

struct JitStats {
  u64 functions = 0;
  u64 bytes = 0;
  u64 failures = 0;
  u64 next_report = 1;
};

JitStats& Stats() {
  static JitStats stats;
  return stats;
}

}  // namespace

class JitCompiler {
 public:
  JitCompiler(Thread& t, DefinedFunc& func, Instance* inst, Module* mod)
      : t_(t), func_(func), inst_(inst), mod_(mod) {}

  JitCode* Compile();

 private:
  struct Decoded {
    u32 pc;
    u32 next;
    Instr instr;
  };
  struct Fix {
    size_t at;
    u32 target;  // istream offset; end_ is the exit
  };
  struct Slow {
    u32 pc;
    u32 next;
    std::vector<size_t> jumps;
  };
  struct Poll {
    size_t jump;
    u32 target;
  };
  struct Table {
    size_t lea;
    u32 first;
    u32 count;
  };

  bool Decode();
  bool EmitInstr(const Decoded& d);
  void EmitStep(u32 pc, u32 next);
  size_t NewSlow(u32 pc, u32 next) {
    slows_.push_back({pc, next, {}});
    return slows_.size() - 1;
  }
  void JccSlow(int c, size_t slow) { slows_[slow].jumps.push_back(a_.Jcc(c)); }
  void Branch(int cond, u32 target, u32 pc);
  void CheckNoRefs(s32 count, size_t slow);
  bool MemoryOperand(const Instr& instr, u32 size, s32* disp);
  void BoundsCheck(s32 disp, u32 size, size_t slow);
  void ReloadMemoryBase();
  void CallHelper(const void* fn);
  void BoundField() {
    a_.RR(0, true, {0x81}, 7, RAX);  // cmp rax, imm32 (patched)
    bound_fields_.push_back(a_.pos());
    a_.U32(0);
  }
  bool Lookup(u32 pc, size_t* offset) const;

  static s32 Slot(u32 depth) { return -static_cast<s32>(depth) * kValue; }
  s32 Off(const void* field) const {
    return static_cast<s32>(reinterpret_cast<const char*>(field) -
                            reinterpret_cast<const char*>(&t_));
  }

  Thread& t_;
  DefinedFunc& func_;
  Instance* inst_;
  Module* mod_;
  Memory* memory_ = nullptr;  // memory 0, when compiled accesses can use it
  u32 start_ = 0, end_ = 0;
  std::vector<Decoded> code_;
  std::vector<u32> labels_;
  Asm a_;
  std::vector<Fix> fixes_;
  std::vector<Slow> slows_;
  std::vector<Poll> polls_;
  std::vector<Table> tables_;
  std::vector<size_t> step_jumps_, poll_jumps_, exit_jumps_, grow_jumps_,
      bound_fields_;
  size_t exit_ = 0;
  u64 pushes_ = 0;  // stack slots compiled code may add between checks
  // Thread field offsets.
  s32 end_off_ = 0, cap_off_ = 0, begin_off_ = 0, refs_begin_off_ = 0,
      refs_end_off_ = 0, budget_off_ = 0;
};

bool JitCompiler::Decode() {
  const ModuleDesc& md = mod_->desc();
  const Istream& istream = md.istream;
  const FuncDesc& desc = func_.desc();
  if (md.funcs.empty() || &desc < md.funcs.data() ||
      &desc >= md.funcs.data() + md.funcs.size()) {
    return false;
  }
  size_t index = &desc - md.funcs.data();
  start_ = desc.code_offset;
  u32 end = istream.end();
  if (index + 1 < md.funcs.size() && md.funcs[index + 1].code_offset > start_) {
    end = md.funcs[index + 1].code_offset;
  } else {
    // The last body: the next code in the stream is an initializer.
    auto clip = [&](const FuncDesc& f) {
      if (f.code_offset > start_ && f.code_offset < end) end = f.code_offset;
    };
    for (const auto& data : md.datas) clip(data.init_func);
    for (const auto& global : md.globals) clip(global.init_func);
    for (const auto& table : md.tables) clip(table.init_func);
    for (const auto& elem : md.elems) {
      clip(elem.init_func);
      for (const auto& element : elem.elements) clip(element);
    }
  }
  end_ = end;
  for (u32 pc = start_; pc < end_;) {
    u32 at = pc;
    Instr instr = istream.Read(&pc);
    if (pc > end_) return false;
    code_.push_back({at, pc, instr});
  }
  return !code_.empty();
}

bool JitCompiler::Lookup(u32 pc, size_t* offset) const {
  if (pc == end_) {
    *offset = exit_;
    return true;
  }
  auto it = std::lower_bound(
      code_.begin(), code_.end(), pc,
      [](const Decoded& d, u32 value) { return d.pc < value; });
  if (it == code_.end() || it->pc != pc) return false;
  *offset = labels_[it - code_.begin()];
  return true;
}

void JitCompiler::CallHelper(const void* fn) {
  a_.MovImmPtr(RAX, fn);
  a_.CallReg(RAX);
}

void JitCompiler::ReloadMemoryBase() {
  if (!memory_ || memory_->shared()) return;
  a_.MovImmPtr(R13, memory_->base_address());
  a_.Load64(R13, At(R13, 0));
}

// The interpreter runs instruction |pc|; compiled code continues at |next|
// unless JitStep says otherwise.
void JitCompiler::EmitStep(u32 pc, u32 next) {
  a_.MovImm32(RSI, pc);
  a_.MovImm32(RDX, next);
  fixes_.push_back({a_.LeaRip(RCX), next});
  step_jumps_.push_back(a_.Jmp());
}

// Jumps to istream |target| when |cond| holds (kAlways: unconditionally).
// A backward branch spends the thread's loop budget and polls when it runs
// out, so compiled loops stay interruptible.
void JitCompiler::Branch(int cond, u32 target, u32 pc) {
  if (target > pc) {
    fixes_.push_back({cond == kAlways ? a_.Jmp() : a_.Jcc(cond), target});
    return;
  }
  size_t skip = 0;
  if (cond != kAlways) skip = a_.Jcc8(cond ^ 1);
  a_.RM(0, false, {0x83}, 5, At(R12, budget_off_));  // sub dword budget, 1
  a_.U8(1);
  polls_.push_back({a_.Jcc(CL), target});
  fixes_.push_back({a_.Jmp(), target});
  if (cond != kAlways) a_.Bind8(skip, a_.pos());
}

// The top |count| values are numbers: no reference index reaches them.
void JitCompiler::CheckNoRefs(s32 count, size_t slow) {
  a_.Load64(RAX, At(R12, refs_end_off_));
  a_.RM(0, true, {0x3B}, RAX, At(R12, refs_begin_off_));
  size_t none = a_.Jcc8(CE);
  a_.Load32(RAX, At(RAX, -4));
  a_.Mov64(RCX, RBX);
  a_.RM(0, true, {0x2B}, RCX, At(R12, begin_off_));
  a_.RR(0, true, {0xC1}, 5, RCX);  // shr rcx, 4
  a_.U8(4);
  a_.AddImm64(RCX, -count);
  a_.RR(0, true, {0x3B}, RAX, RCX);
  JccSlow(CAE, slow);
  a_.Bind8(none, a_.pos());
}

bool JitCompiler::MemoryOperand(const Instr& instr, u32 size, s32* disp) {
  if (!memory_ || instr.imm_index_offset.memidx != 0) return false;
  u64 offset = instr.imm_index_offset.offset;
  if (offset + size > static_cast<u64>(INT32_MAX)) return false;
  *disp = static_cast<s32>(offset);
  return true;
}

// rax holds the 32-bit address; the access [rax+disp, +size) must fit.
void JitCompiler::BoundsCheck(s32 disp, u32 size, size_t slow) {
  a_.RM(0, true, {0x8D}, RCX, At(RAX, disp + static_cast<s32>(size)));
  a_.RM(0, true, {0x3B}, RCX, At(R14, 0));
  JccSlow(CA, slow);
}

bool JitCompiler::EmitInstr(const Decoded& d) {
  const Instr& in = d.instr;
  const u32 pc = d.pc, next = d.next;
  auto& a = a_;
  const Mem top = At(RBX, Slot(1)), second = At(RBX, Slot(2));

  auto pop = [&](u32 n) { a.AddImm64(RBX, -static_cast<s32>(n) * kValue); };
  // i32/i64 binary operation with a register-memory form: eax op= rhs.
  auto alu = [&](bool w, std::initializer_list<u8> opc) {
    a.RM(0, w, {0x8B}, RAX, second);
    a.RM(0, w, opc, RAX, top);
    a.RM(0, w, {0x89}, RAX, second);
    pop(1);
  };
  auto shift = [&](bool w, int digit) {
    a.Load32(RCX, top);
    a.RM(0, w, {0xD3}, digit, second);
    pop(1);
  };
  auto compare = [&](bool w, int cond) {
    a.RM(0, w, {0x8B}, RAX, second);
    a.RM(0, w, {0x3B}, RAX, top);
    a.SetCC(cond, RAX);
    a.MovzxByte(RAX, RAX);
    a.Store32(second, RAX);
    pop(1);
  };
  auto eqz = [&](bool w) {
    a.RM(0, w, {0x83}, 7, top);
    a.U8(0);
    a.SetCC(CE, RAX);
    a.MovzxByte(RAX, RAX);
    a.Store32(top, RAX);
  };
  auto divide = [&](bool w, bool is_signed, bool remainder) {
    size_t slow = NewSlow(pc, next);
    a.RM(0, w, {0x8B}, RCX, top);
    a.Test(w, RCX, RCX);
    JccSlow(CE, slow);
    if (is_signed) {
      a.CmpImm(w, RCX, -1);
      JccSlow(CE, slow);
    }
    a.RM(0, w, {0x8B}, RAX, second);
    if (is_signed) {
      if (w) a.U8(0x48);
      a.U8(0x99);  // cdq / cqo
    } else {
      a.RR(0, false, {0x31}, RDX, RDX);  // xor edx, edx
    }
    a.RR(0, w, {0xF7}, is_signed ? 7 : 6, RCX);
    a.RM(0, w, {0x89}, remainder ? RDX : RAX, second);
    pop(1);
  };
  // f32 (F3) / f64 (F2) binary arithmetic: xmm0 = lhs op rhs.
  auto farith = [&](u8 prefix, u8 op) {
    a.RM(prefix, false, {0x0F, 0x10}, 0, second);
    a.RM(prefix, false, {0x0F, op}, 0, top);
    a.RM(prefix, false, {0x0F, 0x11}, 0, second);
    pop(1);
  };
  // Float comparison; |swap| compares rhs with lhs.
  auto fcompare = [&](bool f64, bool swap, int cond, int parity) {
    u8 prefix = f64 ? 0xF2 : 0xF3;
    a.RM(prefix, false, {0x0F, 0x10}, 0, swap ? top : second);
    a.RM(f64 ? 0x66 : 0, false, {0x0F, 0x2E}, 0, swap ? second : top);
    a.SetCC(cond, RAX);
    if (parity >= 0) {
      a.SetCC(parity, RCX);
      // eq: equal and ordered; ne: unequal or unordered.
      a.RR(0, false, {static_cast<u8>(parity == CNP ? 0x20 : 0x08)}, RCX, RAX);
    }
    a.MovzxByte(RAX, RAX);
    a.Store32(second, RAX);
    pop(1);
  };
  auto funary = [&](u8 prefix, std::initializer_list<u8> opc, int imm) {
    a.RM(prefix, false, opc, 0, top);
    if (imm >= 0) a.U8(static_cast<u8>(imm));
    a.RM(prefix == 0x66 ? (opc.end()[-1] == 0x0B ? 0xF2 : 0xF3) : prefix, false,
         {0x0F, 0x11}, 0, top);
  };
  auto load = [&](u32 size, bool w, std::initializer_list<u8> opc, u8 prefix,
                  bool store64) {
    s32 disp;
    if (!MemoryOperand(in, size, &disp)) return false;
    size_t slow = NewSlow(pc, next);
    a.Load32(RAX, top);
    BoundsCheck(disp, size, slow);
    if (size == 16) {
      a.LoadV(AtIndex(R13, RAX, disp));
      a.StoreV(top);
      return true;
    }
    a.RM(prefix, w, opc, RAX, AtIndex(R13, RAX, disp));
    if (store64) a.Store64(top, RAX);
    else a.Store32(top, RAX);
    return true;
  };
  auto store = [&](u32 size) {
    s32 disp;
    if (!MemoryOperand(in, size, &disp)) return false;
    size_t slow = NewSlow(pc, next);
    a.Load32(RAX, second);
    BoundsCheck(disp, size, slow);
    Mem target = AtIndex(R13, RAX, disp);
    switch (size) {
      case 1:
        a.Load32(RCX, top);
        a.RM(0, false, {0x88}, RCX, target);
        break;
      case 2:
        a.Load32(RCX, top);
        a.RM(0x66, false, {0x89}, RCX, target);
        break;
      case 4:
        a.Load32(RCX, top);
        a.Store32(target, RCX);
        break;
      case 8:
        a.Load64(RCX, top);
        a.Store64(target, RCX);
        break;
      default:
        a.LoadV(top);
        a.StoreV(target);
        break;
    }
    pop(2);
    return true;
  };
  // Signed truncation; cvtt* yields the "integer indefinite" value for NaN
  // and out-of-range inputs, which (and a true minimum) the interpreter
  // decides.
  auto trunc_signed = [&](u8 prefix, bool w) {
    size_t slow = NewSlow(pc, next);
    a.RM(prefix, w, {0x0F, 0x2C}, RAX, top);
    a.Mov64(RCX, RAX);
    a.RR(0, w, {0xF7}, 3, RCX);  // neg: overflows only for the minimum
    JccSlow(CO, slow);
    a.RM(0, w, {0x89}, RAX, top);
  };
  auto trunc_u32 = [&](u8 prefix) {
    size_t slow = NewSlow(pc, next);
    a.RM(prefix, true, {0x0F, 0x2C}, RAX, top);
    a.Mov64(RCX, RAX);
    a.RR(0, true, {0xC1}, 5, RCX);  // shr rcx, 32
    a.U8(32);
    JccSlow(CNE, slow);
    a.Store32(top, RAX);
  };
  auto push_imm32 = [&](u32 v) {
    a.RM(0, false, {0xC7}, 0, At(RBX, 0));
    a.U32(v);
    a.AddImm64(RBX, kValue);
    ++pushes_;
  };
  auto push_imm64 = [&](u64 v) {
    if (FitsS32(static_cast<s64>(v))) {
      a.RM(0, true, {0xC7}, 0, At(RBX, 0));
      a.U32(static_cast<u32>(v));
    } else {
      a.MovImm64(RAX, v);
      a.Store64(At(RBX, 0), RAX);
    }
    a.AddImm64(RBX, kValue);
    ++pushes_;
  };
  auto bitcount = [&](bool w, u8 op, u32 zero_result, int flip) {
    a.RM(0, w, {0x0F, op}, RAX, top);  // bsr / bsf
    a.MovImm32(RCX, zero_result);
    a.RR(0, w, {0x0F, 0x44}, RAX, RCX);  // cmovz
    if (flip) {
      a.RR(0, w, {0x83}, 6, RAX);
      a.U8(static_cast<u8>(flip));
    }
    a.RM(0, w, {0x89}, RAX, top);
  };

  switch (in.op) {
    case O::Nop:
    case O::I32WrapI64:
    case O::I32ReinterpretF32:
    case O::I64ReinterpretF64:
    case O::F32ReinterpretI32:
    case O::F64ReinterpretI64:
      return true;

    case O::LocalGet: {
      if (in.imm_u32 == 0 || in.imm_u32 > (1u << 26)) break;
      a.LoadV(At(RBX, Slot(in.imm_u32)));
      a.StoreV(At(RBX, 0));
      a.AddImm64(RBX, kValue);
      ++pushes_;
      return true;
    }
    case O::LocalSet: {
      if (in.imm_u32 < 2 || in.imm_u32 > (1u << 26)) break;
      size_t slow = NewSlow(pc, next);
      CheckNoRefs(1, slow);
      a.LoadV(top);
      a.StoreV(At(RBX, Slot(in.imm_u32)));
      pop(1);
      return true;
    }
    case O::LocalTee: {
      if (in.imm_u32 < 2 || in.imm_u32 > (1u << 26)) break;
      a.LoadV(top);
      a.StoreV(At(RBX, Slot(in.imm_u32)));
      return true;
    }
    case O::Drop: {
      size_t slow = NewSlow(pc, next);
      CheckNoRefs(1, slow);
      pop(1);
      return true;
    }
    case O::Select: {
      size_t slow = NewSlow(pc, next);
      CheckNoRefs(3, slow);
      a.Load32(RAX, top);
      pop(2);
      a.Test(false, RAX, RAX);
      size_t keep = a.Jcc8(CNE);
      a.LoadV(At(RBX, 0));
      a.StoreV(At(RBX, Slot(1)));
      a.Bind8(keep, a.pos());
      return true;
    }

    case O::I32Const:
    case O::F32Const:
      push_imm32(in.imm_u32);
      return true;
    case O::I64Const:
    case O::F64Const:
      push_imm64(in.imm_u64);
      return true;

    case O::GlobalGet: {
      const RefVec& globals = inst_->globals();
      if (in.imm_u32 >= globals.size()) break;
      auto* global = static_cast<Global*>(t_.store_.UnsafeObject(globals[in.imm_u32]));
      if (global->type().type.IsRef()) break;
      a.MovImmPtr(RAX, global->value_address());
      a.LoadV(At(RAX, 0));
      a.StoreV(At(RBX, 0));
      a.AddImm64(RBX, kValue);
      ++pushes_;
      return true;
    }
    case O::GlobalSet: {
      const RefVec& globals = inst_->globals();
      if (in.imm_u32 >= globals.size()) break;
      auto* global = static_cast<Global*>(t_.store_.UnsafeObject(globals[in.imm_u32]));
      if (global->type().type.IsRef()) break;
      size_t slow = NewSlow(pc, next);
      a.MovImmPtr(RAX, global->observed_address());
      a.RM(0, false, {0x80}, 7, At(RAX, 0));  // cmp byte [rax], 0
      a.U8(0);
      JccSlow(CNE, slow);
      a.MovImmPtr(RAX, global->value_address());
      a.LoadV(top);
      a.StoreV(At(RAX, 0));
      pop(1);
      return true;
    }

    case O::InterpAlloca: {
      u32 n = in.imm_u32;
      if (n > (1u << 26)) break;
      a.RR(0, false, {0x0F, 0x57}, 0, 0);  // xorps xmm0, xmm0
      if (n <= 16) {
        for (u32 i = 0; i < n; ++i) a.StoreV(At(RBX, static_cast<s32>(i) * kValue));
        a.AddImm64(RBX, static_cast<s32>(n) * kValue);
      } else {
        a.RM(0, true, {0x8D}, RCX, At(RBX, static_cast<s32>(n) * kValue));
        size_t loop = a.pos();
        a.StoreV(At(RBX, 0));
        a.AddImm64(RBX, kValue);
        a.RR(0, true, {0x3B}, RBX, RCX);
        size_t back = a.Jcc8(CB);
        a.Bind8(back, loop);
      }
      pushes_ += n;
      return true;
    }
    case O::InterpDropKeep: {
      u32 drop = in.imm_u32x2.fst, keep = in.imm_u32x2.snd;
      if (drop == 0) return true;
      if (keep > 8 || drop > (1u << 26)) break;
      size_t slow = NewSlow(pc, next);
      CheckNoRefs(static_cast<s32>(drop + keep), slow);
      for (u32 i = 0; i < keep; ++i) {
        a.LoadV(At(RBX, Slot(keep - i)));
        a.StoreV(At(RBX, Slot(drop + keep - i)));
      }
      pop(drop);
      return true;
    }
    case O::InterpCatchDrop:
      if (in.imm_u32 == 0) return true;
      break;

    case O::Br:
      Branch(kAlways, in.imm_u32, pc);
      return true;
    case O::BrIf:
    case O::InterpBrUnless:
      a.Load32(RAX, top);
      pop(1);
      a.Test(false, RAX, RAX);
      Branch(in.op == O::BrIf ? CNE : CE, in.imm_u32, pc);
      return true;
    case O::BrTable: {
      u32 count = in.imm_u32 + 1;
      a.Load32(RAX, top);
      pop(1);
      a.MovImm32(RCX, in.imm_u32);
      a.RR(0, false, {0x3B}, RAX, RCX);
      a.RR(0, false, {0x0F, 0x47}, RAX, RCX);  // cmova: the default entry
      size_t lea = a.LeaRip(RCX);
      a.RM(0, true, {0x63}, RAX, Mem{RCX, RAX, 2, 0});  // movsxd rax, [rcx+rax*4]
      a.RR(0, true, {0x01}, RCX, RAX);                  // add rax, rcx
      a.JmpReg(RAX);
      tables_.push_back({lea, next, count});
      return true;
    }

    case O::I32Add: alu(false, {0x03}); return true;
    case O::I32Sub: alu(false, {0x2B}); return true;
    case O::I32Mul: alu(false, {0x0F, 0xAF}); return true;
    case O::I32And: alu(false, {0x23}); return true;
    case O::I32Or:  alu(false, {0x0B}); return true;
    case O::I32Xor: alu(false, {0x33}); return true;
    case O::I64Add: alu(true, {0x03}); return true;
    case O::I64Sub: alu(true, {0x2B}); return true;
    case O::I64Mul: alu(true, {0x0F, 0xAF}); return true;
    case O::I64And: alu(true, {0x23}); return true;
    case O::I64Or:  alu(true, {0x0B}); return true;
    case O::I64Xor: alu(true, {0x33}); return true;

    case O::I32Shl:  shift(false, 4); return true;
    case O::I32ShrU: shift(false, 5); return true;
    case O::I32ShrS: shift(false, 7); return true;
    case O::I32Rotl: shift(false, 0); return true;
    case O::I32Rotr: shift(false, 1); return true;
    case O::I64Shl:  shift(true, 4); return true;
    case O::I64ShrU: shift(true, 5); return true;
    case O::I64ShrS: shift(true, 7); return true;
    case O::I64Rotl: shift(true, 0); return true;
    case O::I64Rotr: shift(true, 1); return true;

    case O::I32DivS: divide(false, true, false); return true;
    case O::I32DivU: divide(false, false, false); return true;
    case O::I32RemS: divide(false, true, true); return true;
    case O::I32RemU: divide(false, false, true); return true;
    case O::I64DivS: divide(true, true, false); return true;
    case O::I64DivU: divide(true, false, false); return true;
    case O::I64RemS: divide(true, true, true); return true;
    case O::I64RemU: divide(true, false, true); return true;

    case O::I32Eq:  compare(false, CE); return true;
    case O::I32Ne:  compare(false, CNE); return true;
    case O::I32LtS: compare(false, CL); return true;
    case O::I32LtU: compare(false, CB); return true;
    case O::I32GtS: compare(false, CG); return true;
    case O::I32GtU: compare(false, CA); return true;
    case O::I32LeS: compare(false, CLE); return true;
    case O::I32LeU: compare(false, CBE); return true;
    case O::I32GeS: compare(false, CGE); return true;
    case O::I32GeU: compare(false, CAE); return true;
    case O::I64Eq:  compare(true, CE); return true;
    case O::I64Ne:  compare(true, CNE); return true;
    case O::I64LtS: compare(true, CL); return true;
    case O::I64LtU: compare(true, CB); return true;
    case O::I64GtS: compare(true, CG); return true;
    case O::I64GtU: compare(true, CA); return true;
    case O::I64LeS: compare(true, CLE); return true;
    case O::I64LeU: compare(true, CBE); return true;
    case O::I64GeS: compare(true, CGE); return true;
    case O::I64GeU: compare(true, CAE); return true;
    case O::I32Eqz: eqz(false); return true;
    case O::I64Eqz: eqz(true); return true;

    case O::I32Clz: bitcount(false, 0xBD, 63, 31); return true;
    case O::I64Clz: bitcount(true, 0xBD, 127, 63); return true;
    case O::I32Ctz: bitcount(false, 0xBC, 32, 0); return true;
    case O::I64Ctz: bitcount(true, 0xBC, 64, 0); return true;
    case O::I32Popcnt:
    case O::I64Popcnt: {
      if (!Cpu().popcnt) break;
      bool w = in.op == O::I64Popcnt;
      a.RM(0xF3, w, {0x0F, 0xB8}, RAX, top);
      a.RM(0, w, {0x89}, RAX, top);
      return true;
    }

    case O::I32Extend8S:
      a.RM(0, false, {0x0F, 0xBE}, RAX, top);
      a.Store32(top, RAX);
      return true;
    case O::I32Extend16S:
      a.RM(0, false, {0x0F, 0xBF}, RAX, top);
      a.Store32(top, RAX);
      return true;
    case O::I64Extend8S:
      a.RM(0, true, {0x0F, 0xBE}, RAX, top);
      a.Store64(top, RAX);
      return true;
    case O::I64Extend16S:
      a.RM(0, true, {0x0F, 0xBF}, RAX, top);
      a.Store64(top, RAX);
      return true;
    case O::I64Extend32S:
    case O::I64ExtendI32S:
      a.RM(0, true, {0x63}, RAX, top);
      a.Store64(top, RAX);
      return true;
    case O::I64ExtendI32U:
      a.Load32(RAX, top);
      a.Store64(top, RAX);
      return true;

    case O::I32Load:
    case O::F32Load:
      if (load(4, false, {0x8B}, 0, false)) return true;
      break;
    case O::I64Load:
    case O::F64Load:
      if (load(8, true, {0x8B}, 0, true)) return true;
      break;
    case O::I32Load8S:
      if (load(1, false, {0x0F, 0xBE}, 0, false)) return true;
      break;
    case O::I32Load8U:
      if (load(1, false, {0x0F, 0xB6}, 0, false)) return true;
      break;
    case O::I32Load16S:
      if (load(2, false, {0x0F, 0xBF}, 0, false)) return true;
      break;
    case O::I32Load16U:
      if (load(2, false, {0x0F, 0xB7}, 0, false)) return true;
      break;
    case O::I64Load8S:
      if (load(1, true, {0x0F, 0xBE}, 0, true)) return true;
      break;
    case O::I64Load8U:
      if (load(1, false, {0x0F, 0xB6}, 0, true)) return true;
      break;
    case O::I64Load16S:
      if (load(2, true, {0x0F, 0xBF}, 0, true)) return true;
      break;
    case O::I64Load16U:
      if (load(2, false, {0x0F, 0xB7}, 0, true)) return true;
      break;
    case O::I64Load32S:
      if (load(4, true, {0x63}, 0, true)) return true;
      break;
    case O::I64Load32U:
      if (load(4, false, {0x8B}, 0, true)) return true;
      break;
    case O::V128Load:
      if (load(16, false, {}, 0, false)) return true;
      break;

    case O::I32Store:
    case O::F32Store:
    case O::I64Store32:
      if (store(4)) return true;
      break;
    case O::I64Store:
    case O::F64Store:
      if (store(8)) return true;
      break;
    case O::I32Store8:
    case O::I64Store8:
      if (store(1)) return true;
      break;
    case O::I32Store16:
    case O::I64Store16:
      if (store(2)) return true;
      break;
    case O::V128Store:
      if (store(16)) return true;
      break;

    case O::F32Add: farith(0xF3, 0x58); return true;
    case O::F32Sub: farith(0xF3, 0x5C); return true;
    case O::F32Mul: farith(0xF3, 0x59); return true;
    case O::F32Div: farith(0xF3, 0x5E); return true;
    case O::F64Add: farith(0xF2, 0x58); return true;
    case O::F64Sub: farith(0xF2, 0x5C); return true;
    case O::F64Mul: farith(0xF2, 0x59); return true;
    case O::F64Div: farith(0xF2, 0x5E); return true;
    case O::F32Sqrt: funary(0xF3, {0x0F, 0x51}, -1); return true;
    case O::F64Sqrt: funary(0xF2, {0x0F, 0x51}, -1); return true;
    case O::F32Neg:
      a.RM(0, false, {0x81}, 6, top);
      a.U32(0x80000000u);
      return true;
    case O::F32Abs:
      a.RM(0, false, {0x81}, 4, top);
      a.U32(0x7FFFFFFFu);
      return true;
    case O::F64Neg:
      a.RM(0, true, {0x0F, 0xBA}, 7, top);  // btc qword, 63
      a.U8(63);
      return true;
    case O::F64Abs:
      a.RM(0, true, {0x0F, 0xBA}, 6, top);  // btr qword, 63
      a.U8(63);
      return true;
    case O::F32Copysign:
      a.Load32(RAX, second);
      a.RR(0, false, {0x81}, 4, RAX);
      a.U32(0x7FFFFFFFu);
      a.Load32(RCX, top);
      a.RR(0, false, {0x81}, 4, RCX);
      a.U32(0x80000000u);
      a.RR(0, false, {0x09}, RCX, RAX);  // or eax, ecx
      a.Store32(second, RAX);
      pop(1);
      return true;
    case O::F64Copysign:
      a.Load64(RAX, second);
      a.RR(0, true, {0x0F, 0xBA}, 6, RAX);  // btr rax, 63
      a.U8(63);
      a.Load64(RCX, top);
      a.RR(0, true, {0xC1}, 5, RCX);  // shr rcx, 63
      a.U8(63);
      a.RR(0, true, {0xC1}, 4, RCX);  // shl rcx, 63
      a.U8(63);
      a.RR(0, true, {0x09}, RCX, RAX);
      a.Store64(second, RAX);
      pop(1);
      return true;
    case O::F32Ceil:
    case O::F32Floor:
    case O::F32Trunc:
    case O::F32Nearest:
    case O::F64Ceil:
    case O::F64Floor:
    case O::F64Trunc:
    case O::F64Nearest: {
      if (!Cpu().sse41) break;
      bool f64 = in.op == O::F64Ceil || in.op == O::F64Floor ||
                 in.op == O::F64Trunc || in.op == O::F64Nearest;
      int mode = (in.op == O::F32Ceil || in.op == O::F64Ceil)     ? 2
                 : (in.op == O::F32Floor || in.op == O::F64Floor) ? 1
                 : (in.op == O::F32Trunc || in.op == O::F64Trunc) ? 3
                                                                  : 0;
      // roundss/roundsd xmm0, m, mode (precision exception suppressed)
      funary(0x66, {0x0F, 0x3A, static_cast<u8>(f64 ? 0x0B : 0x0A)}, mode | 8);
      return true;
    }

    case O::F32Eq: fcompare(false, false, CE, CNP); return true;
    case O::F32Ne: fcompare(false, false, CNE, CP); return true;
    case O::F32Lt: fcompare(false, true, CA, -1); return true;
    case O::F32Gt: fcompare(false, false, CA, -1); return true;
    case O::F32Le: fcompare(false, true, CAE, -1); return true;
    case O::F32Ge: fcompare(false, false, CAE, -1); return true;
    case O::F64Eq: fcompare(true, false, CE, CNP); return true;
    case O::F64Ne: fcompare(true, false, CNE, CP); return true;
    case O::F64Lt: fcompare(true, true, CA, -1); return true;
    case O::F64Gt: fcompare(true, false, CA, -1); return true;
    case O::F64Le: fcompare(true, true, CAE, -1); return true;
    case O::F64Ge: fcompare(true, false, CAE, -1); return true;

    case O::I32TruncF32S: trunc_signed(0xF3, false); return true;
    case O::I32TruncF64S: trunc_signed(0xF2, false); return true;
    case O::I64TruncF32S: trunc_signed(0xF3, true); return true;
    case O::I64TruncF64S: trunc_signed(0xF2, true); return true;
    case O::I32TruncF32U: trunc_u32(0xF3); return true;
    case O::I32TruncF64U: trunc_u32(0xF2); return true;

    case O::F32ConvertI32S:
      a.RM(0xF3, false, {0x0F, 0x2A}, 0, top);
      a.RM(0xF3, false, {0x0F, 0x11}, 0, top);
      return true;
    case O::F64ConvertI32S:
      a.RM(0xF2, false, {0x0F, 0x2A}, 0, top);
      a.RM(0xF2, false, {0x0F, 0x11}, 0, top);
      return true;
    case O::F32ConvertI64S:
      a.RM(0xF3, true, {0x0F, 0x2A}, 0, top);
      a.RM(0xF3, false, {0x0F, 0x11}, 0, top);
      return true;
    case O::F64ConvertI64S:
      a.RM(0xF2, true, {0x0F, 0x2A}, 0, top);
      a.RM(0xF2, false, {0x0F, 0x11}, 0, top);
      return true;
    case O::F32ConvertI32U:
      a.Load32(RAX, top);
      a.RR(0xF3, true, {0x0F, 0x2A}, 0, RAX);
      a.RM(0xF3, false, {0x0F, 0x11}, 0, top);
      return true;
    case O::F64ConvertI32U:
      a.Load32(RAX, top);
      a.RR(0xF2, true, {0x0F, 0x2A}, 0, RAX);
      a.RM(0xF2, false, {0x0F, 0x11}, 0, top);
      return true;
    case O::F64PromoteF32:
      a.RM(0xF3, false, {0x0F, 0x5A}, 0, top);
      a.RM(0xF2, false, {0x0F, 0x11}, 0, top);
      return true;
    case O::F32DemoteF64:
      a.RM(0xF2, false, {0x0F, 0x5A}, 0, top);
      a.RM(0xF3, false, {0x0F, 0x11}, 0, top);
      return true;

    default:
      break;
  }
  EmitStep(pc, next);
  return true;
}

JitCode* JitCompiler::Compile() {
  const Trampoline& trampoline = Trampolines();
  if (!trampoline.enter) return nullptr;
  end_off_ = Off(&t_.values_.end_);
  cap_off_ = Off(&t_.values_.cap_);
  begin_off_ = Off(&t_.values_.begin_);
  refs_begin_off_ = Off(&t_.refs_.begin_);
  refs_end_off_ = Off(&t_.refs_.end_);
  budget_off_ = Off(&t_.jit_budget_);

  const RefVec& memories = inst_->memories();
  if (!memories.empty()) {
    auto* memory = static_cast<Memory*>(t_.store_.UnsafeObject(memories[0]));
    if (!memory->type().limits.is_64) memory_ = memory;
  }
  if (!Decode()) return nullptr;

  // Entry stub: memory registers, then room for the compiled pushes.
  if (memory_) {
    if (memory_->shared()) {
      a_.MovImmPtr(R13, memory_->shared_data());
    } else {
      ReloadMemoryBase();
    }
    a_.MovImmPtr(R14, memory_->length_address());
  }
  a_.Load64(RAX, At(R12, cap_off_));
  a_.RR(0, true, {0x2B}, RAX, RBX);  // sub rax, rbx
  BoundField();
  size_t entry_grow = a_.Jcc(CL);
  a_.JmpReg(RSI);

  labels_.resize(code_.size());
  for (size_t i = 0; i < code_.size(); ++i) {
    labels_[i] = static_cast<u32>(a_.pos());
    if (!EmitInstr(code_[i])) return nullptr;
  }

  for (Slow& slow : slows_) {
    for (size_t jump : slow.jumps) a_.Bind(jump, a_.pos());
    EmitStep(slow.pc, slow.next);
  }
  for (Poll& poll : polls_) {
    a_.Bind(poll.jump, a_.pos());
    fixes_.push_back({a_.LeaRip(RSI), poll.target});
    poll_jumps_.push_back(a_.Jmp());
  }

  // Step thunk: rsi = pc, rdx = next pc, rcx = next code.
  size_t step = a_.pos();
  a_.Store64(At(R12, end_off_), RBX);
  a_.Mov64(RDI, R12);
  a_.Mov64(R8, R15);
  CallHelper(reinterpret_cast<const void*>(&Thread::JitStep));
  size_t resume = a_.pos();
  a_.Load64(RBX, At(R12, end_off_));
  ReloadMemoryBase();
  a_.Test(true, RAX, RAX);
  exit_jumps_.push_back(a_.Jcc(CE));
  // The interpreter may have grown the stack (calls, catches): keep room for
  // the compiled pushes ahead.
  a_.Load64(RCX, At(R12, cap_off_));
  a_.RR(0, true, {0x2B}, RCX, RBX);
  a_.RR(0, true, {0x81}, 7, RCX);
  bound_fields_.push_back(a_.pos());
  a_.U32(0);
  grow_jumps_.push_back(a_.Jcc(CL));
  a_.JmpReg(RAX);

  // Poll thunk: rsi = loop target code.
  size_t poll = a_.pos();
  a_.Store64(At(R12, end_off_), RBX);
  a_.Mov64(RDI, R12);
  CallHelper(reinterpret_cast<const void*>(&Thread::JitPoll));
  a_.Bind(a_.Jmp(), resume);

  exit_ = a_.pos();
  a_.MovImmPtr(RAX, trampoline.exit);
  a_.JmpReg(RAX);

  // Grow from the entry stub: the target is in rsi.
  a_.Bind(entry_grow, a_.pos());
  a_.Push(RSI);
  a_.Push(RSI);
  a_.Store64(At(R12, end_off_), RBX);
  a_.Mov64(RDI, R12);
  a_.MovImm32(RSI, 0);
  bound_fields_.push_back(a_.pos() - 4);
  CallHelper(reinterpret_cast<const void*>(&Thread::JitGrow));
  a_.Mov64(RBX, RAX);
  a_.Pop(RSI);
  a_.Pop(RSI);
  a_.JmpReg(RSI);

  // Grow after a step: the target is in rax.
  size_t step_grow = a_.pos();
  a_.Push(RAX);
  a_.Push(RAX);
  a_.Mov64(RDI, R12);
  a_.MovImm32(RSI, 0);
  bound_fields_.push_back(a_.pos() - 4);
  CallHelper(reinterpret_cast<const void*>(&Thread::JitGrow));
  a_.Mov64(RBX, RAX);
  a_.Pop(RAX);
  a_.Pop(RAX);
  a_.JmpReg(RAX);

  while (a_.pos() & 3) a_.U8(0xCC);
  for (Table& table : tables_) {
    size_t base = a_.pos();
    a_.Bind(table.lea, base);
    for (u32 i = 0; i < table.count; ++i) {
      size_t target;
      if (!Lookup(table.first + i * Istream::kBrTableEntrySize, &target)) {
        return nullptr;
      }
      a_.U32(static_cast<u32>(static_cast<s64>(target) - static_cast<s64>(base)));
    }
  }

  for (size_t jump : step_jumps_) a_.Bind(jump, step);
  for (size_t jump : poll_jumps_) a_.Bind(jump, poll);
  for (size_t jump : exit_jumps_) a_.Bind(jump, exit_);
  for (size_t jump : grow_jumps_) a_.Bind(jump, step_grow);
  for (const Fix& fix : fixes_) {
    size_t target;
    if (!Lookup(fix.target, &target)) return nullptr;
    a_.Bind(fix.at, target);
  }
  u64 bound = pushes_ * kValue;
  if (bound > static_cast<u64>(INT32_MAX)) return nullptr;
  for (size_t field : bound_fields_) a_.Patch32(field, static_cast<u32>(bound));

  auto* jit = new JitCode;
  jit->chunk = Arena().Place(a_.b.data(), a_.b.size(), &jit->code);
  if (!jit->chunk) {
    delete jit;
    return nullptr;
  }
  jit->size = a_.b.size();
  jit->map.resize(code_.size());
  for (size_t i = 0; i < code_.size(); ++i) {
    jit->map[i] = static_cast<u64>(code_[i].pc) << 32 | labels_[i];
  }
  return jit;
}

DefinedFunc::~DefinedFunc() {
  if (jit_) {
    Arena().Release(jit_->chunk, jit_->size);
    delete jit_;
  }
}

RunResult Thread::JitTryEnter(Trap::Ptr* out_trap, bool* entered) {
  *entered = false;
  if (frames_.empty() || trace_stream_) return RunResult::Ok;
  Frame& frame = frames_.back();
  if (!frame.inst) return RunResult::Ok;
  if (--jit_budget_ < 0) {
    jit_budget_ = kJitBudget;
    if (store_.PollInterrupt()) {
      *out_trap = Trap::New(store_, "WebAssembly execution interrupted");
      return RunResult::Trap;
    }
  }
  Object* object = store_.UnsafeObject(frame.func);
  if (object->kind() != ObjectKind::DefinedFunc) return RunResult::Ok;
  auto* func = static_cast<DefinedFunc*>(object);
  if (!func->jit_) {
    if (func->jit_failed_ || ++func->jit_heat_ < kJitHeat) return RunResult::Ok;
    func->jit_ = JitCompiler(*this, *func, frame.inst, frame.mod).Compile();
    JitStats& stats = Stats();
    if (!func->jit_) {
      func->jit_failed_ = true;
      ++stats.failures;
      return RunResult::Ok;
    }
    ++stats.functions;
    stats.bytes += func->jit_->size;
    if (stats.functions == stats.next_report) {
      stats.next_report *= 4;
      fprintf(stderr, "wasm-jit: %llu functions, %llu KiB, %llu not compiled\n",
              static_cast<unsigned long long>(stats.functions),
              static_cast<unsigned long long>(stats.bytes / 1024),
              static_cast<unsigned long long>(stats.failures));
    }
  }
  void* target = func->jit_->Find(frame.offset);
  if (!target) return RunResult::Ok;
  uintptr_t low = wasm_rt_native_stack_low();
  uintptr_t here = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  if (low == UINTPTR_MAX || here < low || here - low < kJitStackReserve) {
    return RunResult::Ok;
  }
  jit_result_ = RunResult::Ok;
  Trampolines().enter(this, target, func->jit_->entry(), frames_.size(),
                      values_.end());
  *entered = true;
  RunResult result = jit_result_;
  jit_result_ = RunResult::Ok;
  if (result != RunResult::Ok && jit_trap_) *out_trap = std::move(jit_trap_);
  jit_trap_.reset();
  return result;
}

void* Thread::JitStep(Thread* t, u32 pc, u32 next_pc, void* next_addr,
                      u64 depth) {
  Frame& frame = t->frames_.back();
  Ref func = frame.func;
  frame.offset = pc;
  Trap::Ptr trap;
  RunResult result = t->StepInternal(&trap);
  if (result == RunResult::Ok && t->frames_.size() > depth) {
    result = t->RunFrames(depth, &trap);
  }
  if (result != RunResult::Ok) {
    t->jit_result_ = result;
    t->jit_trap_ = std::move(trap);
    return nullptr;
  }
  // The activation continues only in its own frame and function (a tail
  // call or an unwind past it leaves compiled code to the interpreter).
  if (t->frames_.size() != depth) return nullptr;
  Frame& top = t->frames_.back();
  if (top.func != func) return nullptr;
  if (top.offset == next_pc) return next_addr;
  auto* defined = static_cast<DefinedFunc*>(t->store_.UnsafeObject(func));
  return defined->jit_ ? defined->jit_->Find(top.offset) : nullptr;
}

void* Thread::JitPoll(Thread* t, void* target_addr) {
  t->jit_budget_ = kJitBudget;
  if (t->store_.PollInterrupt()) {
    t->jit_result_ = RunResult::Trap;
    t->jit_trap_ = Trap::New(t->store_, "WebAssembly execution interrupted");
    return nullptr;
  }
  return target_addr;
}

Value* Thread::JitGrow(Thread* t, u64 bytes) {
  t->values_.Reserve(t->values_.size() + bytes / sizeof(Value));
  return t->values_.end();
}

}  // namespace interp
}  // namespace wabt
