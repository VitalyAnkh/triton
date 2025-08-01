#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h"
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "triton/Conversion/TritonGPUToLLVM/PatternTritonGPUOpToLLVM.h"
#include "triton/Conversion/TritonGPUToLLVM/Utility.h"

namespace {

using namespace mlir;
using namespace mlir::triton;

// NOTE: [Additional Function Arguments]
// Triton patches additional arguments to the function signature to support
// (1) shared memory, (2) global scratch memory, and (3) profile scratch memory.
// To support use of shared memory and global scratch memory inside of a
// function, the caller allocates a single large block of the relevant memory
// and calls the function with these extra arguments at the end.
// Profile scratch memory is only used when the function is instrumented for
// profiling.
//
// For the kernel function itself, the shared memory base is a global symbol
// so no additional function argument is required but global scratch memory
// allocation is still passed in as the last argument. Though here the scratch
// memory is shared between all programs, so a linear offset based on the
// program id is required to get the local scratch base.

struct FuncOpConversion : public ConvertOpToLLVMPattern<triton::FuncOp> {
  FuncOpConversion(LLVMTypeConverter &converter,
                   const TargetInfoBase &targetInfo, PatternBenefit benefit)
      : ConvertOpToLLVMPattern(converter, benefit), targetInfo(targetInfo) {}

  /// Only retain those attributes that are not constructed by
  /// `LLVMFuncOp::build`. If `filterArgAttrs` is set, also filter out argument
  /// attributes.
  static void filterFuncAttributes(triton::FuncOp op, bool filterArgAttrs,
                                   SmallVectorImpl<NamedAttribute> &result) {

    for (const auto &attr : op->getAttrs()) {
      if (attr.getName() == SymbolTable::getSymbolAttrName() ||
          attr.getName() == op.getFunctionTypeAttrName() ||
          attr.getName() == "std.varargs" ||
          (filterArgAttrs && attr.getName() == op.getArgAttrsAttrName()))
        continue;
      result.push_back(attr);
    }
  }

  triton::FuncOp amendFuncOp(triton::FuncOp funcOp,
                             ConversionPatternRewriter &rewriter,
                             const TargetInfoBase &targetInfo) const {
    // Push back two new arguments that indicate the current pointer to shared
    // memory and global scratch memory.
    auto loc = funcOp.getLoc();
    auto ctx = funcOp->getContext();
    auto sharedPtrTy =
        LLVM::LLVMPointerType::get(ctx, targetInfo.getSharedAddressSpace());
    auto globalPtrTy = LLVM::LLVMPointerType::get(ctx, 1);
    auto profilePtrTy = LLVM::LLVMPointerType::get(ctx, 1);

    // 1. Modify the function type to add the new arguments.
    auto funcTy = funcOp.getFunctionType();
    auto amendedInputTy = llvm::to_vector<4>(funcTy.getInputs());
    bool isKernel = triton::isKernel(funcOp);
    if (isKernel) {
      for (auto i : llvm::seq(amendedInputTy.size())) {
        if (isa<TensorDescType>(amendedInputTy[i])) {
          funcOp.setArgAttr(i, "tt.nv_tma_desc",
                            mlir::IntegerAttr::get(i32_ty, 1));
        }
      }
    }
    if (!isKernel) {
      amendedInputTy.push_back(sharedPtrTy);
    }
    amendedInputTy.push_back(globalPtrTy);
    amendedInputTy.push_back(profilePtrTy);
    auto amendedFuncTy =
        FunctionType::get(ctx, amendedInputTy, funcTy.getResults());
    // 2. Modify the argument attributes to add the new argument.
    SmallVector<NamedAttribute> amendedAttrs;
    filterFuncAttributes(funcOp, /*filterArgAttrs=*/true, amendedAttrs);
    if (auto argAttrs = funcOp.getAllArgAttrs()) {
      llvm::SmallVector<mlir::Attribute> amendedArgAttrs(argAttrs.begin(),
                                                         argAttrs.end());
      while (amendedArgAttrs.size() < amendedInputTy.size()) {
        amendedArgAttrs.emplace_back(DictionaryAttr::get(ctx));
      }
      amendedAttrs.push_back(
          rewriter.getNamedAttr(funcOp.getArgAttrsAttrName(),
                                rewriter.getArrayAttr(amendedArgAttrs)));
    }

    // 3. Add the new arguments to the region
    auto amendedFuncOp = rewriter.create<triton::FuncOp>(
        funcOp.getLoc(), funcOp.getName(), amendedFuncTy, amendedAttrs);
    auto &region = funcOp.getBody();
    if (!isKernel) {
      region.addArgument(sharedPtrTy, loc);
    }
    region.addArgument(globalPtrTy, loc);
    region.addArgument(profilePtrTy, loc);
    rewriter.inlineRegionBefore(region, amendedFuncOp.getBody(),
                                amendedFuncOp.end());
    return amendedFuncOp;
  }

  // Map the MLIR attribute `tt.nv_tma_desc` to the appropriate LLVM and NVVM
  // attributes.
  static void handleByvalTmaDescArgs(LLVM::LLVMFuncOp &llvmFuncOp) {
    const bool isKernel = triton::isKernel(llvmFuncOp);
    for (unsigned i = 0; i < llvmFuncOp.getNumArguments(); ++i) {
      const auto attrs = llvmFuncOp.getArgAttrDict(i);
      if (!attrs) {
        continue;
      }

      for (const auto &attr : attrs) {
        if (attr.getName() == "tt.nv_tma_desc") {
          const auto i32_type =
              mlir::IntegerType::get(llvmFuncOp.getContext(), 32);
          assert(attr.getValue() == mlir::IntegerAttr::get(i32_type, 1));
          assert(isKernel &&
                 "tt.nv_tma_desc is not supported for device functions");

          // See
          // https://github.com/google/jax/blob/main/jaxlib/mosaic/gpu/passes.cc
          mlir::BlockArgument arg = llvmFuncOp.getArgument(i);
          const auto byteType =
              mlir::IntegerType::get(llvmFuncOp.getContext(), 8);
          const auto arrayType = mlir::LLVM::LLVMArrayType::get(
              llvmFuncOp.getContext(), byteType, 128);
          llvmFuncOp.setArgAttr(i, LLVM::LLVMDialect::getByValAttrName(),
                                mlir::TypeAttr::get(arrayType));
          llvmFuncOp.setArgAttr(i, NVVM::NVVMDialect::getGridConstantAttrName(),
                                mlir::UnitAttr::get(llvmFuncOp.getContext()));
          llvmFuncOp.setArgAttr(i, LLVM::LLVMDialect::getAlignAttrName(),
                                mlir::IntegerAttr::get(i32_type, 64));
        }
      }
    }
  }

  LogicalResult
  matchAndRewrite(triton::FuncOp funcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Prevent LLVM's inliner to inline this function
    auto amendedFuncOp = amendFuncOp(funcOp, rewriter, targetInfo);

    FailureOr<LLVM::LLVMFuncOp> maybeNewFuncOp =
        mlir::convertFuncOpToLLVMFuncOp(amendedFuncOp, rewriter,
                                        *getTypeConverter());
    if (failed(maybeNewFuncOp)) {
      return failure();
    }

    LLVM::LLVMFuncOp newFuncOp = *maybeNewFuncOp;

    auto ctx = funcOp->getContext();

    if (triton::isKernel(funcOp)) {
      // Set an attribute to indicate this function is a kernel entry.
      newFuncOp->setAttr(NVVM::NVVMDialect::getKernelFuncAttrName(),
                         rewriter.getIntegerAttr(type::u1Ty(ctx), 1));
      newFuncOp.setLinkage(LLVM::Linkage::External);
    } else {
      // The noinline attribute will be used by the LLVM codegen to prevent
      // inlining.
      // https://github.com/llvm/llvm-project/blob/main/mlir/lib/Dialect/LLVMIR/IR/LLVMInlining.cpp#L267
      newFuncOp.setPassthroughAttr(
          ArrayAttr::get(ctx, rewriter.getStringAttr("noinline")));
      newFuncOp.setLinkage(LLVM::Linkage::Internal);
    }

    // Determine the actual number of required warps.
    int numWarps = triton::gpu::lookupNumWarps(funcOp);
    if (auto totalNumWarps = funcOp.getParentOp()->getAttrOfType<IntegerAttr>(
            "ttg.total-num-warps"))
      numWarps = totalNumWarps.getInt();

    // Set `nvvm.maxnreg` if it was specified on the module.
    if (Attribute maxnregAttr =
            funcOp.getParentOp()->getAttr(triton::gpu::AttrMaxRegistersName))
      newFuncOp->setAttr(NVVM::NVVMDialect::getMaxnregAttrName(), maxnregAttr);

    // Set an attribute for reqntidx, it could be used in latter LLVM codegen
    // for `nvvm.annotation` metadata.
    newFuncOp->setAttr(NVVM::NVVMDialect::getReqntidAttrName(),
                       rewriter.getDenseI32ArrayAttr(32 * numWarps));

    rewriter.eraseOp(funcOp);
    rewriter.eraseOp(amendedFuncOp);

    // Add attributes for by-value TMA descriptor args (nvidia)
    handleByvalTmaDescArgs(newFuncOp);

    return success();
  }

private:
  const TargetInfoBase &targetInfo;
};

} // namespace

void mlir::triton::populateFuncOpConversionPattern(
    LLVMTypeConverter &typeConverter, RewritePatternSet &patterns,
    const TargetInfoBase &targetInfo, PatternBenefit benefit) {
  patterns.add<FuncOpConversion>(typeConverter, targetInfo, benefit);
}
