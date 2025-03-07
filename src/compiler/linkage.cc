// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/linkage.h"

#include "src/codegen/assembler-inl.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/optimized-compilation-info.h"
#include "src/compiler/frame.h"
#include "src/compiler/osr.h"
#include "src/compiler/pipeline.h"

namespace v8 {
namespace internal {
namespace compiler {

namespace {

// Offsets from callee to caller frame, in slots.
constexpr int kFirstCallerSlotOffset = 1;
constexpr int kNoCallerSlotOffset = 0;

inline LinkageLocation regloc(Register reg, MachineType type) {
  return LinkageLocation::ForRegister(reg.code(), type);
}

inline LinkageLocation regloc(DoubleRegister reg, MachineType type) {
  return LinkageLocation::ForRegister(reg.code(), type);
}

}  // namespace


std::ostream& operator<<(std::ostream& os, const CallDescriptor::Kind& k) {
  switch (k) {
    case CallDescriptor::kCallCodeObject:
      os << "Code";
      break;
    case CallDescriptor::kCallJSFunction:
      os << "JS";
      break;
    case CallDescriptor::kCallAddress:
      os << "Addr";
      break;
#if V8_ENABLE_WEBASSEMBLY
    case CallDescriptor::kCallWasmCapiFunction:
      os << "WasmExit";
      break;
    case CallDescriptor::kCallWasmFunction:
      os << "WasmFunction";
      break;
    case CallDescriptor::kCallWasmImportWrapper:
      os << "WasmImportWrapper";
      break;
#endif  // V8_ENABLE_WEBASSEMBLY
    case CallDescriptor::kCallBuiltinPointer:
      os << "BuiltinPointer";
      break;
  }
  return os;
}


std::ostream& operator<<(std::ostream& os, const CallDescriptor& d) {
  // TODO(svenpanne) Output properties etc. and be less cryptic.
  return os << d.kind() << ":" << d.debug_name() << ":r" << d.ReturnCount()
            << "s" << d.ParameterSlotCount() << "i" << d.InputCount() << "f"
            << d.FrameStateCount();
}

MachineSignature* CallDescriptor::GetMachineSignature(Zone* zone) const {
  size_t param_count = ParameterCount();
  size_t return_count = ReturnCount();
  MachineType* types = zone->NewArray<MachineType>(param_count + return_count);
  int current = 0;
  for (size_t i = 0; i < return_count; ++i) {
    types[current++] = GetReturnType(i);
  }
  for (size_t i = 0; i < param_count; ++i) {
    types[current++] = GetParameterType(i);
  }
  return zone->New<MachineSignature>(return_count, param_count, types);
}

int CallDescriptor::GetStackParameterDelta(
    CallDescriptor const* tail_caller) const {
  // In the IsTailCallForTierUp case, the callee has
  // identical linkage and runtime arguments to the caller, thus the stack
  // parameter delta is 0. We don't explicitly pass the runtime arguments as
  // inputs to the TailCall node, since they already exist on the stack.
  if (IsTailCallForTierUp()) return 0;

  // Add padding if necessary before computing the stack parameter delta.
  int callee_slots_above_sp = AddArgumentPaddingSlots(GetOffsetToReturns());
  int tail_caller_slots_above_sp =
      AddArgumentPaddingSlots(tail_caller->GetOffsetToReturns());
  int stack_param_delta = callee_slots_above_sp - tail_caller_slots_above_sp;
  DCHECK(!ShouldPadArguments(stack_param_delta));
  return stack_param_delta;
}

int CallDescriptor::GetOffsetToFirstUnusedStackSlot() const {
  int offset = kFirstCallerSlotOffset;
  for (size_t i = 0; i < InputCount(); ++i) {
    LinkageLocation operand = GetInputLocation(i);
    if (!operand.IsRegister()) {
      DCHECK(operand.IsCallerFrameSlot());
      int slot_offset = -operand.GetLocation();
      offset = std::max(offset, slot_offset + operand.GetSizeInPointers());
    }
  }
  return offset;
}

int CallDescriptor::GetOffsetToReturns() const {
  // Find the return slot with the least offset relative to the callee.
  int offset = kNoCallerSlotOffset;
  for (size_t i = 0; i < ReturnCount(); ++i) {
    LinkageLocation operand = GetReturnLocation(i);
    if (!operand.IsRegister()) {
      DCHECK(operand.IsCallerFrameSlot());
      int slot_offset = -operand.GetLocation();
      offset = std::min(offset, slot_offset);
    }
  }
  // If there was a return slot, return the offset minus 1 slot.
  if (offset != kNoCallerSlotOffset) {
    return offset - 1;
  }

  // Otherwise, return the first slot after the parameters area, including
  // optional padding slots.
  int last_argument_slot = GetOffsetToFirstUnusedStackSlot() - 1;
  offset = AddArgumentPaddingSlots(last_argument_slot);

  DCHECK_IMPLIES(offset == 0, ParameterSlotCount() == 0);
  return offset;
}

int CallDescriptor::GetTaggedParameterSlots() const {
  int result = 0;
  for (size_t i = 0; i < InputCount(); ++i) {
    LinkageLocation operand = GetInputLocation(i);
    if (!operand.IsRegister() && operand.GetType().IsTagged()) {
      ++result;
    }
  }
  return result;
}

bool CallDescriptor::CanTailCall(const CallDescriptor* callee) const {
  if (ReturnCount() != callee->ReturnCount()) return false;
  const int stack_returns_delta =
      GetOffsetToReturns() - callee->GetOffsetToReturns();
  for (size_t i = 0; i < ReturnCount(); ++i) {
    if (GetReturnLocation(i).IsCallerFrameSlot() &&
        callee->GetReturnLocation(i).IsCallerFrameSlot()) {
      if (GetReturnLocation(i).AsCallerFrameSlot() + stack_returns_delta !=
          callee->GetReturnLocation(i).AsCallerFrameSlot()) {
        return false;
      }
    } else if (!LinkageLocation::IsSameLocation(GetReturnLocation(i),
                                                callee->GetReturnLocation(i))) {
      return false;
    }
  }
  return true;
}

// TODO(jkummerow, sigurds): Arguably frame size calculation should be
// keyed on code/frame type, not on CallDescriptor kind. Think about a
// good way to organize this logic.
int CallDescriptor::CalculateFixedFrameSize(CodeKind code_kind) const {
  switch (kind_) {
    case kCallJSFunction:
      return StandardFrameConstants::kFixedSlotCount;
    case kCallAddress:
#if V8_ENABLE_WEBASSEMBLY
      if (code_kind == CodeKind::C_WASM_ENTRY) {
        return CWasmEntryFrameConstants::kFixedSlotCount;
      }
#endif  // V8_ENABLE_WEBASSEMBLY
      return CommonFrameConstants::kFixedSlotCountAboveFp +
             CommonFrameConstants::kCPSlotCount;
    case kCallCodeObject:
    case kCallBuiltinPointer:
      return TypedFrameConstants::kFixedSlotCount;
#if V8_ENABLE_WEBASSEMBLY
    case kCallWasmFunction:
    case kCallWasmImportWrapper:
      return WasmFrameConstants::kFixedSlotCount;
    case kCallWasmCapiFunction:
      return WasmExitFrameConstants::kFixedSlotCount;
#endif  // V8_ENABLE_WEBASSEMBLY
  }
  UNREACHABLE();
}

CallDescriptor* Linkage::ComputeIncoming(Zone* zone,
                                         OptimizedCompilationInfo* info) {
#if V8_ENABLE_WEBASSEMBLY
  DCHECK(info->IsOptimizing() || info->IsWasm());
#else
  DCHECK(info->IsOptimizing());
#endif  // V8_ENABLE_WEBASSEMBLY
  if (!info->closure().is_null()) {
    // If we are compiling a JS function, use a JS call descriptor,
    // plus the receiver.
    SharedFunctionInfo shared = info->closure()->shared();
    return GetJSCallDescriptor(zone, info->is_osr(),
                               1 + shared.internal_formal_parameter_count(),
                               CallDescriptor::kCanUseRoots);
  }
  return nullptr;  // TODO(titzer): ?
}


// static
bool Linkage::NeedsFrameStateInput(Runtime::FunctionId function) {
  switch (function) {
    // Most runtime functions need a FrameState. A few chosen ones that we know
    // not to call into arbitrary JavaScript, not to throw, and not to lazily
    // deoptimize are allowlisted here and can be called without a FrameState.
    case Runtime::kAbort:
    case Runtime::kAllocateInOldGeneration:
    case Runtime::kCreateIterResultObject:
    case Runtime::kIncBlockCounter:
    case Runtime::kIsFunction:
    case Runtime::kNewClosure:
    case Runtime::kNewClosure_Tenured:
    case Runtime::kNewFunctionContext:
    case Runtime::kPushBlockContext:
    case Runtime::kPushCatchContext:
    case Runtime::kReThrow:
    case Runtime::kStringEqual:
    case Runtime::kStringLessThan:
    case Runtime::kStringLessThanOrEqual:
    case Runtime::kStringGreaterThan:
    case Runtime::kStringGreaterThanOrEqual:
    case Runtime::kToFastProperties:  // TODO(conradw): Is it safe?
    case Runtime::kTraceEnter:
    case Runtime::kTraceExit:
      return false;

    // Some inline intrinsics are also safe to call without a FrameState.
    case Runtime::kInlineCreateIterResultObject:
    case Runtime::kInlineIncBlockCounter:
    case Runtime::kInlineGeneratorClose:
    case Runtime::kInlineGeneratorGetResumeMode:
    case Runtime::kInlineCreateJSGeneratorObject:
    case Runtime::kInlineIsArray:
    case Runtime::kInlineIsJSReceiver:
    case Runtime::kInlineIsRegExp:
    case Runtime::kInlineIsSmi:
      return false;

    default:
      break;
  }

  // For safety, default to needing a FrameState unless allowlisted.
  return true;
}


bool CallDescriptor::UsesOnlyRegisters() const {
  for (size_t i = 0; i < InputCount(); ++i) {
    if (!GetInputLocation(i).IsRegister()) return false;
  }
  for (size_t i = 0; i < ReturnCount(); ++i) {
    if (!GetReturnLocation(i).IsRegister()) return false;
  }
  return true;
}


CallDescriptor* Linkage::GetRuntimeCallDescriptor(
    Zone* zone, Runtime::FunctionId function_id, int js_parameter_count,
    Operator::Properties properties, CallDescriptor::Flags flags) {
  const Runtime::Function* function = Runtime::FunctionForId(function_id);
  const int return_count = function->result_size;
  const char* debug_name = function->name;

  if (!Linkage::NeedsFrameStateInput(function_id)) {
    flags = static_cast<CallDescriptor::Flags>(
        flags & ~CallDescriptor::kNeedsFrameState);
  }

  return GetCEntryStubCallDescriptor(zone, return_count, js_parameter_count,
                                     debug_name, properties, flags);
}

CallDescriptor* Linkage::GetCEntryStubCallDescriptor(
    Zone* zone, int return_count, int js_parameter_count,
    const char* debug_name, Operator::Properties properties,
    CallDescriptor::Flags flags, StackArgumentOrder stack_order) {
  const size_t function_count = 1;
  const size_t num_args_count = 1;
  const size_t context_count = 1;
  const size_t parameter_count = function_count +
                                 static_cast<size_t>(js_parameter_count) +
                                 num_args_count + context_count;

  LocationSignature::Builder locations(zone, static_cast<size_t>(return_count),
                                       static_cast<size_t>(parameter_count));

  // Add returns.
  if (locations.return_count_ > 0) {
    locations.AddReturn(regloc(kReturnRegister0, MachineType::AnyTagged()));
  }
  if (locations.return_count_ > 1) {
    locations.AddReturn(regloc(kReturnRegister1, MachineType::AnyTagged()));
  }
  if (locations.return_count_ > 2) {
    locations.AddReturn(regloc(kReturnRegister2, MachineType::AnyTagged()));
  }

  // All parameters to the runtime call go on the stack.
  for (int i = 0; i < js_parameter_count; i++) {
    locations.AddParam(LinkageLocation::ForCallerFrameSlot(
        i - js_parameter_count, MachineType::AnyTagged()));
  }
  // Add runtime function itself.
  locations.AddParam(
      regloc(kRuntimeCallFunctionRegister, MachineType::Pointer()));

  // Add runtime call argument count.
  locations.AddParam(
      regloc(kRuntimeCallArgCountRegister, MachineType::Int32()));

  // Add context.
  locations.AddParam(regloc(kContextRegister, MachineType::AnyTagged()));

  // The target for runtime calls is a code object.
  MachineType target_type = MachineType::AnyTagged();
  LinkageLocation target_loc =
      LinkageLocation::ForAnyRegister(MachineType::AnyTagged());
  return zone->New<CallDescriptor>(     // --
      CallDescriptor::kCallCodeObject,  // kind
      target_type,                      // target MachineType
      target_loc,                       // target location
      locations.Build(),                // location_sig
      js_parameter_count,               // stack_parameter_count
      properties,                       // properties
      kNoCalleeSaved,                   // callee-saved
      kNoCalleeSaved,                   // callee-saved fp
      flags,                            // flags
      debug_name,                       // debug name
      stack_order);                     // stack order
}

CallDescriptor* Linkage::GetJSCallDescriptor(Zone* zone, bool is_osr,
                                             int js_parameter_count,
                                             CallDescriptor::Flags flags) {
  const size_t return_count = 1;
  const size_t context_count = 1;
  const size_t new_target_count = 1;
  const size_t num_args_count = 1;
  const size_t parameter_count =
      js_parameter_count + new_target_count + num_args_count + context_count;

  LocationSignature::Builder locations(zone, return_count, parameter_count);

  // All JS calls have exactly one return value.
  locations.AddReturn(regloc(kReturnRegister0, MachineType::AnyTagged()));

  // All parameters to JS calls go on the stack.
  for (int i = 0; i < js_parameter_count; i++) {
    int spill_slot_index = -i - 1;
    locations.AddParam(LinkageLocation::ForCallerFrameSlot(
        spill_slot_index, MachineType::AnyTagged()));
  }

  // Add JavaScript call new target value.
  locations.AddParam(
      regloc(kJavaScriptCallNewTargetRegister, MachineType::AnyTagged()));

  // Add JavaScript call argument count.
  locations.AddParam(
      regloc(kJavaScriptCallArgCountRegister, MachineType::Int32()));

  // Add context.
  locations.AddParam(regloc(kContextRegister, MachineType::AnyTagged()));

  // The target for JS function calls is the JSFunction object.
  MachineType target_type = MachineType::AnyTagged();
  // When entering into an OSR function from unoptimized code the JSFunction
  // is not in a register, but it is on the stack in the marker spill slot.
  LinkageLocation target_loc =
      is_osr ? LinkageLocation::ForSavedCallerFunction()
             : regloc(kJSFunctionRegister, MachineType::AnyTagged());
  return zone->New<CallDescriptor>(     // --
      CallDescriptor::kCallJSFunction,  // kind
      target_type,                      // target MachineType
      target_loc,                       // target location
      locations.Build(),                // location_sig
      js_parameter_count,               // stack_parameter_count
      Operator::kNoProperties,          // properties
      kNoCalleeSaved,                   // callee-saved
      kNoCalleeSaved,                   // callee-saved fp
      flags,                            // flags
      "js-call");                       // debug name
}

// TODO(turbofan): cache call descriptors for code stub calls.
// TODO(jgruber): Clean up stack parameter count handling. The descriptor
// already knows the formal stack parameter count and ideally only additional
// stack parameters should be passed into this method. All call-sites should
// be audited for correctness (e.g. many used to assume a stack parameter count
// of 0).
CallDescriptor* Linkage::GetStubCallDescriptor(
    Zone* zone, const CallInterfaceDescriptor& descriptor,
    int stack_parameter_count, CallDescriptor::Flags flags,
    Operator::Properties properties, StubCallMode stub_mode) {
  const int register_parameter_count = descriptor.GetRegisterParameterCount();
  const int js_parameter_count =
      register_parameter_count + stack_parameter_count;
  const int context_count = descriptor.HasContextParameter() ? 1 : 0;
  const size_t parameter_count =
      static_cast<size_t>(js_parameter_count + context_count);

  DCHECK_GE(stack_parameter_count, descriptor.GetStackParameterCount());

  size_t return_count = descriptor.GetReturnCount();
  LocationSignature::Builder locations(zone, return_count, parameter_count);

  // Add returns.
  static constexpr Register return_registers[] = {
      kReturnRegister0, kReturnRegister1, kReturnRegister2};
  size_t num_returns = 0;
  size_t num_fp_returns = 0;
  for (size_t i = 0; i < locations.return_count_; i++) {
    MachineType type = descriptor.GetReturnType(static_cast<int>(i));
    if (IsFloatingPoint(type.representation())) {
      DCHECK_LT(num_fp_returns, 1);  // Only 1 FP return is supported.
      locations.AddReturn(regloc(kFPReturnRegister0, type));
      num_fp_returns++;
    } else {
      DCHECK_LT(num_returns, arraysize(return_registers));
      locations.AddReturn(regloc(return_registers[num_returns], type));
      num_returns++;
    }
  }

  // Add parameters in registers and on the stack.
  for (int i = 0; i < js_parameter_count; i++) {
    if (i < register_parameter_count) {
      // The first parameters go in registers.
      // TODO(bbudge) Add floating point registers to the InterfaceDescriptor
      // and use them for FP types. Currently, this works because on most
      // platforms, all FP registers are available for use. On ia32, xmm0 is
      // not allocatable and so we must work around that with platform-specific
      // descriptors, adjusting the GP register set to avoid eax, which has
      // register code 0.
      Register reg = descriptor.GetRegisterParameter(i);
      MachineType type = descriptor.GetParameterType(i);
      locations.AddParam(regloc(reg, type));
    } else {
      // The rest of the parameters go on the stack.
      int stack_slot = i - register_parameter_count - stack_parameter_count;
      locations.AddParam(LinkageLocation::ForCallerFrameSlot(
          stack_slot, i < descriptor.GetParameterCount()
                          ? descriptor.GetParameterType(i)
                          : MachineType::AnyTagged()));
    }
  }
  // Add context.
  if (context_count) {
    locations.AddParam(regloc(kContextRegister, MachineType::AnyTagged()));
  }

  // The target for stub calls depends on the requested mode.
  CallDescriptor::Kind kind;
  MachineType target_type;
  switch (stub_mode) {
    case StubCallMode::kCallCodeObject:
      kind = CallDescriptor::kCallCodeObject;
      target_type = MachineType::AnyTagged();
      break;
#if V8_ENABLE_WEBASSEMBLY
    case StubCallMode::kCallWasmRuntimeStub:
      kind = CallDescriptor::kCallWasmFunction;
      target_type = MachineType::Pointer();
      break;
#endif  // V8_ENABLE_WEBASSEMBLY
    case StubCallMode::kCallBuiltinPointer:
      kind = CallDescriptor::kCallBuiltinPointer;
      target_type = MachineType::AnyTagged();
      break;
  }

  RegList allocatable_registers = descriptor.allocatable_registers();
  RegList callee_saved_registers = kNoCalleeSaved;
  if (descriptor.CalleeSaveRegisters()) {
    callee_saved_registers = allocatable_registers;
    DCHECK(callee_saved_registers);
  }
  LinkageLocation target_loc = LinkageLocation::ForAnyRegister(target_type);
  return zone->New<CallDescriptor>(          // --
      kind,                                  // kind
      target_type,                           // target MachineType
      target_loc,                            // target location
      locations.Build(),                     // location_sig
      stack_parameter_count,                 // stack_parameter_count
      properties,                            // properties
      callee_saved_registers,                // callee-saved registers
      kNoCalleeSaved,                        // callee-saved fp
      CallDescriptor::kCanUseRoots | flags,  // flags
      descriptor.DebugName(),                // debug name
      descriptor.GetStackArgumentOrder(),    // stack order
      allocatable_registers);
}

// static
CallDescriptor* Linkage::GetBytecodeDispatchCallDescriptor(
    Zone* zone, const CallInterfaceDescriptor& descriptor,
    int stack_parameter_count) {
  const int register_parameter_count = descriptor.GetRegisterParameterCount();
  const int parameter_count = register_parameter_count + stack_parameter_count;

  DCHECK_EQ(descriptor.GetReturnCount(), 1);
  LocationSignature::Builder locations(zone, 1, parameter_count);

  locations.AddReturn(regloc(kReturnRegister0, descriptor.GetReturnType(0)));

  // Add parameters in registers and on the stack.
  for (int i = 0; i < parameter_count; i++) {
    if (i < register_parameter_count) {
      // The first parameters go in registers.
      Register reg = descriptor.GetRegisterParameter(i);
      MachineType type = descriptor.GetParameterType(i);
      locations.AddParam(regloc(reg, type));
    } else {
      // The rest of the parameters go on the stack.
      int stack_slot = i - register_parameter_count - stack_parameter_count;
      locations.AddParam(LinkageLocation::ForCallerFrameSlot(
          stack_slot, MachineType::AnyTagged()));
    }
  }

  // The target for interpreter dispatches is a code entry address.
  MachineType target_type = MachineType::Pointer();
  LinkageLocation target_loc = LinkageLocation::ForAnyRegister(target_type);
  const CallDescriptor::Flags kFlags =
      CallDescriptor::kCanUseRoots | CallDescriptor::kFixedTargetRegister;
  return zone->New<CallDescriptor>(  // --
      CallDescriptor::kCallAddress,  // kind
      target_type,                   // target MachineType
      target_loc,                    // target location
      locations.Build(),             // location_sig
      stack_parameter_count,         // stack_parameter_count
      Operator::kNoProperties,       // properties
      kNoCalleeSaved,                // callee-saved registers
      kNoCalleeSaved,                // callee-saved fp
      kFlags,                        // flags
      descriptor.DebugName());
}

LinkageLocation Linkage::GetOsrValueLocation(int index) const {
  CHECK(incoming_->IsJSFunctionCall());
  int parameter_count = static_cast<int>(incoming_->JSParameterCount() - 1);
  int first_stack_slot = OsrHelper::FirstStackSlotIndex(parameter_count);

  if (index == kOsrContextSpillSlotIndex) {
    // Context. Use the parameter location of the context spill slot.
    // Parameter (arity + 2) is special for the context of the function frame.
    // >> context_index = target + receiver + params + new_target + #args
    int context_index = 1 + 1 + parameter_count + 1 + 1;
    return incoming_->GetInputLocation(context_index);
  } else if (index >= first_stack_slot) {
    // Local variable stored in this (callee) stack.
    int spill_index =
        index - first_stack_slot + StandardFrameConstants::kFixedSlotCount;
    return LinkageLocation::ForCalleeFrameSlot(spill_index,
                                               MachineType::AnyTagged());
  } else {
    // Parameter. Use the assigned location from the incoming call descriptor.
    int parameter_index = 1 + index;  // skip index 0, which is the target.
    return incoming_->GetInputLocation(parameter_index);
  }
}

namespace {
inline bool IsTaggedReg(const LinkageLocation& loc, Register reg) {
  return loc.IsRegister() && loc.AsRegister() == reg.code() &&
         loc.GetType().representation() ==
             MachineRepresentation::kTaggedPointer;
}
}  // namespace

bool Linkage::ParameterHasSecondaryLocation(int index) const {
  // TODO(titzer): this should be configurable, not call-type specific.
  if (incoming_->IsJSFunctionCall()) {
    LinkageLocation loc = GetParameterLocation(index);
    return IsTaggedReg(loc, kJSFunctionRegister) ||
           IsTaggedReg(loc, kContextRegister);
  }
#if V8_ENABLE_WEBASSEMBLY
  if (incoming_->IsWasmFunctionCall()) {
    LinkageLocation loc = GetParameterLocation(index);
    return IsTaggedReg(loc, kWasmInstanceRegister);
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  return false;
}

LinkageLocation Linkage::GetParameterSecondaryLocation(int index) const {
  // TODO(titzer): these constants are necessary due to offset/slot# mismatch
  static const int kJSContextSlot = 2 + StandardFrameConstants::kCPSlotCount;
  static const int kJSFunctionSlot = 3 + StandardFrameConstants::kCPSlotCount;

  DCHECK(ParameterHasSecondaryLocation(index));
  LinkageLocation loc = GetParameterLocation(index);

  // TODO(titzer): this should be configurable, not call-type specific.
  if (incoming_->IsJSFunctionCall()) {
    if (IsTaggedReg(loc, kJSFunctionRegister)) {
      return LinkageLocation::ForCalleeFrameSlot(kJSFunctionSlot,
                                                 MachineType::AnyTagged());
    } else {
      DCHECK(IsTaggedReg(loc, kContextRegister));
      return LinkageLocation::ForCalleeFrameSlot(kJSContextSlot,
                                                 MachineType::AnyTagged());
    }
  }
#if V8_ENABLE_WEBASSEMBLY
  static const int kWasmInstanceSlot = 3 + StandardFrameConstants::kCPSlotCount;
  if (incoming_->IsWasmFunctionCall()) {
    DCHECK(IsTaggedReg(loc, kWasmInstanceRegister));
    return LinkageLocation::ForCalleeFrameSlot(kWasmInstanceSlot,
                                               MachineType::AnyTagged());
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  UNREACHABLE();
  return LinkageLocation::ForCalleeFrameSlot(0, MachineType::AnyTagged());
}


}  // namespace compiler
}  // namespace internal
}  // namespace v8
