//===-- AMDGPUPromoteAlloca.cpp - Promote Allocas -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass eliminates allocas by either converting them into vectors or
// by migrating them to local address space.
//
//===----------------------------------------------------------------------===//

#include "AMDGPU.h"
#include "AMDGPUSubtarget.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "amdgpu-promote-alloca"

using namespace llvm;

namespace {

// FIXME: This can create globals so should be a module pass.
class AMDGPUPromoteAlloca : public FunctionPass {
private:
  const TargetMachine *TM;
  Module *Mod;
  const DataLayout *DL;
  MDNode *MaxWorkGroupSizeRange;

  // FIXME: This should be per-kernel.
  uint32_t LocalMemLimit;
  uint32_t CurrentLocalMemUsage;

  bool IsAMDGCN;
  bool IsAMDHSA;

  std::pair<Value *, Value *> getLocalSizeYZ(IRBuilder<> &Builder);
  Value *getWorkitemID(IRBuilder<> &Builder, unsigned N);

  /// BaseAlloca is the alloca root the search started from.
  /// Val may be that alloca or a recursive user of it.
  bool collectUsesWithPtrTypes(Value *BaseAlloca,
                               Value *Val,
                               std::vector<Value*> &WorkList) const;

  /// Val is a derived pointer from Alloca. OpIdx0/OpIdx1 are the operand
  /// indices to an instruction with 2 pointer inputs (e.g. select, icmp).
  /// Returns true if both operands are derived from the same alloca. Val should
  /// be the same value as one of the input operands of UseInst.
  bool binaryOpIsDerivedFromSameAlloca(Value *Alloca, Value *Val,
                                       Instruction *UseInst,
                                       int OpIdx0, int OpIdx1) const;

public:
  static char ID;

  AMDGPUPromoteAlloca(const TargetMachine *TM_ = nullptr) :
    FunctionPass(ID),
    TM(TM_),
    Mod(nullptr),
    DL(nullptr),
    MaxWorkGroupSizeRange(nullptr),
    LocalMemLimit(0),
    CurrentLocalMemUsage(0),
    IsAMDGCN(false),
    IsAMDHSA(false) { }

  bool doInitialization(Module &M) override;
  bool runOnFunction(Function &F) override;

  StringRef getPassName() const override { return "AMDGPU Promote Alloca"; }

  void handleAlloca(AllocaInst &I);

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    FunctionPass::getAnalysisUsage(AU);
  }
};

} // End anonymous namespace

char AMDGPUPromoteAlloca::ID = 0;

INITIALIZE_TM_PASS(AMDGPUPromoteAlloca, DEBUG_TYPE,
                   "AMDGPU promote alloca to vector or LDS", false, false)

char &llvm::AMDGPUPromoteAllocaID = AMDGPUPromoteAlloca::ID;


bool AMDGPUPromoteAlloca::doInitialization(Module &M) {
  if (!TM)
    return false;

  Mod = &M;
  DL = &Mod->getDataLayout();

  // The maximum workitem id.
  //
  // FIXME: Should get as subtarget property. Usually runtime enforced max is
  // 256.
  MDBuilder MDB(Mod->getContext());
  MaxWorkGroupSizeRange = MDB.createRange(APInt(32, 0), APInt(32, 2048));

  const Triple &TT = TM->getTargetTriple();

  IsAMDGCN = TT.getArch() == Triple::amdgcn;
  IsAMDHSA = TT.getOS() == Triple::AMDHSA;

  return false;
}

bool AMDGPUPromoteAlloca::runOnFunction(Function &F) {
  if (!TM || skipFunction(F))
    return false;

  const AMDGPUSubtarget &ST = TM->getSubtarget<AMDGPUSubtarget>(F);
  if (!ST.isPromoteAllocaEnabled())
    return false;

  FunctionType *FTy = F.getFunctionType();

  // If the function has any arguments in the local address space, then it's
  // possible these arguments require the entire local memory space, so
  // we cannot use local memory in the pass.
  for (Type *ParamTy : FTy->params()) {
    PointerType *PtrTy = dyn_cast<PointerType>(ParamTy);
    if (PtrTy && PtrTy->getAddressSpace() == AMDGPUAS::LOCAL_ADDRESS) {
      LocalMemLimit = 0;
      DEBUG(dbgs() << "Function has local memory argument. Promoting to "
                      "local memory disabled.\n");
      return false;
    }
  }

  LocalMemLimit = ST.getLocalMemorySize();
  if (LocalMemLimit == 0)
    return false;

  const DataLayout &DL = Mod->getDataLayout();

  // Check how much local memory is being used by global objects
  CurrentLocalMemUsage = 0;
  for (GlobalVariable &GV : Mod->globals()) {
    if (GV.getType()->getAddressSpace() != AMDGPUAS::LOCAL_ADDRESS)
      continue;

    for (const User *U : GV.users()) {
      const Instruction *Use = dyn_cast<Instruction>(U);
      if (!Use)
        continue;

      if (Use->getParent()->getParent() == &F) {
        unsigned Align = GV.getAlignment();
        if (Align == 0)
          Align = DL.getABITypeAlignment(GV.getValueType());

        // FIXME: Try to account for padding here. The padding is currently
        // determined from the inverse order of uses in the function. I'm not
        // sure if the use list order is in any way connected to this, so the
        // total reported size is likely incorrect.
        uint64_t AllocSize = DL.getTypeAllocSize(GV.getValueType());
        CurrentLocalMemUsage = alignTo(CurrentLocalMemUsage, Align);
        CurrentLocalMemUsage += AllocSize;
        break;
      }
    }
  }

  unsigned MaxOccupancy = ST.getOccupancyWithLocalMemSize(CurrentLocalMemUsage);

  // Restrict local memory usage so that we don't drastically reduce occupancy,
  // unless it is already significantly reduced.

  // TODO: Have some sort of hint or other heuristics to guess occupancy based
  // on other factors..
  unsigned OccupancyHint = ST.getWavesPerEU(F).second;
  if (OccupancyHint == 0)
    OccupancyHint = 7;

  // Clamp to max value.
  OccupancyHint = std::min(OccupancyHint, ST.getMaxWavesPerEU());

  // Check the hint but ignore it if it's obviously wrong from the existing LDS
  // usage.
  MaxOccupancy = std::min(OccupancyHint, MaxOccupancy);


  // Round up to the next tier of usage.
  unsigned MaxSizeWithWaveCount
    = ST.getMaxLocalMemSizeWithWaveCount(MaxOccupancy);

  // Program is possibly broken by using more local mem than available.
  if (CurrentLocalMemUsage > MaxSizeWithWaveCount)
    return false;

  LocalMemLimit = MaxSizeWithWaveCount;

  DEBUG(
    dbgs() << F.getName() << " uses " << CurrentLocalMemUsage << " bytes of LDS\n"
    << "  Rounding size to " << MaxSizeWithWaveCount
    << " with a maximum occupancy of " << MaxOccupancy << '\n'
    << " and " << (LocalMemLimit - CurrentLocalMemUsage)
    << " available for promotion\n"
  );

  BasicBlock &EntryBB = *F.begin();
  for (auto I = EntryBB.begin(), E = EntryBB.end(); I != E; ) {
    AllocaInst *AI = dyn_cast<AllocaInst>(I);

    ++I;
    if (AI)
      handleAlloca(*AI);
  }

  return true;
}

std::pair<Value *, Value *>
AMDGPUPromoteAlloca::getLocalSizeYZ(IRBuilder<> &Builder) {
  if (!IsAMDHSA) {
    Function *LocalSizeYFn
      = Intrinsic::getDeclaration(Mod, Intrinsic::r600_read_local_size_y);
    Function *LocalSizeZFn
      = Intrinsic::getDeclaration(Mod, Intrinsic::r600_read_local_size_z);

    CallInst *LocalSizeY = Builder.CreateCall(LocalSizeYFn, {});
    CallInst *LocalSizeZ = Builder.CreateCall(LocalSizeZFn, {});

    LocalSizeY->setMetadata(LLVMContext::MD_range, MaxWorkGroupSizeRange);
    LocalSizeZ->setMetadata(LLVMContext::MD_range, MaxWorkGroupSizeRange);

    return std::make_pair(LocalSizeY, LocalSizeZ);
  }

  // We must read the size out of the dispatch pointer.
  assert(IsAMDGCN);

  // We are indexing into this struct, and want to extract the workgroup_size_*
  // fields.
  //
  //   typedef struct hsa_kernel_dispatch_packet_s {
  //     uint16_t header;
  //     uint16_t setup;
  //     uint16_t workgroup_size_x ;
  //     uint16_t workgroup_size_y;
  //     uint16_t workgroup_size_z;
  //     uint16_t reserved0;
  //     uint32_t grid_size_x ;
  //     uint32_t grid_size_y ;
  //     uint32_t grid_size_z;
  //
  //     uint32_t private_segment_size;
  //     uint32_t group_segment_size;
  //     uint64_t kernel_object;
  //
  // #ifdef HSA_LARGE_MODEL
  //     void *kernarg_address;
  // #elif defined HSA_LITTLE_ENDIAN
  //     void *kernarg_address;
  //     uint32_t reserved1;
  // #else
  //     uint32_t reserved1;
  //     void *kernarg_address;
  // #endif
  //     uint64_t reserved2;
  //     hsa_signal_t completion_signal; // uint64_t wrapper
  //   } hsa_kernel_dispatch_packet_t
  //
  Function *DispatchPtrFn
    = Intrinsic::getDeclaration(Mod, Intrinsic::amdgcn_dispatch_ptr);

  CallInst *DispatchPtr = Builder.CreateCall(DispatchPtrFn, {});
  DispatchPtr->addAttribute(AttributeSet::ReturnIndex, Attribute::NoAlias);
  DispatchPtr->addAttribute(AttributeSet::ReturnIndex, Attribute::NonNull);

  // Size of the dispatch packet struct.
  DispatchPtr->addDereferenceableAttr(AttributeSet::ReturnIndex, 64);

  Type *I32Ty = Type::getInt32Ty(Mod->getContext());
  Value *CastDispatchPtr = Builder.CreateBitCast(
    DispatchPtr, PointerType::get(I32Ty, AMDGPUAS::CONSTANT_ADDRESS));

  // We could do a single 64-bit load here, but it's likely that the basic
  // 32-bit and extract sequence is already present, and it is probably easier
  // to CSE this. The loads should be mergable later anyway.
  Value *GEPXY = Builder.CreateConstInBoundsGEP1_64(CastDispatchPtr, 1);
  LoadInst *LoadXY = Builder.CreateAlignedLoad(GEPXY, 4);

  Value *GEPZU = Builder.CreateConstInBoundsGEP1_64(CastDispatchPtr, 2);
  LoadInst *LoadZU = Builder.CreateAlignedLoad(GEPZU, 4);

  MDNode *MD = llvm::MDNode::get(Mod->getContext(), None);
  LoadXY->setMetadata(LLVMContext::MD_invariant_load, MD);
  LoadZU->setMetadata(LLVMContext::MD_invariant_load, MD);
  LoadZU->setMetadata(LLVMContext::MD_range, MaxWorkGroupSizeRange);

  // Extract y component. Upper half of LoadZU should be zero already.
  Value *Y = Builder.CreateLShr(LoadXY, 16);

  return std::make_pair(Y, LoadZU);
}

Value *AMDGPUPromoteAlloca::getWorkitemID(IRBuilder<> &Builder, unsigned N) {
  Intrinsic::ID IntrID = Intrinsic::ID::not_intrinsic;

  switch (N) {
  case 0:
    IntrID = IsAMDGCN ? Intrinsic::amdgcn_workitem_id_x
      : Intrinsic::r600_read_tidig_x;
    break;
  case 1:
    IntrID = IsAMDGCN ? Intrinsic::amdgcn_workitem_id_y
      : Intrinsic::r600_read_tidig_y;
    break;

  case 2:
    IntrID = IsAMDGCN ? Intrinsic::amdgcn_workitem_id_z
      : Intrinsic::r600_read_tidig_z;
    break;
  default:
    llvm_unreachable("invalid dimension");
  }

  Function *WorkitemIdFn = Intrinsic::getDeclaration(Mod, IntrID);
  CallInst *CI = Builder.CreateCall(WorkitemIdFn);
  CI->setMetadata(LLVMContext::MD_range, MaxWorkGroupSizeRange);

  return CI;
}

static VectorType *arrayTypeToVecType(Type *ArrayTy) {
  return VectorType::get(ArrayTy->getArrayElementType(),
                         ArrayTy->getArrayNumElements());
}

static Value *
calculateVectorIndex(Value *Ptr,
                     const std::map<GetElementPtrInst *, Value *> &GEPIdx) {
  GetElementPtrInst *GEP = cast<GetElementPtrInst>(Ptr);

  auto I = GEPIdx.find(GEP);
  return I == GEPIdx.end() ? nullptr : I->second;
}

static Value* GEPToVectorIndex(GetElementPtrInst *GEP) {
  // FIXME we only support simple cases
  if (GEP->getNumOperands() != 3)
    return nullptr;

  ConstantInt *I0 = dyn_cast<ConstantInt>(GEP->getOperand(1));
  if (!I0 || !I0->isZero())
    return nullptr;

  return GEP->getOperand(2);
}

// Not an instruction handled below to turn into a vector.
//
// TODO: Check isTriviallyVectorizable for calls and handle other
// instructions.
static bool canVectorizeInst(Instruction *Inst, User *User) {
  switch (Inst->getOpcode()) {
  case Instruction::Load:
  case Instruction::BitCast:
  case Instruction::AddrSpaceCast:
    return true;
  case Instruction::Store: {
    // Must be the stored pointer operand, not a stored value.
    StoreInst *SI = cast<StoreInst>(Inst);
    return SI->getPointerOperand() == User;
  }
  default:
    return false;
  }
}

static bool tryPromoteAllocaToVector(AllocaInst *Alloca) {
  ArrayType *AllocaTy = dyn_cast<ArrayType>(Alloca->getAllocatedType());

  DEBUG(dbgs() << "Alloca candidate for vectorization\n");

  // FIXME: There is no reason why we can't support larger arrays, we
  // are just being conservative for now.
  if (!AllocaTy ||
      AllocaTy->getElementType()->isVectorTy() ||
      AllocaTy->getNumElements() > 4 ||
      AllocaTy->getNumElements() < 2) {
    DEBUG(dbgs() << "  Cannot convert type to vector\n");
    return false;
  }

  std::map<GetElementPtrInst*, Value*> GEPVectorIdx;
  std::vector<Value*> WorkList;
  for (User *AllocaUser : Alloca->users()) {
    GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(AllocaUser);
    if (!GEP) {
      if (!canVectorizeInst(cast<Instruction>(AllocaUser), Alloca))
        return false;

      WorkList.push_back(AllocaUser);
      continue;
    }

    Value *Index = GEPToVectorIndex(GEP);

    // If we can't compute a vector index from this GEP, then we can't
    // promote this alloca to vector.
    if (!Index) {
      DEBUG(dbgs() << "  Cannot compute vector index for GEP " << *GEP << '\n');
      return false;
    }

    GEPVectorIdx[GEP] = Index;
    for (User *GEPUser : AllocaUser->users()) {
      if (!canVectorizeInst(cast<Instruction>(GEPUser), AllocaUser))
        return false;

      WorkList.push_back(GEPUser);
    }
  }

  VectorType *VectorTy = arrayTypeToVecType(AllocaTy);

  DEBUG(dbgs() << "  Converting alloca to vector "
        << *AllocaTy << " -> " << *VectorTy << '\n');

  for (Value *V : WorkList) {
    Instruction *Inst = cast<Instruction>(V);
    IRBuilder<> Builder(Inst);
    switch (Inst->getOpcode()) {
    case Instruction::Load: {
      Type *VecPtrTy = VectorTy->getPointerTo(AMDGPUAS::PRIVATE_ADDRESS);
      Value *Ptr = Inst->getOperand(0);
      Value *Index = calculateVectorIndex(Ptr, GEPVectorIdx);

      Value *BitCast = Builder.CreateBitCast(Alloca, VecPtrTy);
      Value *VecValue = Builder.CreateLoad(BitCast);
      Value *ExtractElement = Builder.CreateExtractElement(VecValue, Index);
      Inst->replaceAllUsesWith(ExtractElement);
      Inst->eraseFromParent();
      break;
    }
    case Instruction::Store: {
      Type *VecPtrTy = VectorTy->getPointerTo(AMDGPUAS::PRIVATE_ADDRESS);

      Value *Ptr = Inst->getOperand(1);
      Value *Index = calculateVectorIndex(Ptr, GEPVectorIdx);
      Value *BitCast = Builder.CreateBitCast(Alloca, VecPtrTy);
      Value *VecValue = Builder.CreateLoad(BitCast);
      Value *NewVecValue = Builder.CreateInsertElement(VecValue,
                                                       Inst->getOperand(0),
                                                       Index);
      Builder.CreateStore(NewVecValue, BitCast);
      Inst->eraseFromParent();
      break;
    }
    case Instruction::BitCast:
    case Instruction::AddrSpaceCast:
      break;

    default:
      llvm_unreachable("Inconsistency in instructions promotable to vector");
    }
  }
  return true;
}

static bool isCallPromotable(CallInst *CI) {
  IntrinsicInst *II = dyn_cast<IntrinsicInst>(CI);
  if (!II)
    return false;

  switch (II->getIntrinsicID()) {
  case Intrinsic::memcpy:
  case Intrinsic::memmove:
  case Intrinsic::memset:
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
  case Intrinsic::invariant_start:
  case Intrinsic::invariant_end:
  case Intrinsic::invariant_group_barrier:
  case Intrinsic::objectsize:
    return true;
  default:
    return false;
  }
}

bool AMDGPUPromoteAlloca::binaryOpIsDerivedFromSameAlloca(Value *BaseAlloca,
                                                          Value *Val,
                                                          Instruction *Inst,
                                                          int OpIdx0,
                                                          int OpIdx1) const {
  // Figure out which operand is the one we might not be promoting.
  Value *OtherOp = Inst->getOperand(OpIdx0);
  if (Val == OtherOp)
    OtherOp = Inst->getOperand(OpIdx1);

  if (isa<ConstantPointerNull>(OtherOp))
    return true;

  Value *OtherObj = GetUnderlyingObject(OtherOp, *DL);
  if (!isa<AllocaInst>(OtherObj))
    return false;

  // TODO: We should be able to replace undefs with the right pointer type.

  // TODO: If we know the other base object is another promotable
  // alloca, not necessarily this alloca, we can do this. The
  // important part is both must have the same address space at
  // the end.
  if (OtherObj != BaseAlloca) {
    DEBUG(dbgs() << "Found a binary instruction with another alloca object\n");
    return false;
  }

  return true;
}

bool AMDGPUPromoteAlloca::collectUsesWithPtrTypes(
  Value *BaseAlloca,
  Value *Val,
  std::vector<Value*> &WorkList) const {

  for (User *User : Val->users()) {
    if (is_contained(WorkList, User))
      continue;

    if (CallInst *CI = dyn_cast<CallInst>(User)) {
      if (!isCallPromotable(CI))
        return false;

      WorkList.push_back(User);
      continue;
    }

    Instruction *UseInst = cast<Instruction>(User);
    if (UseInst->getOpcode() == Instruction::PtrToInt)
      return false;

    if (LoadInst *LI = dyn_cast<LoadInst>(UseInst)) {
      if (LI->isVolatile())
        return false;

      continue;
    }

    if (StoreInst *SI = dyn_cast<StoreInst>(UseInst)) {
      if (SI->isVolatile())
        return false;

      // Reject if the stored value is not the pointer operand.
      if (SI->getPointerOperand() != Val)
        return false;
    } else if (AtomicRMWInst *RMW = dyn_cast<AtomicRMWInst>(UseInst)) {
      if (RMW->isVolatile())
        return false;
    } else if (AtomicCmpXchgInst *CAS = dyn_cast<AtomicCmpXchgInst>(UseInst)) {
      if (CAS->isVolatile())
        return false;
    }

    // Only promote a select if we know that the other select operand
    // is from another pointer that will also be promoted.
    if (ICmpInst *ICmp = dyn_cast<ICmpInst>(UseInst)) {
      if (!binaryOpIsDerivedFromSameAlloca(BaseAlloca, Val, ICmp, 0, 1))
        return false;

      // May need to rewrite constant operands.
      WorkList.push_back(ICmp);
    }

    if (!User->getType()->isPointerTy())
      continue;

    if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(UseInst)) {
      // Be conservative if an address could be computed outside the bounds of
      // the alloca.
      if (!GEP->isInBounds())
        return false;
    }

    // Only promote a select if we know that the other select operand is from
    // another pointer that will also be promoted.
    if (SelectInst *SI = dyn_cast<SelectInst>(UseInst)) {
      if (!binaryOpIsDerivedFromSameAlloca(BaseAlloca, Val, SI, 1, 2))
        return false;
    }

    // Repeat for phis.
    if (PHINode *Phi = dyn_cast<PHINode>(UseInst)) {
      // TODO: Handle more complex cases. We should be able to replace loops
      // over arrays.
      switch (Phi->getNumIncomingValues()) {
      case 1:
        break;
      case 2:
        if (!binaryOpIsDerivedFromSameAlloca(BaseAlloca, Val, Phi, 0, 1))
          return false;
        break;
      default:
        return false;
      }
    }

    WorkList.push_back(User);
    if (!collectUsesWithPtrTypes(BaseAlloca, User, WorkList))
      return false;
  }

  return true;
}

// FIXME: Should try to pick the most likely to be profitable allocas first.
void AMDGPUPromoteAlloca::handleAlloca(AllocaInst &I) {
  // Array allocations are probably not worth handling, since an allocation of
  // the array type is the canonical form.
  if (!I.isStaticAlloca() || I.isArrayAllocation())
    return;

  IRBuilder<> Builder(&I);

  // First try to replace the alloca with a vector
  Type *AllocaTy = I.getAllocatedType();

  DEBUG(dbgs() << "Trying to promote " << I << '\n');

  if (tryPromoteAllocaToVector(&I)) {
    DEBUG(dbgs() << " alloca is not a candidate for vectorization.\n");
    return;
  }

  const Function &ContainingFunction = *I.getParent()->getParent();

  // Don't promote the alloca to LDS for shader calling conventions as the work
  // item ID intrinsics are not supported for these calling conventions.
  // Furthermore not all LDS is available for some of the stages.
  if (AMDGPU::isShader(ContainingFunction.getCallingConv()))
    return;

  const AMDGPUSubtarget &ST =
    TM->getSubtarget<AMDGPUSubtarget>(ContainingFunction);
  // FIXME: We should also try to get this value from the reqd_work_group_size
  // function attribute if it is available.
  unsigned WorkGroupSize = ST.getFlatWorkGroupSizes(ContainingFunction).second;

  const DataLayout &DL = Mod->getDataLayout();

  unsigned Align = I.getAlignment();
  if (Align == 0)
    Align = DL.getABITypeAlignment(I.getAllocatedType());

  // FIXME: This computed padding is likely wrong since it depends on inverse
  // usage order.
  //
  // FIXME: It is also possible that if we're allowed to use all of the memory
  // could could end up using more than the maximum due to alignment padding.

  uint32_t NewSize = alignTo(CurrentLocalMemUsage, Align);
  uint32_t AllocSize = WorkGroupSize * DL.getTypeAllocSize(AllocaTy);
  NewSize += AllocSize;

  if (NewSize > LocalMemLimit) {
    DEBUG(dbgs() << "  " << AllocSize
          << " bytes of local memory not available to promote\n");
    return;
  }

  CurrentLocalMemUsage = NewSize;

  std::vector<Value*> WorkList;

  if (!collectUsesWithPtrTypes(&I, &I, WorkList)) {
    DEBUG(dbgs() << " Do not know how to convert all uses\n");
    return;
  }

  DEBUG(dbgs() << "Promoting alloca to local memory\n");

  Function *F = I.getParent()->getParent();

  Type *GVTy = ArrayType::get(I.getAllocatedType(), WorkGroupSize);
  GlobalVariable *GV = new GlobalVariable(
      *Mod, GVTy, false, GlobalValue::InternalLinkage,
      UndefValue::get(GVTy),
      Twine(F->getName()) + Twine('.') + I.getName(),
      nullptr,
      GlobalVariable::NotThreadLocal,
      AMDGPUAS::LOCAL_ADDRESS);
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  GV->setAlignment(I.getAlignment());

  Value *TCntY, *TCntZ;

  std::tie(TCntY, TCntZ) = getLocalSizeYZ(Builder);
  Value *TIdX = getWorkitemID(Builder, 0);
  Value *TIdY = getWorkitemID(Builder, 1);
  Value *TIdZ = getWorkitemID(Builder, 2);

  Value *Tmp0 = Builder.CreateMul(TCntY, TCntZ, "", true, true);
  Tmp0 = Builder.CreateMul(Tmp0, TIdX);
  Value *Tmp1 = Builder.CreateMul(TIdY, TCntZ, "", true, true);
  Value *TID = Builder.CreateAdd(Tmp0, Tmp1);
  TID = Builder.CreateAdd(TID, TIdZ);

  Value *Indices[] = {
    Constant::getNullValue(Type::getInt32Ty(Mod->getContext())),
    TID
  };

  Value *Offset = Builder.CreateInBoundsGEP(GVTy, GV, Indices);
  I.mutateType(Offset->getType());
  I.replaceAllUsesWith(Offset);
  I.eraseFromParent();

  for (Value *V : WorkList) {
    CallInst *Call = dyn_cast<CallInst>(V);
    if (!Call) {
      if (ICmpInst *CI = dyn_cast<ICmpInst>(V)) {
        Value *Src0 = CI->getOperand(0);
        Type *EltTy = Src0->getType()->getPointerElementType();
        PointerType *NewTy = PointerType::get(EltTy, AMDGPUAS::LOCAL_ADDRESS);

        if (isa<ConstantPointerNull>(CI->getOperand(0)))
          CI->setOperand(0, ConstantPointerNull::get(NewTy));

        if (isa<ConstantPointerNull>(CI->getOperand(1)))
          CI->setOperand(1, ConstantPointerNull::get(NewTy));

        continue;
      }

      // The operand's value should be corrected on its own.
      if (isa<AddrSpaceCastInst>(V))
        continue;

      Type *EltTy = V->getType()->getPointerElementType();
      PointerType *NewTy = PointerType::get(EltTy, AMDGPUAS::LOCAL_ADDRESS);

      // FIXME: It doesn't really make sense to try to do this for all
      // instructions.
      V->mutateType(NewTy);

      // Adjust the types of any constant operands.
      if (SelectInst *SI = dyn_cast<SelectInst>(V)) {
        if (isa<ConstantPointerNull>(SI->getOperand(1)))
          SI->setOperand(1, ConstantPointerNull::get(NewTy));

        if (isa<ConstantPointerNull>(SI->getOperand(2)))
          SI->setOperand(2, ConstantPointerNull::get(NewTy));
      } else if (PHINode *Phi = dyn_cast<PHINode>(V)) {
        for (unsigned I = 0, E = Phi->getNumIncomingValues(); I != E; ++I) {
          if (isa<ConstantPointerNull>(Phi->getIncomingValue(I)))
            Phi->setIncomingValue(I, ConstantPointerNull::get(NewTy));
        }
      }

      continue;
    }

    IntrinsicInst *Intr = cast<IntrinsicInst>(Call);
    Builder.SetInsertPoint(Intr);
    switch (Intr->getIntrinsicID()) {
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
      // These intrinsics are for address space 0 only
      Intr->eraseFromParent();
      continue;
    case Intrinsic::memcpy: {
      MemCpyInst *MemCpy = cast<MemCpyInst>(Intr);
      Builder.CreateMemCpy(MemCpy->getRawDest(), MemCpy->getRawSource(),
                           MemCpy->getLength(), MemCpy->getAlignment(),
                           MemCpy->isVolatile());
      Intr->eraseFromParent();
      continue;
    }
    case Intrinsic::memmove: {
      MemMoveInst *MemMove = cast<MemMoveInst>(Intr);
      Builder.CreateMemMove(MemMove->getRawDest(), MemMove->getRawSource(),
                            MemMove->getLength(), MemMove->getAlignment(),
                            MemMove->isVolatile());
      Intr->eraseFromParent();
      continue;
    }
    case Intrinsic::memset: {
      MemSetInst *MemSet = cast<MemSetInst>(Intr);
      Builder.CreateMemSet(MemSet->getRawDest(), MemSet->getValue(),
                           MemSet->getLength(), MemSet->getAlignment(),
                           MemSet->isVolatile());
      Intr->eraseFromParent();
      continue;
    }
    case Intrinsic::invariant_start:
    case Intrinsic::invariant_end:
    case Intrinsic::invariant_group_barrier:
      Intr->eraseFromParent();
      // FIXME: I think the invariant marker should still theoretically apply,
      // but the intrinsics need to be changed to accept pointers with any
      // address space.
      continue;
    case Intrinsic::objectsize: {
      Value *Src = Intr->getOperand(0);
      Type *SrcTy = Src->getType()->getPointerElementType();
      Function *ObjectSize = Intrinsic::getDeclaration(Mod,
        Intrinsic::objectsize,
        { Intr->getType(), PointerType::get(SrcTy, AMDGPUAS::LOCAL_ADDRESS) }
      );

      CallInst *NewCall
        = Builder.CreateCall(ObjectSize, { Src, Intr->getOperand(1) });
      Intr->replaceAllUsesWith(NewCall);
      Intr->eraseFromParent();
      continue;
    }
    default:
      Intr->dump();
      llvm_unreachable("Don't know how to promote alloca intrinsic use.");
    }
  }
}

FunctionPass *llvm::createAMDGPUPromoteAlloca(const TargetMachine *TM) {
  return new AMDGPUPromoteAlloca(TM);
}