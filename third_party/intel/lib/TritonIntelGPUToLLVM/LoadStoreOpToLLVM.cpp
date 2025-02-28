#include "Dialect/TritonIntelGPU/IR/Dialect.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/TypeUtilities.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TypeSwitch.h"

#include "PatternTritonGPUOpToLLVM.h"
#include "TargetInfo.h"
#include "Utility.h"

#include "intel/include/Dialect/TritonIntelGPU/IR/Attributes.h"
#include "intel/include/Dialect/TritonIntelGPU/Transforms/Utility.h"
#include "triton/Dialect/TritonGPU/IR/Attributes.h"

using namespace mlir;
using namespace mlir::triton;
using namespace mlir::triton::gpu;
using namespace mlir::triton::gpu::intel;

using ::mlir::LLVM::delinearize;
using ::mlir::triton::gpu::getTotalElemsPerThread;

namespace {

// Return the mask for the unique data accessed by given tensor type.
// Used to mask out the redundant data accessed by threads.
Value redundantDataMask(Type valueTy, ConversionPatternRewriter &rewriter,
                        Location loc,
                        const triton::intel::TargetInfo &targetInfo) {
  auto tensorTy = dyn_cast<RankedTensorType>(valueTy);
  Value mask = int_val(1, 1);
  auto tid = tid_val();
  auto clusterCTAId = targetInfo.getClusterCTAId(rewriter, loc);
  if (tensorTy) {
    auto layout = tensorTy.getEncoding();
    auto shape = tensorTy.getShape();
    unsigned rank = shape.size();
    auto sizePerThread = triton::gpu::getSizePerThread(layout);
    auto threadsPerWarp = triton::gpu::getThreadsPerWarp(layout);
    auto warpsPerCTA = triton::gpu::getWarpsPerCTA(layout);
    auto order = triton::gpu::getOrder(layout);
    auto shapePerCTATile = triton::gpu::getShapePerCTATile(layout, shape);
    Value warpSize = LLVM::intel::getModuleWarpSize(rewriter, loc);
    Value laneId = urem(tid, warpSize);
    Value warpId = udiv(tid, warpSize);
    SmallVector<Value> multiDimWarpId =
        delinearize(rewriter, loc, warpId, warpsPerCTA, order);
    SmallVector<Value> multiDimThreadId =
        delinearize(rewriter, loc, laneId, threadsPerWarp, order);
    for (unsigned dim = 0; dim < rank; ++dim) {
      // if there is no data replication across threads on this dimension
      if (shape[dim] >= shapePerCTATile[dim])
        continue;
      // Otherwise, we need to mask threads that will replicate data on this
      // dimension. Calculate the thread index on this dimension for the CTA
      Value threadDim =
          add(mul(multiDimWarpId[dim], i32_val(threadsPerWarp[dim])),
              multiDimThreadId[dim]);
      mask = and_(mask, icmp_slt(mul(threadDim, i32_val(sizePerThread[dim])),
                                 i32_val(shape[dim])));
    }
    // Do not write duplicated data when multicast is enabled
    if (triton::gpu::getNumCTAs(layout) > 1) {
      auto _0 = i32_val(0);
      auto CTAsPerCGA = triton::gpu::getCTAsPerCGA(layout);
      auto CTASplitNum = triton::gpu::getCTASplitNum(layout);
      auto CTAOrder = triton::gpu::getCTAOrder(layout);

      auto multiDimClusterCTAId =
          delinearize(rewriter, loc, clusterCTAId, CTAsPerCGA, CTAOrder);

      for (unsigned dim = 0; dim < rank; ++dim) {
        // Skip when multicast is not enabled in this dimension
        if (CTAsPerCGA[dim] == CTASplitNum[dim])
          continue;
        // This wrapping rule must be consistent with emitCTAOffsetForLayout
        unsigned splitNum = std::min<unsigned>(shape[dim], CTASplitNum[dim]);
        Value repId = udiv(multiDimClusterCTAId[dim], i32_val(splitNum));
        // Consider the example where CTAsPerCGA = [4] and CTASplitNum = [2]:
        //     CTA0 and CTA2 holds data of block0,
        //     CTA1 and CTA3 holds data of block1.
        // Only CTA0 and CTA1 are expected to write while CTA2 and CTA3 should
        // be masked. We add the following mask:
        //     multiDimClusterCTAId[dim] / splitNum == 0
        // Actually in all existing cases of multicast, splitNum is always 1.
        // The mask is equivalent to:
        //     multiDimClusterCTAId[dim] == 0
        mask = and_(mask, icmp_eq(repId, _0));
      }
    }
  } else {
    // If the tensor is not ranked, then it is a scalar and only thread 0 of
    // CTA0 can write
    mask = and_(mask, icmp_eq(clusterCTAId, i32_val(0)));
    mask = and_(mask, icmp_eq(tid, i32_val(0)));
  }
  return mask;
}

/// Holds the values related to a block pointer.
/// It includes the base pointer, base width and height, row and column
/// stride, and offset base for X and Y.
struct BlockPointerValues {
  Value base;
  Value baseWidth;
  Value baseHeight;
  Value rowStride;
  Value colStride;
  Value offsetBaseX;
  Value offsetBaseY;
};

// Unpack values as the params to 2DBlockLoad Payload: offsetBaseY,
// offsetBaseX, baseHeight, baseWidth, rowStride, colStride, base.
// FIXME: Only supports 2D matrices for now.
BlockPointerValues
getValuesFromBlockPointerStruct(Value blockPointerStruct,
                                ConversionPatternRewriter &rewriter) {
  const SmallVector<Value> &elems = unpackLLElements(
      blockPointerStruct.getLoc(), blockPointerStruct, rewriter);
  assert(elems.size() == 7 &&
         "unexpected number of values unpacked from a block pointer");
  BlockPointerValues values{
      .base = elems[6],
      .baseWidth = elems[3],
      .baseHeight = elems[2],
      .rowStride = elems[4],
      .colStride = elems[5],
      .offsetBaseX = elems[1],
      .offsetBaseY = elems[0],
  };
  return values;
}

/// Compute the 2D prefetch shape for each warp given an input 2D tensor.
/// Because a cache line is 64 bytes, and we want to prefetch one cache line a
/// time (per thread), the maximum number of bytes per column is 64. We know
/// that the maximum size for each 2D prefetch is 2048 bytes, therefore the
/// maximum number of rows is given by 2048/64=32.
SmallVector<unsigned, 2> get2DPrefetchShapePerWarp(RankedTensorType tensorTy) {
  Type eltTy = tensorTy.getElementType();
  const ArrayRef<int64_t> tensorShape = tensorTy.getShape();
  unsigned elemSizeInBits = eltTy.getIntOrFloatBitWidth();
  unsigned elemSizeInBytes = elemSizeInBits / 8;
  unsigned maxBytesPerCol = 64;
  unsigned numRows = std::min<unsigned>(tensorShape[0], 32);
  unsigned numCols = maxBytesPerCol / elemSizeInBytes;
  return {numRows, numCols};
}

/// Get the 2D warps per CTA given the tensor shape and the prefetch
/// shape per warp.
SmallVector<unsigned, 2>
getWarpsPerCTA(const ArrayRef<int64_t> tensorShape,
               const SmallVector<unsigned, 2> &shapePerWarp,
               unsigned numWarps) {
  assert(tensorShape.size() == 2 && shapePerWarp.size() == 2 &&
         "only 2D tensors are supported");

  const unsigned rowColRatio = ceil<unsigned>(shapePerWarp[0], shapePerWarp[1]);
  const unsigned colRowRatio = ceil<unsigned>(shapePerWarp[1], shapePerWarp[0]);

  SmallVector<unsigned, 2> warpsPerCTA = {1, 1};
  do {
    if (warpsPerCTA[0] * warpsPerCTA[1] >= numWarps)
      break;
    if (tensorShape[0] / (shapePerWarp[0] * colRowRatio) / warpsPerCTA[0] >=
        tensorShape[1] / (shapePerWarp[1] * rowColRatio) / warpsPerCTA[1]) {
      if (warpsPerCTA[0] < tensorShape[0] / shapePerWarp[0])
        warpsPerCTA[0] *= 2;
      else
        warpsPerCTA[1] *= 2;
    } else
      warpsPerCTA[1] *= 2;
  } while (true);

  return warpsPerCTA;
}

// Contains some helper functions for both Load and Store conversions.
struct LoadStoreConversionBase {
  explicit LoadStoreConversionBase(const triton::intel::TargetInfo &targetInfo,
                                   ModuleAxisInfoAnalysis &axisAnalysisPass)
      : targetInfo(targetInfo), axisAnalysisPass(axisAnalysisPass) {}

  unsigned getContiguity(Value ptr) const {
    auto tensorTy = dyn_cast<RankedTensorType>(ptr.getType());
    if (!tensorTy)
      return 1;
    return axisAnalysisPass.getPtrContiguity(ptr);
  }

  unsigned getVectorSize(Value ptr) const {
    auto tensorTy = dyn_cast<RankedTensorType>(ptr.getType());
    if (!tensorTy)
      return 1;
    auto contiguity = getContiguity(ptr);
    auto pointeeBitWidth = triton::getPointeeBitWidth(tensorTy);
    // The maximum vector size is 128 bits.
    return std::min<unsigned>(128 / pointeeBitWidth, contiguity);
  }

  unsigned getMaskAlignment(Value mask) const {
    return axisAnalysisPass.getMaskAlignment(mask);
  }

protected:
  ModuleAxisInfoAnalysis &axisAnalysisPass;
  const triton::intel::TargetInfo &targetInfo;
};

struct PrefetchOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::gpu::intel::PrefetchOp>,
      public LoadStoreConversionBase {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::gpu::intel::PrefetchOp>::ConvertTritonGPUOpToLLVMPattern;

  PrefetchOpConversion(TritonGPUToLLVMTypeConverter &converter,
                       const triton::intel::TargetInfo &targetInfo,
                       ModuleAxisInfoAnalysis &axisAnalysisPass,
                       PatternBenefit benefit)
      : ConvertTritonGPUOpToLLVMPattern<triton::gpu::intel::PrefetchOp>(
            converter, benefit),
        LoadStoreConversionBase(targetInfo, axisAnalysisPass) {}

  LogicalResult
  matchAndRewrite(triton::gpu::intel::PrefetchOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    Value ptr = op.getPtr();
    if (isTensorPointerType(ptr.getType()))
      return rewriteTensorPointerPrefetch(op, adaptor, rewriter);

    llvm_unreachable("Unexpected prefetch operation on 'regular' ptr");
    return failure();
  }

  LogicalResult
  rewriteTensorPointerPrefetch(triton::gpu::intel::PrefetchOp op,
                               OpAdaptor adaptor,
                               ConversionPatternRewriter &rewriter) const {
    auto mod = rewriter.getBlock()->getParent()->getParentOfType<ModuleOp>();
    Location loc = op.getLoc();
    Value ptr = op.getPtr();
    auto ptrType = cast<PointerType>(ptr.getType());
    auto tensorType = cast<RankedTensorType>(ptrType.getPointeeType());
    Type eltTy = tensorType.getElementType();
    const ArrayRef<int64_t> tensorShape = tensorType.getShape();

    unsigned numWarps = triton::gpu::TritonGPUDialect::getNumWarps(mod);

    SmallVector<unsigned, 2> shapePerWarp =
        get2DPrefetchShapePerWarp(tensorType);
    SmallVector<unsigned, 2> warpsPerCTA =
        getWarpsPerCTA(tensorShape, shapePerWarp, numWarps);

    SmallVector<int64_t> numReps = {
        mlir::ceil<int64_t>(tensorShape[0], shapePerWarp[0] * warpsPerCTA[0]),
        mlir::ceil<int64_t>(tensorShape[1], shapePerWarp[1] * warpsPerCTA[1])};

    unsigned bytesPerCol = shapePerWarp[1] * eltTy.getIntOrFloatBitWidth() / 8;
    unsigned elemSizeInBits = bytesPerCol >= 4 ? 32 : bytesPerCol * 8;
    unsigned tileWidthInElem =
        mlir::ceil<unsigned>(bytesPerCol * 8, elemSizeInBits);
    unsigned tileHeightInElem = shapePerWarp[0];

    Value warpSize = LLVM::intel::getModuleWarpSize(rewriter, loc);
    Value warpId = udiv(getThreadId(rewriter, loc), warpSize);
    Value laneId = urem(getThreadId(rewriter, loc), warpSize);
    SmallVector<Value> multiDimWarpId =
        mlir::LLVM::delinearize(rewriter, loc, warpId, warpsPerCTA, {1, 0});

    auto [base, baseWidth, baseHeight, rowStride, colStride, offsetBaseX,
          offsetBaseY] =
        getValuesFromBlockPointerStruct(adaptor.getPtr(), rewriter);

    base = gep(base.getType(), eltTy, base, offsetBaseX);
    offsetBaseY = trunc(i32_ty, offsetBaseY);
    rowStride = trunc(i32_ty, rowStride);
    Value rowOffset = mul(offsetBaseY, rowStride);
    base = gep(base.getType(), eltTy, base, rowOffset);

    baseWidth = trunc(i32_ty, baseWidth);
    baseWidth = mul(baseWidth, i32_val(eltTy.getIntOrFloatBitWidth() / 8));
    baseHeight = trunc(i32_ty, baseHeight);
    rowStride = trunc(i32_ty, rowStride);
    rowStride = mul(rowStride, i32_val(eltTy.getIntOrFloatBitWidth() / 8));

    multiDimWarpId[1] = trunc(i32_ty, multiDimWarpId[1]);
    multiDimWarpId[0] = trunc(i32_ty, multiDimWarpId[0]);

    for (int row = 0; row < numReps[0]; ++row) {
      for (int col = 0; col < numReps[1]; ++col) {
        Value offsetX, offsetY;
        offsetX = add(
            // the offset of this warp.
            mul(multiDimWarpId[1], i32_val(shapePerWarp[1])),
            // add the replica offset with a warp stride.
            i32_val(col * warpsPerCTA[1] * shapePerWarp[1]));
        // Round the offset into to the tensor shape
        offsetX = urem(offsetX, i32_val(tensorShape[0]));
        offsetY = add(
            // the offset of this warp.
            mul(multiDimWarpId[0], i32_val(shapePerWarp[0])),
            // add the replica offset with a warp stride.
            i32_val(row * warpsPerCTA[0] * shapePerWarp[0]));
        // Round the offset into to the tensor shape
        offsetY = urem(offsetY, i32_val(tensorShape[0]));
        rewriter.create<TritonGEN::Matrix2DBlockPrefetchOp>(
            loc,
            /*ptr*/ base,
            /*base_width*/ baseWidth,
            /*base_height*/ baseHeight,
            /*base_pitch*/ rowStride,
            /*x*/ trunc(i32_ty, offsetX),
            /*y*/ trunc(i32_ty, offsetY),
            /*elem_size_in_bits*/ elemSizeInBits,
            /*tile_width*/ tileWidthInElem,
            /*tile_height*/ tileHeightInElem,
            /*v_blocks*/ 1,
            /*transpose*/ false,
            /*vnni_transform*/ false,
            /*cache_opt*/ TritonGEN::LoadCacheControl::L1C_L3C);
      }
    }

    rewriter.eraseOp(op);
    return success();
  }
};

struct LoadOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::LoadOp>,
      public LoadStoreConversionBase {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::LoadOp>::ConvertTritonGPUOpToLLVMPattern;

  LoadOpConversion(TritonIntelGPUToLLVMTypeConverter &converter,
                   const triton::intel::TargetInfo &targetInfo,
                   ModuleAxisInfoAnalysis &axisAnalysisPass,
                   PatternBenefit benefit)
      : ConvertTritonGPUOpToLLVMPattern<triton::LoadOp>(converter, benefit),
        LoadStoreConversionBase(targetInfo, axisAnalysisPass) {}

  LogicalResult
  rewriteTensorPointerLoad(triton::LoadOp op, OpAdaptor adaptor,
                           ConversionPatternRewriter &rewriter) const {
    Location loc = op.getLoc();
    Value ptr = op.getPtr();
    Value mask = op.getMask();
    Value other = op.getOther();
    Type resultType = op.getType();
    auto tensorType = cast<RankedTensorType>(resultType);

    // Only lower loadOp with dpas layout encoding.
    if (!hasDotDpasEncoding(tensorType))
      return failure();

    DotOperandEncodingAttr dotLayout = getDotEncoding(tensorType).value();
    auto dpasLayout = cast<DpasEncodingAttr>(dotLayout.getParent());

    const unsigned opIdx = dotLayout.getOpIdx();
    Type eltTy = tensorType.getElementType();
    const ArrayRef<int64_t> tensorShape = tensorType.getShape();
    unsigned numElems = getTotalElemsPerThread(resultType);
    SmallVector<int64_t> numReps =
        dpasLayout.getDPASRepetitions(tensorShape, opIdx);
    const SmallVector<unsigned> warpsPerCTA = dpasLayout.getWarpsPerCTA();
    SmallVector<unsigned> order = triton::gpu::getOrder(dpasLayout);
    int threadsPerWarp = triton::gpu::getWarpSize(dpasLayout);

    Value warpSize = i32_val(threadsPerWarp);
    Value warpId = udiv(getThreadId(rewriter, loc), warpSize);
    Value laneId = urem(getThreadId(rewriter, loc), warpSize);
    SmallVector<Value> multiDimWarpId =
        delinearize(rewriter, loc, warpId, warpsPerCTA, order);

    bool isOperandA = (opIdx == 0);
    SmallVector<unsigned> operandShape =
        isOperandA ? dpasLayout.getShapeA() : dpasLayout.getShapeB();
    SmallVector<int64_t> elemsPerInstr = {operandShape[0], operandShape[1]};
    int64_t elemsPerLane = product<int64_t>(elemsPerInstr) /
                           product<unsigned>(getThreadsPerWarp(dpasLayout));
    TritonGPUToLLVMTypeConverter *typeConverter = getTypeConverter();
    Type unpackType = LLVM::getFixedVectorType(
        typeConverter->convertType(eltTy), elemsPerLane);

    // pack scalars for operand A and B.
    Type elemType = (isOperandA && eltTy != f32_ty) ? i16_ty : i32_ty;
    unsigned opsPerChannel = dpasLayout.getOpsPerChannel();
    elemsPerLane = isOperandA ? elemsPerLane / (opsPerChannel == 4 ? 2 : 1)
                              : elemsPerLane / opsPerChannel;
    Type load2DGenXType = LLVM::getFixedVectorType(elemType, elemsPerLane);

    // Outer dim for A is the M, for B is the N. Inner dim for both is the K.
    int outerDimWarpNum = std::min<int>(
        warpsPerCTA[opIdx], ceil(tensorShape[opIdx], elemsPerInstr[opIdx]));
    Value outerDimWarpId =
        urem(multiDimWarpId[opIdx], i32_val(outerDimWarpNum));

    auto [base, baseWidth, baseHeight, rowStride, colStride, offsetBaseX,
          offsetBaseY] =
        getValuesFromBlockPointerStruct(adaptor.getPtr(), rewriter);

    // Load the operand.
    int64_t numRepOuter = numReps[opIdx];
    int64_t numRepK = numReps[!opIdx];

    SmallVector<Value> rets;
    for (int outer = 0; outer < numRepOuter; ++outer) {
      for (int k = 0; k < numRepK; ++k) {
        Value offsetX =
            isOperandA
                ? i32_val(k * elemsPerInstr[1])
                : add(mul(outerDimWarpId, i32_val(elemsPerInstr[opIdx])),
                      i32_val(outer * outerDimWarpNum * elemsPerInstr[opIdx]));
        Value offsetY =
            isOperandA
                ? add(mul(outerDimWarpId, i32_val(elemsPerInstr[opIdx])),
                      i32_val(outer * outerDimWarpNum * elemsPerInstr[opIdx]))
                : i32_val(k * elemsPerInstr[0]);

        offsetX = add(offsetX, offsetBaseX);
        offsetY = add(offsetY, offsetBaseY);
        baseWidth = trunc(i32_ty, baseWidth);
        baseHeight = trunc(i32_ty, baseHeight);
        rowStride = trunc(i32_ty, rowStride);

        unsigned elemSizeInBits = eltTy.getIntOrFloatBitWidth();
        Value elemSizeInBytes = i32_val(elemSizeInBits / 8);

        auto load2dOp = rewriter.create<TritonGEN::Matrix2DBlockLoadOp>(
            loc, load2DGenXType,
            /*ptr*/ base,
            /*base_width*/ mul(baseWidth, elemSizeInBytes),
            /*base_height*/ baseHeight,
            /*base_pitch*/ mul(rowStride, elemSizeInBytes),
            /*x*/ trunc(i32_ty, offsetX),
            /*y*/ trunc(i32_ty, offsetY),
            /*elem_size_in_bits*/ elemSizeInBits,
            /*tile_width*/ elemsPerInstr[1],
            /*tile_height*/ elemsPerInstr[0],
            /*v_blocks*/ 1,
            /*transpose*/ false,
            /*vnni_transform*/
            (!isOperandA && eltTy.getIntOrFloatBitWidth() != 32));

        rets.push_back(bitcast(load2dOp, unpackType));
      }
    }

    SmallVector<Value> loadedVals;
    for (Value &ret : rets) {
      auto loadTy = cast<VectorType>(unpackType);
      for (size_t i = 0; i < loadTy.getNumElements(); ++i) {
        Value loaded = extract_element(ret, i32_val(i));
        loadedVals.push_back(loaded);
      }
    }

    Type llvmResultStructTy = typeConverter->convertType(op.getType());
    Value resultStruct = packLLElements(loc, typeConverter, loadedVals,
                                        rewriter, llvmResultStructTy);
    rewriter.replaceOp(op, {resultStruct});

    return success();
  }

  LogicalResult
  matchAndRewrite(triton::LoadOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op->getLoc();
    auto typeConverter = getTypeConverter();
    auto *ctx = rewriter.getContext();

    // original values
    Value ptr = op.getPtr();
    Value mask = op.getMask();
    Value other = op.getOther();

    // adaptor values
    if (isTensorPointerType(ptr.getType()))
      return rewriteTensorPointerLoad(op, adaptor, rewriter);

    assert(!isTensorPointerType(ptr.getType()) &&
           "Cannot convert load with a tensor pointer into LLVM; "
           "this case should be transformed to normal load before lowering");
    Value llPtr = adaptor.getPtr();
    Value llMask = adaptor.getMask();
    Value llOther = adaptor.getOther();

    // Determine the vectorization size
    Type valueElemTy =
        typeConverter->convertType(getElementTypeOrSelf(op.getType()));
    unsigned vec = getVectorSize(ptr);
    unsigned numElems = getTotalElemsPerThread(ptr.getType());
    if (llMask)
      vec = std::min<size_t>(vec, getMaskAlignment(mask));

    // Get the LLVM values for pointers
    auto ptrElems = unpackLLElements(loc, llPtr, rewriter);
    assert(ptrElems.size() == numElems);

    // Get the LLVM values for mask
    SmallVector<Value> maskElems;
    if (llMask) {
      maskElems = unpackLLElements(loc, llMask, rewriter);
      assert(maskElems.size() == numElems);
    }

    // Get the LLVM values for `other`
    // TODO: (goostavz) handle when other is const but not splat, which
    //       should be rarely seen
    bool otherIsSplatConstInt = false;
    DenseElementsAttr constAttr;
    int64_t splatVal = 0;
    if (other && isa<IntegerType>(valueElemTy) &&
        matchPattern(other, m_Constant(&constAttr)) && constAttr.isSplat() &&
        isa<IntegerType>(constAttr.getElementType())) {
      otherIsSplatConstInt = true;
      splatVal = constAttr.getSplatValue<APInt>().getSExtValue();
    }
    SmallVector<Value> otherElems;
    if (other) {
      otherElems = unpackLLElements(loc, llOther, rewriter);
    }

    // vectorized iteration through all the pointer/mask/other elements
    const int valueElemNBits =
        std::max(8u, valueElemTy.getIntOrFloatBitWidth());
    const int numVecs = numElems / vec;

    SmallVector<Value> loadedVals;
    for (size_t vecStart = 0; vecStart < numElems; vecStart += vec) {
      // TODO: optimization when ptr is GEP with constant offset
      size_t in_off = 0;

      const size_t maxWordWidth = std::max<size_t>(32, valueElemNBits);
      const size_t totalWidth = valueElemNBits * vec;
      const size_t width = std::min(totalWidth, maxWordWidth);
      const size_t nWords = std::max<size_t>(1, totalWidth / width);
      const size_t wordNElems = width / valueElemNBits;
      const size_t movWidth = width < 16 ? 16 : width;
      assert(wordNElems * nWords * numVecs == numElems);

      Value pred = mask ? maskElems[vecStart] : int_val(1, 1);

      SmallVector<Type> retTys(nWords, IntegerType::get(getContext(), width));
      Type retTy = retTys.size() > 1
                       ? vec_ty(IntegerType::get(ctx, width), nWords)
                       : retTys[0];

      Value other_ = undef(retTy);
      if (other) {
        for (size_t ii = 0; ii < nWords; ++ii) {
          size_t size = width / valueElemNBits;

          auto vecTy = vec_ty(valueElemTy, size);
          Value v = undef(vecTy);
          for (size_t s = 0; s < size; ++s) {
            Value falseVal = otherElems[vecStart + ii * size + s];
            Value sVal = createIndexAttrConstant(
                rewriter, loc, this->getTypeConverter()->getIndexType(), s);
            v = insert_element(vecTy, v, falseVal, sVal);
          }
          v = bitcast(v, IntegerType::get(ctx, width));

          if (otherIsSplatConstInt) {
            for (size_t s = 0; s < 32; s += valueElemNBits)
              splatVal |= splatVal << valueElemNBits;
            v = int_val(width, splatVal);
          }

          Value iiVal = createIndexAttrConstant(
              rewriter, loc, this->getTypeConverter()->getIndexType(), ii);
          if (nWords > 1) {
            other_ = insert_element(retTy, other_, v, iiVal);
          } else {
            other_ = v;
          }
        }
      } else {
        other_ = rewriter.create<LLVM::ConstantOp>(loc, retTy,
                                                   rewriter.getZeroAttr(retTy));
      }

      // Create a predicated load operation.
      Block &endBlock = LLVM::intel::createPredicatedBlock(
          rewriter, loc, pred, SmallVector<Value, 1>{other_}, [&]() {
            Value addrElem =
                bitcast(ptrElems[vecStart], ptr_ty(ctx, 1 /*global*/));
            uint32_t alignment = nWords * width / 8;
            Value ret = load(retTy, addrElem, alignment);
            return SmallVector<Value, 1>{ret};
          });
      Value ret = *endBlock.args_begin();

      // Extract and store return values
      SmallVector<Value> rets;
      for (unsigned int ii = 0; ii < nWords; ++ii) {
        Value curr;
        if (isa<VectorType>(retTy)) {
          curr =
              extract_element(IntegerType::get(ctx, width), ret, i32_val(ii));
        } else {
          curr = ret;
        }
        curr = bitcast(curr, LLVM::getFixedVectorType(valueElemTy,
                                                      width / valueElemNBits));
        rets.push_back(curr);
      }
      int tmp = width / valueElemNBits;
      for (size_t ii = 0; ii < vec; ++ii) {
        Value loaded =
            extract_element(valueElemTy, rets[ii / tmp], i32_val(ii % tmp));
        loadedVals.push_back(loaded);
      }
    } // end vec

    Type llvmResultStructTy = typeConverter->convertType(op.getType());
    Value resultStruct = packLLElements(loc, typeConverter, loadedVals,
                                        rewriter, llvmResultStructTy);
    rewriter.replaceOp(op, {resultStruct});
    return success();
  }
};

struct StoreOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::StoreOp>,
      public LoadStoreConversionBase {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::StoreOp>::ConvertTritonGPUOpToLLVMPattern;

  StoreOpConversion(TritonIntelGPUToLLVMTypeConverter &converter,
                    const triton::intel::TargetInfo &targetInfo,
                    ModuleAxisInfoAnalysis &axisAnalysisPass,
                    PatternBenefit benefit)
      : ConvertTritonGPUOpToLLVMPattern<triton::StoreOp>(converter, benefit),
        LoadStoreConversionBase(targetInfo, axisAnalysisPass) {}

  LogicalResult
  rewriteTensorPointerStore(triton::StoreOp op, OpAdaptor adaptor,
                            ConversionPatternRewriter &rewriter) const {
    Location loc = op.getLoc();
    Type resultType = op.getValue().getType();
    auto tensorType = cast<RankedTensorType>(resultType);

    // Only lower StoreOp with dpas layout encoding.
    if (!hasDpasEncoding(tensorType))
      return failure();

    auto dpasLayout = cast<DpasEncodingAttr>(tensorType.getEncoding());
    TritonGPUToLLVMTypeConverter *typeConverter = getTypeConverter();
    MLIRContext *ctx = rewriter.getContext();

    Type eltTy = tensorType.getElementType();
    unsigned elemSizeInBits = eltTy.getIntOrFloatBitWidth();
    Value elemSizeInBytes = i32_val(elemSizeInBits / 8);
    const ArrayRef<int64_t> tensorShape = tensorType.getShape();
    unsigned numElems = getTotalElemsPerThread(tensorType);
    auto elemsPerInstr = dpasLayout.getShapeC();
    const SmallVector<unsigned> warpsPerCTA = dpasLayout.getWarpsPerCTA();
    SmallVector<int64_t> numReps =
        dpasLayout.getDPASRepetitions(tensorShape, 2);
    SmallVector<unsigned> order = triton::gpu::getOrder(dpasLayout);
    int threadsPerWarp = triton::gpu::getWarpSize(dpasLayout);

    Value warpSize = i32_val(threadsPerWarp);
    Value warpId = udiv(getThreadId(rewriter, loc), warpSize);
    Value laneId = urem(getThreadId(rewriter, loc), warpSize);
    SmallVector<Value> multiDimWarpId =
        mlir::LLVM::delinearize(rewriter, loc, warpId, warpsPerCTA, order);

    int64_t elemsPerLane = product<unsigned>(elemsPerInstr) / threadsPerWarp;
    Type store2DGenXType =
        LLVM::getFixedVectorType(IntegerType::get(ctx, elemSizeInBits),
                                 elemsPerLane); // make it opaque type.

    Value blockPtr = adaptor.getPtr();
    auto [base, width, height, rowStride, colStride, offsetBaseX, offsetBaseY] =
        getValuesFromBlockPointerStruct(blockPtr, rewriter);

    auto vals = unpackLLElements(loc, adaptor.getValue(), rewriter);
    assert(vals.size() == numElems);

    width = trunc(i32_ty, width);
    height = trunc(i32_ty, height);
    rowStride = trunc(i32_ty, rowStride);
    // encoded as bytes.
    Value baseWidth = mul(width, elemSizeInBytes);
    // encoded as bytes.
    Value basePitch = mul(rowStride, elemSizeInBytes);

    // A dense stride for the replicates.
    std::array<unsigned, 2> replicaStride = {
        static_cast<unsigned>(elemsPerInstr[0]),
        static_cast<unsigned>(elemsPerInstr[1])};
    std::array<unsigned, 2> warpStride = {
        static_cast<unsigned>(numReps[0] * elemsPerInstr[0]),
        static_cast<unsigned>(numReps[1] * elemsPerInstr[1])};

    Value dimWarpId0 = mul(multiDimWarpId[0], i32_val(warpStride[0]));
    Value dimWarpId1 = mul(multiDimWarpId[1], i32_val(warpStride[1]));
    Value warpId0Offset = add(dimWarpId0, offsetBaseY);
    Value warpId1Offset = add(dimWarpId1, offsetBaseX);
    unsigned valOffset = 0;
    for (int m = 0; m < numReps[0]; ++m) {
      Value offsetY = add(warpId0Offset, i32_val(m * replicaStride[0]));
      for (int n = 0; n < numReps[1]; ++n) {
        Value offsetX = add(warpId1Offset, i32_val(n * replicaStride[1]));

        Value storeVal = rewriter.create<LLVM::UndefOp>(
            loc, LLVM::getFixedVectorType(typeConverter->convertType(eltTy),
                                          elemsPerLane));
        for (size_t i = 0; i < elemsPerLane; ++i) {
          storeVal = insert_element(storeVal, vals[valOffset], i32_val(i));
          ++valOffset;
        }

        rewriter.create<TritonGEN::Matrix2DBlockStoreOp>(
            loc,
            /*ptr*/ base,
            /*base_width*/ baseWidth,
            /*base_height*/ height,
            /*base_pitch*/ basePitch,
            /*x*/ trunc(i32_ty, offsetX),
            /*y*/ trunc(i32_ty, offsetY),
            /*elem_size_in_bits*/ elemSizeInBits,
            /*tile_width*/ elemsPerInstr[1],
            /*tile_height*/ elemsPerInstr[0],
            /*v_blocks*/ 1,
            /*transpose*/ false,
            /*vnni_transform*/ false,
            /*stored_val*/ bitcast(storeVal, store2DGenXType));
      }
    }
    rewriter.eraseOp(op);
    return success();
  }

  LogicalResult
  matchAndRewrite(triton::StoreOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value ptr = op.getPtr();
    Value value = op.getValue();

    if (isTensorPointerType(ptr.getType()))
      return rewriteTensorPointerStore(op, adaptor, rewriter);

    Value llPtr = adaptor.getPtr();
    Value llMask = adaptor.getMask();
    Value llValue = adaptor.getValue();

    auto loc = op->getLoc();
    MLIRContext *ctx = rewriter.getContext();

    auto valueTy = value.getType();
    Type valueElemTy =
        typeConverter->convertType(getElementTypeOrSelf(valueTy));

    unsigned vec = getVectorSize(ptr);
    unsigned elemsPerThread = getTotalElemsPerThread(ptr.getType());

    auto ptrElems = unpackLLElements(loc, llPtr, rewriter);
    auto valueElems = unpackLLElements(loc, llValue, rewriter);
    assert(ptrElems.size() == valueElems.size());

    // Determine the vectorization size
    SmallVector<Value> maskElems;
    if (llMask) {
      Value mask = op.getMask();
      maskElems = unpackLLElements(loc, llMask, rewriter);
      assert(valueElems.size() == maskElems.size());

      unsigned maskAlign = getMaskAlignment(mask);
      vec = std::min(vec, maskAlign);
    }

    Value mask = redundantDataMask(valueTy, rewriter, loc, targetInfo);
    const size_t dtsize =
        std::max<int>(1, valueElemTy.getIntOrFloatBitWidth() / 8);
    const size_t valueElemNBits = dtsize * 8;

    const int numVecs = elemsPerThread / vec;
    for (size_t vecStart = 0; vecStart < elemsPerThread; vecStart += vec) {
      // TODO: optimization when ptr is AddPtr with constant offset
      size_t in_off = 0;

      const size_t maxWordWidth = std::max<size_t>(32, valueElemNBits);
      const size_t totalWidth = valueElemNBits * vec;
      const size_t width = std::min(totalWidth, maxWordWidth);
      const size_t nWords = std::max<size_t>(1, totalWidth / width);
      const size_t wordNElems = width / valueElemNBits;
      assert(wordNElems * nWords * numVecs == elemsPerThread);

      // TODO(Superjomn) Add cache policy fields to StoreOp.
      // TODO(Superjomn) Deal with cache policy here.

      Type valArgTy = IntegerType::get(ctx, width);
      auto wordTy = vec_ty(valueElemTy, wordNElems);

      SmallVector<std::pair<Value, std::string>> asmArgs;
      for (size_t wordIdx = 0; wordIdx < nWords; ++wordIdx) {
        // llWord is a width-len composition
        Value llWord = undef(wordTy);
        // Insert each value element to the composition
        for (size_t elemIdx = 0; elemIdx < wordNElems; ++elemIdx) {
          const size_t elemOffset = vecStart + wordIdx * wordNElems + elemIdx;
          assert(elemOffset < valueElems.size());
          Value elem = valueElems[elemOffset];
          if (elem.getType().isInteger(1))
            elem = sext(i8_ty, elem);
          elem = bitcast(elem, valueElemTy);

          llWord = insert_element(wordTy, llWord, elem, i32_val(elemIdx));
        }
        llWord = bitcast(llWord, valArgTy);
        std::string constraint =
            (width == 64) ? "l" : ((width == 32) ? "r" : "c");
        asmArgs.emplace_back(llWord, constraint);
      }

      Value maskVal = llMask ? and_(mask, maskElems[vecStart]) : mask;

      auto vecTy = vec_ty(valArgTy, nWords);
      Value vecWord = undef(vecTy);
      for (int index = 0; index < asmArgs.size(); ++index) {
        auto llWord = asmArgs[index].first;
        vecWord = insert_element(vecTy, vecWord, llWord, i32_val(index));
      }

      // Create a predicated store operation.
      LLVM::intel::createPredicatedBlock(rewriter, loc, maskVal, [&] {
        Value addrElem = bitcast(ptrElems[vecStart], ptr_ty(ctx, 1 /*global*/));
        uint32_t alignment = nWords * width / 8;
        store(vecWord, addrElem, alignment);
        return ArrayRef<Value>();
      });
    } // for
    rewriter.eraseOp(op);
    return success();
  }
};
void createBarrier(ConversionPatternRewriter &rewriter, Location loc,
                   int numCTAs) {
  assert(numCTAs == 1 && "Expecting numCTA to be 1");
  barrier();
}

struct AtomicCASOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::AtomicCASOp>,
      public LoadStoreConversionBase {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::AtomicCASOp>::ConvertTritonGPUOpToLLVMPattern;

  AtomicCASOpConversion(TritonIntelGPUToLLVMTypeConverter &converter,
                        const triton::intel::TargetInfo &targetInfo,
                        ModuleAxisInfoAnalysis &axisAnalysisPass,
                        PatternBenefit benefit)
      : ConvertTritonGPUOpToLLVMPattern<triton::AtomicCASOp>(converter,
                                                             benefit),
        LoadStoreConversionBase(targetInfo, axisAnalysisPass) {}

  LogicalResult
  matchAndRewrite(triton::AtomicCASOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    MLIRContext *ctx = rewriter.getContext();

    auto moduleOp = op->getParentOfType<ModuleOp>();
    assert(moduleOp && "Parent ModuleOp not found for AtomicCASOp");
    int numCTAs = triton::gpu::TritonGPUDialect::getNumCTAs(moduleOp);

    Value llPtr = adaptor.getPtr();
    Value llCmp = adaptor.getCmp();
    Value llVal = adaptor.getVal();

    auto ptrElements = unpackLLElements(loc, llPtr, rewriter);
    auto cmpElements = unpackLLElements(loc, llCmp, rewriter);
    auto valElements = unpackLLElements(loc, llVal, rewriter);

    auto valueTy = op.getType();
    auto tensorTy = dyn_cast<RankedTensorType>(valueTy);
    Type valueElemTy =
        tensorTy ? getTypeConverter()->convertType(tensorTy.getElementType())
                 : valueTy;
    auto valueElemNBits = valueElemTy.getIntOrFloatBitWidth();
    auto elemsPerThread = getTotalElemsPerThread(op.getVal().getType());
    // vec = 1 for scalar
    auto vec = getVectorSize(op.getPtr());
    // tensor
    if (tensorTy) {
      auto valTy = cast<RankedTensorType>(op.getVal().getType());
      vec = std::min<unsigned>(vec, valTy.getElementType().isF16() ? 2 : 1);
    }

    Value mask = redundantDataMask(valueTy, rewriter, loc, targetInfo);
    auto vecTy = vec_ty(valueElemTy, vec);
    SmallVector<Value> resultVals(elemsPerThread);

    for (size_t i = 0; i < elemsPerThread; i += vec) {
      Value casVal = undef(vecTy);
      for (int ii = 0; ii < vec; ++ii) {
        Value iiVal = createIndexAttrConstant(
            rewriter, loc, getTypeConverter()->getIndexType(), ii);
        casVal = insert_element(vecTy, casVal, valElements[i + ii], iiVal);
      }

      Value casPtr = ptrElements[i];
      Value casCmp = cmpElements[i];
      casVal = valElements[i];

      assert((valueElemNBits == 32 || valueElemNBits == 64) &&
             "Unexpected width");

      Value zero = (valueElemNBits == 32) ? i32_val(0) : i64_val(0);
      Block &endBlock =
          LLVM::intel::createPredicatedBlock(rewriter, loc, mask, {zero}, [&] {
            // casPtr = bitcast(casPtr, ptr_ty(ctx, 1));
            casCmp = bitcast(casCmp, zero.getType());
            casVal = bitcast(casVal, zero.getType());

            auto cmpxchg = rewriter.create<LLVM::AtomicCmpXchgOp>(
                loc, casPtr, casCmp, casVal, LLVM::AtomicOrdering::acq_rel,
                LLVM::AtomicOrdering::monotonic);
            Value newLoaded =
                rewriter.create<LLVM::ExtractValueOp>(loc, cmpxchg, 0);
            return SmallVector<Value, 1>{newLoaded};
          });

      Value ret = endBlock.getArgument(0);
      Type retType = (!tensorTy || vec == 1) ? valueElemTy : vecTy;
      ret = bitcast(ret, retType);

      if (tensorTy) {
        for (int ii = 0; ii < vec; ++ii) {
          resultVals[i + ii] =
              vec == 1 ? ret : extract_element(valueElemTy, ret, i32_val(ii));
        }
      } else {
        createBarrier(rewriter, loc, numCTAs);
        Value atomPtr =
            LLVM::intel::getSharedMemoryBase(loc, rewriter, op.getOperation());
        atomPtr = bitcast(atomPtr, ptr_ty(ctx, 3));
        targetInfo.storeShared(rewriter, loc, atomPtr, ret, mask);
        createBarrier(rewriter, loc, numCTAs);
        Value ret = load(valueElemTy, atomPtr);
        createBarrier(rewriter, loc, numCTAs);
        rewriter.replaceOp(op, {ret});
      }
    }

    if (tensorTy) {
      Type structTy = getTypeConverter()->convertType(tensorTy);
      Value resultStruct = packLLElements(loc, getTypeConverter(), resultVals,
                                          rewriter, structTy);
      rewriter.replaceOp(op, {resultStruct});
    }
    return success();
  }
};

struct AtomicRMWOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::AtomicRMWOp>,
      public LoadStoreConversionBase {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::AtomicRMWOp>::ConvertTritonGPUOpToLLVMPattern;

  AtomicRMWOpConversion(TritonIntelGPUToLLVMTypeConverter &converter,
                        const triton::intel::TargetInfo &targetInfo,
                        ModuleAxisInfoAnalysis &axisAnalysisPass,
                        PatternBenefit benefit)
      : ConvertTritonGPUOpToLLVMPattern<triton::AtomicRMWOp>(converter,
                                                             benefit),
        LoadStoreConversionBase(targetInfo, axisAnalysisPass) {}

  LogicalResult
  matchAndRewrite(triton::AtomicRMWOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    MLIRContext *ctx = rewriter.getContext();

    auto moduleOp = op->getParentOfType<ModuleOp>();
    assert(moduleOp && "Parent ModuleOp not found for AtomicRMWOp");
    int numCTAs = triton::gpu::TritonGPUDialect::getNumCTAs(moduleOp);

    auto atomicRmwAttr = op.getAtomicRmwOp();

    Value val = op.getVal();
    Value ptr = op.getPtr();

    Value llPtr = adaptor.getPtr();
    Value llVal = adaptor.getVal();
    Value llMask = adaptor.getMask();

    auto valElements = unpackLLElements(loc, llVal, rewriter);
    auto ptrElements = unpackLLElements(loc, llPtr, rewriter);
    SmallVector<Value> maskElements;
    if (llMask)
      maskElements = unpackLLElements(loc, llMask, rewriter);

    auto valueTy = op.getType();
    auto tensorTy = dyn_cast<RankedTensorType>(valueTy);
    Type valueElemTy =
        tensorTy ? getTypeConverter()->convertType(tensorTy.getElementType())
                 : valueTy;
    const size_t valueElemNBits = valueElemTy.getIntOrFloatBitWidth();
    auto elemsPerThread = getTotalElemsPerThread(val.getType());
    // vec = 1, numElements = 1 for scalar
    auto vec = getVectorSize(ptr);
    int numElems = 1;
    // tensor
    if (tensorTy) {
      auto valTy = cast<RankedTensorType>(val.getType());
      auto maxVecSize =
          valueElemNBits / valTy.getElementType().getIntOrFloatBitWidth();
      vec = std::min<unsigned>(vec,
                               valTy.getElementType().isF16() ? maxVecSize : 1);
      // mask
      numElems = tensorTy.getNumElements();
    }
    Value mask = redundantDataMask(valueTy, rewriter, loc, targetInfo);

    auto vecTy = vec_ty(valueElemTy, vec);
    SmallVector<Value> resultVals(elemsPerThread);
    for (size_t i = 0; i < elemsPerThread; i += vec) {
      Value rmwVal = undef(vecTy);
      for (int ii = 0; ii < vec; ++ii) {
        Value iiVal = createIndexAttrConstant(
            rewriter, loc, getTypeConverter()->getIndexType(), ii);
        rmwVal = insert_element(vecTy, rmwVal, valElements[i + ii], iiVal);
      }

      Value rmwPtr = ptrElements[i];
      Value rmwMask = llMask ? and_(mask, maskElements[i]) : mask;

      assert((valueElemNBits == 16 || valueElemNBits == 32 ||
              valueElemNBits == 64) &&
             "Unexpected width");

      Value zero;
      llvm::TypeSwitch<mlir::Type>(valueElemTy)
          .Case<mlir::IntegerType>(
              [&](auto ty) { zero = int_val(valueElemNBits, 0); })
          .Case<mlir::Float16Type>([&](auto ty) { zero = f16_val(0); })
          .Case<mlir::Float32Type>([&](auto ty) { zero = f32_val(0); })
          .Case<mlir::Float64Type>([&](auto ty) { zero = f64_val(0); });

      Block *endBlock = nullptr;
      // TODO: check device capabilities to avoid unnecessary emulation or
      // emit unsupported feature error.
      if (valueElemNBits == 16) {
        op.emitWarning(
            "'tt.atomic_rmw' op fp16 datatype is not supported in the target "
            "HW, software emulation is an experimental feature (use at own "
            "risk)");
        endBlock =
            emulateFp16AtomicRmw(rewriter, loc, atomicRmwAttr, valueElemTy,
                                 rmwPtr, rmwVal, rmwMask, {zero});
      } else {
        endBlock = &LLVM::intel::createPredicatedBlock(
            rewriter, loc, rmwMask, {zero}, [&] {
              mlir::LLVM::AtomicBinOp rmwKind;
              switch (atomicRmwAttr) {
              case RMWOp::AND:
                rmwKind = LLVM::AtomicBinOp::_and;
                break;
              case RMWOp::OR:
                rmwKind = LLVM::AtomicBinOp::_or;
                break;
              case RMWOp::XOR:
                rmwKind = LLVM::AtomicBinOp::_xor;
                break;
              case RMWOp::ADD:
                rmwKind = LLVM::AtomicBinOp::add;
                break;
              case RMWOp::FADD:
                rmwKind = LLVM::AtomicBinOp::fadd;
                break;
              case RMWOp::MAX:
                rmwKind = LLVM::AtomicBinOp::max;
                break;
              case RMWOp::UMAX:
                rmwKind = LLVM::AtomicBinOp::umax;
                break;
              case RMWOp::MIN:
                rmwKind = LLVM::AtomicBinOp::min;
                break;
              case RMWOp::UMIN:
                rmwKind = LLVM::AtomicBinOp::umin;
                break;
              case RMWOp::XCHG:
                rmwKind = LLVM::AtomicBinOp::xchg;
                break;
              }

              rmwVal = bitcast(rmwVal, valueElemTy);
              auto atomRMW = rewriter.create<LLVM::AtomicRMWOp>(
                  loc, rmwKind, rmwPtr, rmwVal, LLVM::AtomicOrdering::acq_rel);
              return SmallVector<Value, 1>{atomRMW.getRes()};
            });
      }

      Value ret = endBlock->getArgument(0);
      Type retType = (!tensorTy || vec == 1) ? valueElemTy : vecTy;
      ret = bitcast(ret, retType);

      if (tensorTy) {
        for (int ii = 0; ii < vec; ++ii) {
          resultVals[i + ii] =
              vec == 1 ? ret : extract_element(valueElemTy, ret, i32_val(ii));
        }
      } else {
        Value atomPtr =
            LLVM::intel::getSharedMemoryBase(loc, rewriter, op.getOperation());
        atomPtr = bitcast(atomPtr, ptr_ty(ctx, 3));
        // Only threads with rmwMask = True store the result
        targetInfo.storeShared(rewriter, loc, atomPtr, ret, rmwMask);
        createBarrier(rewriter, loc, numCTAs);
        Value loadVal = load(valueElemTy, atomPtr);
        createBarrier(rewriter, loc, numCTAs);
        rewriter.replaceOp(op, {loadVal});
      }
    }

    if (tensorTy) {
      Type structTy = getTypeConverter()->convertType(tensorTy);
      Value resultStruct = packLLElements(loc, getTypeConverter(), resultVals,
                                          rewriter, structTy);
      rewriter.replaceOp(op, {resultStruct});
    }
    return success();
  }

  // Emulate 16-bit atomicrmw through a loop with 32-bit cmpxchg.
  Block *emulateFp16AtomicRmw(ConversionPatternRewriter &rewriter, Location loc,
                              mlir::triton::RMWOp atomicOp, Type valueElemTy,
                              Value rmwPtr, Value rmwVal, Value rmwMask,
                              ArrayRef<Value> ops) const {
    Block *insertionBlock = rewriter.getInsertionBlock();
    Block *headerBlock =
        rewriter.splitBlock(insertionBlock, rewriter.getInsertionPoint());
    Block *endBlock = rewriter.splitBlock(headerBlock, headerBlock->begin());
    rewriter.setInsertionPointToEnd(insertionBlock);
    rewriter.create<cf::CondBranchOp>(loc, rmwMask, headerBlock, endBlock, ops);
    rewriter.setInsertionPointToStart(headerBlock);

    rmwVal = bitcast(rmwVal, valueElemTy);

    // Align pointer by 4 bytes by zeroing lower address bits. Atomically read
    // a vector of two fp16 values as a single i32. The second lowest bit is
    // extracted to later be used as an index to extract the required vector
    // element.
    assert(isa<LLVM::LLVMPointerType>(rmwPtr.getType()));
    auto intPtr = ptrtoint(i64_ty, rmwPtr);
    auto lowPtrBits = and_(intPtr, i64_val(3));
    auto elemIndex = trunc(i32_ty, lshr(lowPtrBits, i64_val(1)));
    auto alignPtr = inttoptr(rmwPtr.getType(), sub(intPtr, lowPtrBits));
    auto firstValInt = load(i32_ty, alignPtr, 4, false, false, false,
                            LLVM::AtomicOrdering::acquire);

    // Create a loop body block. It has a single parameter which holds the
    // latest loaded i32 value.
    Block *bodyBlock =
        rewriter.splitBlock(headerBlock, rewriter.getInsertionPoint());
    auto origValInt =
        bodyBlock->addArgument(firstValInt.getType(), firstValInt.getLoc());
    rewriter.setInsertionPointToEnd(headerBlock);
    rewriter.create<cf::BranchOp>(loc, bodyBlock,
                                  SmallVector<Value, 1>{firstValInt});
    rewriter.setInsertionPointToEnd(bodyBlock);

    // Extract value for modification.
    auto origValVec = bitcast(origValInt, vec_ty(valueElemTy, 2));
    Value origVal = extract_element(origValVec, elemIndex);

    // Apply operation.
    Value newVal = nullptr;
    switch (atomicOp) {
    case RMWOp::FADD:
      newVal = rewriter.create<LLVM::FAddOp>(loc, origVal, rmwVal);
      break;
    case RMWOp::MAX:
      newVal = rewriter.create<LLVM::MaximumOp>(loc, origVal, rmwVal);
      break;
    case RMWOp::MIN:
      newVal = rewriter.create<LLVM::MinimumOp>(loc, origVal, rmwVal);
      break;
    case RMWOp::XCHG:
      newVal = rmwVal;
      break;
    default:
      llvm_unreachable("Unsupported FP16 atomic op");
    }

    // Use modified value to form a new i32 value to write to memory.
    assert(newVal);
    Value newValVec = insert_element(origValVec, newVal, elemIndex);
    Value newValInt = bitcast(newValVec, i32_ty);

    // Execute cmpxchg and loop back if it fails.
    auto successOrdering = LLVM::AtomicOrdering::acq_rel;
    auto failureOrdering = LLVM::AtomicOrdering::monotonic;
    auto cmpxchg = rewriter.create<LLVM::AtomicCmpXchgOp>(
        loc, alignPtr, origValInt, newValInt, successOrdering, failureOrdering);
    auto newLoaded = extract_val(cmpxchg, 0);
    auto done = extract_val(cmpxchg, 1);
    assert(ops.size() == (size_t)1);
    SmallVector<Value, 1> endOps = {origVal};
    rewriter.create<cf::CondBranchOp>(loc, done, endBlock, endOps, bodyBlock,
                                      SmallVector<Value, 1>{newLoaded});

    for (Value op : ops)
      endBlock->addArgument(op.getType(), op.getLoc());

    rewriter.setInsertionPointToStart(endBlock);
    return endBlock;
  }
};

} // namespace

void mlir::triton::intel::populateLoadStoreOpToLLVMPatterns(
    TritonIntelGPUToLLVMTypeConverter &typeConverter,
    const TargetInfo &targetInfo, RewritePatternSet &patterns,
    ModuleAxisInfoAnalysis &axisInfoAnalysis, PatternBenefit benefit) {
  patterns.add<AtomicCASOpConversion, AtomicRMWOpConversion, LoadOpConversion,
               StoreOpConversion, PrefetchOpConversion>(
      typeConverter, targetInfo, axisInfoAnalysis, benefit);
}
