#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/Passes.h"
#include "triton/Dialect/Triton/IR/Utility.h"
#include "triton/Dialect/TritonNvidiaGPU/IR/Dialect.h"
#include "triton/Dialect/TritonNvidiaGPU/Transforms/Passes.h"

namespace ttg = mlir::triton::gpu;

namespace mlir {
namespace triton {
namespace nvidia_gpu {

#define GEN_PASS_DEF_TRITONNVIDIAGPUMMALOWERINGPASS
#include "triton/Dialect/TritonNvidiaGPU/Transforms/Passes.h.inc"

namespace {

template <typename TCGen5MMAOpTy>
class SyncMMALowering : public OpRewritePattern<TCGen5MMAOpTy> {
public:
  using OpRewritePattern<TCGen5MMAOpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(TCGen5MMAOpTy op,
                                PatternRewriter &rewriter) const override {
    // If the op doesn't have synchronous semantic skip the pattern.
    if (op.getIsAsync())
      return failure();
    MLIRContext *ctx = op.getContext();
    Location loc = op.getLoc();
    Attribute sharedMemorySpace = ttg::SharedMemorySpaceAttr::get(ctx);
    auto barrierCTALayout = ttg::CTALayoutAttr::get(
        /*context=*/ctx, /*CTAsPerCGA=*/{1},
        /*CTASplitNum=*/{1}, /*CTAOrder=*/{0});
    auto barrierEncoding = ttg::SwizzledSharedEncodingAttr::get(
        ctx, 1, 1, 1, {0}, barrierCTALayout);
    ttg::MemDescType barrierMemDescType =
        ttg::MemDescType::get({1}, rewriter.getI64Type(), barrierEncoding,
                              sharedMemorySpace, /*mutableMemory=*/true);
    Value barrierAlloc =
        rewriter.create<ttg::LocalAllocOp>(loc, barrierMemDescType, Value());
    rewriter.create<InitBarrierOp>(loc, barrierAlloc, 1);
    op.addCompletionBarrier(barrierAlloc,
                            rewriter.create<arith::ConstantIntOp>(loc, 1, 1));
    op.setIsAsync(true);

    rewriter.setInsertionPointAfter(op);
    Value phase = rewriter.create<arith::ConstantIntOp>(loc, 0, 32);
    rewriter.create<WaitBarrierOp>(loc, barrierAlloc, phase, op.getPred());
    rewriter.create<InvalBarrierOp>(loc, barrierAlloc);
    return success();
  }
};

struct TCGen5MMAScaleSharedToTmemConversion
    : public OpRewritePattern<TCGen5MMAScaledOp> {
  using OpRewritePattern<TCGen5MMAScaledOp>::OpRewritePattern;

  // Create a tmem_copy of scales from shared memory to tmem. `rows` is the M or
  // N of the MMA operation (for LHS or RHS respectively).
  bool lowerScaleToTmem(OpOperand &operand, PatternRewriter &rewriter,
                        int rows) const {
    Location loc = operand.getOwner()->getLoc();
    MLIRContext *context = operand.getOwner()->getContext();
    Attribute tensorMemorySpace = TensorMemorySpaceAttr::get(context);
    auto oldType = cast<ttg::MemDescType>(operand.get().getType());
    auto numElems = product(oldType.getShape());
    Type elType = oldType.getElementType();
    ttg::CTALayoutAttr CTALayout = ttg::getCTALayout(oldType.getEncoding());
    ArrayRef<unsigned> CTASplitNum = CTALayout.getCTASplitNum();
    // Distribute the scales across the rows of the MMA operation.
    SmallVector<int64_t> shape = {rows, numElems / rows};
    Attribute scaleEncoding = TensorMemoryScalesEncodingAttr::get(
        context, CTASplitNum[0], CTASplitNum[1]);
    Type scaleAType =
        ttg::MemDescType::get(shape, elType, scaleEncoding, tensorMemorySpace,
                              /*mutableMemory=*/true);
    auto tmemAlloc = rewriter.create<TMEMAllocOp>(loc, scaleAType, Value());
    rewriter.create<TMEMCopyOp>(loc, operand.get(), tmemAlloc,
                                /*barrier*/ Value());
    operand.set(tmemAlloc);
    return true;
  }

  LogicalResult matchAndRewrite(TCGen5MMAScaledOp op,
                                PatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    MLIRContext *context = op->getContext();
    auto aScaleType = op.getAScale().getType();
    auto bScaleType = op.getBScale().getType();
    int blockM = op.getBlockM();
    int blockN = op.getBlockN();
    bool anyChanged = false;
    if (isa<ttg::SharedMemorySpaceAttr>(aScaleType.getMemorySpace())) {
      anyChanged = lowerScaleToTmem(op.getAScaleMutable(), rewriter, blockM);
    }
    if (isa<ttg::SharedMemorySpaceAttr>(bScaleType.getMemorySpace())) {
      anyChanged = lowerScaleToTmem(op.getBScaleMutable(), rewriter, blockN);
    }
    return LogicalResult::success(anyChanged);
  }
};

} // anonymous namespace

class TritonNvidiaGPUMMALoweringPass
    : public impl::TritonNvidiaGPUMMALoweringPassBase<
          TritonNvidiaGPUMMALoweringPass> {
public:
  void runOnOperation() override {
    MLIRContext *context = &getContext();
    ModuleOp m = getOperation();

    mlir::RewritePatternSet patterns(context);
    patterns
        .add<SyncMMALowering<TCGen5MMAOp>, SyncMMALowering<TCGen5MMAScaledOp>,
             TCGen5MMAScaleSharedToTmemConversion>(context);
    if (applyPatternsGreedily(m, std::move(patterns)).failed())
      signalPassFailure();
  }
};

} // namespace nvidia_gpu
} // namespace triton
} // namespace mlir
