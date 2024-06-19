// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "precomp.h"

// With a single equation, the Einstein summation operator can represent a variety of operators including: matmul,
// summation, transposition, diagonal slice, diagonal sum (trace), inner (dot) product, outer product...
//
// Parameters                   NumPy equivalent                Description
// -------------------------------------------------------------------------------------------------------------
// ('i', A1)                    A1                              returns a view of A1
// ('i->', A1)                  sum(A1)                         sums the values of A1
// ('i,i->i', A1, B1)           A1 * B1                         element-wise multiplication of A1 and B1
// ('i,i->', A1, B1)            inner(A1, B1) or dot(A1, B1)    inner product of A1 and B1
// ('i,i', A1, B1)              inner(A1, B1) or dot(A1, B1)    inner product of A1 and B1
// ('i,j->ij', A1, B1)          outer(A1, B1)                   outer product of A1 and B1
// ('ij->ij', A2)               A2                              returns a view of A2
// ('ij', A2)                   A2                              returns a view of A2
// ('ji', A2)                   A2.T                            view transpose of A2
// ('ji->ij', A2)               A2.T                            view transpose of A2
// ('ii->i', A2)                diag(A2)                        view main diagonal of A2
// ('ii->', A2)                 trace(A2)                       sums main diagonal of A2
// ('ij->', A2)                 sum(A2)                         sums the values of A2
// ('ij->j', A2)                sum(A2, axis=0)                 sum down the columns of A2 (across rows)
// ('ij->i', A2)                sum(A2, axis=1)                 sum horizontally along the rows of A2
// ('ij,ij->ij', A2, B2)        A2 * B2                         element-wise multiplication of A2 and B2
// ('ij,ji->ij', A2, B2)        A2 * B2.transpose()             element-wise multiplication of A2 and B2.T
// ('ij,jk', A2, B2)            matmul(A2, B2) or dot(A2, B2)   matrix multiplication of A2 and B2
// ('ij,jk->ik', A2, B2)        matmul(A2, B2) or dot(A2, B2)   matrix multiplication of A2 and B2
// ('bij,bjk->bik', A2, B2)     matmul(A3, B3)                  matrix multiplication of A3 and B3 (a stack of 2D matrices)
// ('bij,bkj->bik', A2, B2)     matmul(A3, transpose(B3))       matrix multiplication of A3 and B3 (a stack of 2D matrices)
// ('ij,kj->ik', A2, B2)        inner(A2, B2)                   inner product of A2 and B2
// ('ij,kj->ikj', A2, B2)       A2[:, None] * B2                each row of A2 multiplied by B2
// ('ij,kl->ijkl', A2, B2)      A2[:, :, None, None] * B2       each value of A2 multiplied by B2
// (',ij', 3, B2)                                               Scalar times array: array([[ 0, 3, 6], [ 9, 12, 15]])
// ("ij,j", A2, B1)             matvec(A2, B1)                  Matrix and vector.
// ("ii,ii->i", A2, B2)         A2.diag() * B2.diag()           diagonals multiplied by each other
// ("ii,ii->", A2, B2)          dot(A2.diag(), B2.diag())       dot product of diagonals
//
// Decomposition:
//
// Ultimately though EinSum is equivalent to an elementwise multiplication into an internal product tensor
// (given a helper function to reproject all inputs so they're shape-compatible) followed by sum reduction.
//
// 1. Determine the size of the internal product tensor by concatenating the dimensions of all inputs,
//    counting each unique label once. So "bij,bjk->bik" would yield an internal product of shape [b,i,j,k].
// 2. Project each input tensor as needed to the internal product shape (transposing and/or broadcasting).
//    So an input of shape [b,i] with product shape of [b,j,i,k] would insert broadcasted j and k dimensions.
//    An input of shape [a,b,c] with product shape of [b,c,a] would require a transpose.
// 3. Multiply elementwise every input tensor into the internal product.
// 4. Sum reduce the product tensor to the final output shape, reducing along the missing dimensions.
//    So a product shape of [b,j,i,k] and output shape of [b,i,k] reduces along j.
// 
//  ReduceSum(
//      Mul(
//          ExpandAndTransposeAsNeeded(A, aAxesToProductAxes),
//          ExpandAndTransposeAsNeeded(B, bAxesToProductAxes),
//      ),
//      reductionAxes,
//      keepdims=false
//  )
//
// Notes:
//
// - DirectML has no direct EinSum operator, but common cases map to existing operators.
// - EinSum can accept a variable number of input tensors, but the DML EP only supports a limited count
//   (falling back to CPU otherwise).

namespace Dml
{

class DmlOperatorEinSum : public DmlOperator, public EinSumHelper
{
public:
    DmlOperatorEinSum(const MLOperatorKernelCreationContext& kernelCreationContext, uint32_t opsetVersion)
    :   DmlOperator(kernelCreationContext),
        EinSumHelper(kernelCreationContext, kernelCreationContext.GetTensorShapeDescription(), opsetVersion)
    {
        ML_CHECK_VALID_ARGUMENT(kernelCreationContext.GetInputCount() >= 1, "EinSum expects at least one input tensor.");
        ML_CHECK_VALID_ARGUMENT(kernelCreationContext.GetOutputCount() == 1, "EinSum expects one output tensor.");
        ML_CHECK_VALID_ARGUMENT(
            static_cast<uint64_t>(kernelCreationContext.GetInputCount()) + 1 == m_components.size(),
            "EinSum input tensor count is inconsistent with the equation component count."
        );
        assert(m_recognizedOperatorType != RecognizedOperatorType::None); // Should not have reached here because fallback to CPU happened.

        std::vector<std::optional<uint32_t>> inputIndices = {0,1,2};
        std::vector<std::optional<uint32_t>> outputIndices = {0};
        uint32_t bindableInputCount = kernelCreationContext.GetInputCount();
        if (IsMatMulOperatorType())
        {
            ++bindableInputCount; // Account for the optional C tensor.
        }
        inputIndices.resize(bindableInputCount);

        uint32_t minimumDimensionCount = 1;
        DmlOperator::Initialize(kernelCreationContext, inputIndices, outputIndices, std::nullopt, std::nullopt, minimumDimensionCount);

        std::vector<DML_TENSOR_DESC> inputDescs = GetDmlInputDescs();
        std::vector<DML_TENSOR_DESC> outputDescs = GetDmlOutputDescs();

        static_assert(RecognizedOperatorType::Total == static_cast<RecognizedOperatorType>(12), "Update this switch.");
        switch (m_recognizedOperatorType)
        {
        case RecognizedOperatorType::Multiply:
            {
                ReprojectTensorDescsToProductTensor();

                DML_ELEMENT_WISE_MULTIPLY_OPERATOR_DESC operatorDesc = {};
                operatorDesc.ATensor = &inputDescs[0];
                operatorDesc.BTensor = &inputDescs[1];
                operatorDesc.OutputTensor = outputDescs.data();

                SetDmlOperatorDesc({ DML_OPERATOR_ELEMENT_WISE_MULTIPLY, &operatorDesc}, kernelCreationContext);
            }
            break;

        case RecognizedOperatorType::MatMul:
        case RecognizedOperatorType::MatMulTransposeA:
        case RecognizedOperatorType::MatMulTransposeB:
        case RecognizedOperatorType::MatMulNhcw:
        case RecognizedOperatorType::MatMulNhcwTransposeA:
        case RecognizedOperatorType::MatMulNhcwTransposeB:
        case RecognizedOperatorType::MatMulGeneral:
            {
                assert(m_components.size() == 3); // 2 inputs, 1 output
                assert(m_productDimensions.size() - 1 <= 4); // Up to 4D, as MatMul reduces 1 dimension from the internal product.

                // Generate bitmasks for each of the active axes per tensor using their labels.
                const auto input0Labels = m_components[0].GetLabels(m_labelIndices);
                const auto input1Labels = m_components[1].GetLabels(m_labelIndices);
                const auto outputLabels = m_components[2].GetLabels(m_labelIndices);
                const uint32_t input0AxesMask = GetBitMaskFromIndices(input0Labels);
                const uint32_t input1AxesMask = GetBitMaskFromIndices(input1Labels);
                const uint32_t outputAxesMask = GetBitMaskFromIndices(outputLabels);

                // Find each of the interesting axes, including the one being reduced, height, width, batch, and channel.
                // - the reduced axis is the term missing from the output.
                // - height and width are the unique axes respectively found in only input A or input B.
                // - the batch (if present) is the first axis shared by both inputs, and the channel is the subsequent common one.

                auto findAndClearAxis = [](uint32_t& currentAxesMask, uint32_t contraintAxesMask) -> uint32_t
                {
                    uint32_t foundAxis = CountLeastSignificantZeros(currentAxesMask & ~contraintAxesMask);
                    currentAxesMask &= ~(1 << foundAxis);
                    return foundAxis;
                };

                uint32_t remainingAxesMask = ~0u;
                uint32_t reductionAxis     = findAndClearAxis(/*inout*/ remainingAxesMask, outputAxesMask);
                uint32_t heightAxis        = findAndClearAxis(/*inout*/ remainingAxesMask, input1AxesMask);
                uint32_t widthAxis         = findAndClearAxis(/*inout*/ remainingAxesMask, input0AxesMask);
                uint32_t batchAxis         = findAndClearAxis(/*inout*/ remainingAxesMask, 0);
                uint32_t channelAxis       = findAndClearAxis(/*inout*/ remainingAxesMask, 0);

                // Reproject all inputs and the output to the needed order pattern for DML compatibility,
                // which only accepts the rightmost axis as GEMM-reducible when TransB is true.
                ReprojectTensorDescToGivenAxes(/*inout*/ m_inputTensorDescs[0],  input0Labels, {{batchAxis, channelAxis, heightAxis, reductionAxis}});
                ReprojectTensorDescToGivenAxes(/*inout*/ m_inputTensorDescs[1],  input1Labels, {{batchAxis, channelAxis, widthAxis, reductionAxis}});
                ReprojectTensorDescToGivenAxes(/*inout*/ m_outputTensorDescs[0], outputLabels, {{batchAxis, channelAxis, heightAxis, widthAxis}});

                DML_GEMM_OPERATOR_DESC operatorDesc = {};
                operatorDesc.ATensor = &inputDescs[0];
                operatorDesc.BTensor = &inputDescs[1];
                // No operatorDesc.CTensor
                operatorDesc.OutputTensor = &outputDescs[0];
                operatorDesc.TransA = DML_MATRIX_TRANSFORM_NONE;
                operatorDesc.TransB = DML_MATRIX_TRANSFORM_TRANSPOSE;
                operatorDesc.Alpha = 1.0;
                operatorDesc.Beta = 0.0;
                operatorDesc.FusedActivation = nullptr;

                SetDmlOperatorDesc({ DML_OPERATOR_GEMM, &operatorDesc }, kernelCreationContext);
            }
            break;

        case RecognizedOperatorType::ReduceSum:
            {
                ReprojectTensorDescsToProductTensor();

                // Determine which axes are reduced by looking for any output dimensions of size 1.
                // Note this could include dimensions that are not actually being reduced and simply
                // already had size 1 from the input, but such cases harmless nops either way.
                // DML expects the input rank to match output rank (as if ONNX ReduceSum keepdims=1)
                // with reduced output dimensions having size 1, which is handled naturally in the
                // projection call.

                auto outputSizes = m_outputTensorDescs.front().GetSizes();
                std::vector<uint32_t> reducedAxes;
                for (uint32_t axis = 0, axisCount = static_cast<uint32_t>(outputSizes.size()); axis < axisCount; ++axis)
                {
                    if (outputSizes[axis] == 1)
                    {
                        reducedAxes.push_back(axis);
                    }
                }

                DML_REDUCE_OPERATOR_DESC operatorDesc = {};
                operatorDesc.InputTensor = inputDescs.data();
                operatorDesc.OutputTensor = outputDescs.data();
                operatorDesc.Function = DML_REDUCE_FUNCTION_SUM;
                operatorDesc.Axes = reducedAxes.data();
                operatorDesc.AxisCount = gsl::narrow_cast<uint32_t>(reducedAxes.size());

                SetDmlOperatorDesc({ DML_OPERATOR_REDUCE, &operatorDesc }, kernelCreationContext);
            }
            break;

        case RecognizedOperatorType::Transpose:
        case RecognizedOperatorType::Identity:
            {
                if (m_recognizedOperatorType == RecognizedOperatorType::Transpose)
                {
                    // Needed if transposing but not if identity.
                    ReprojectTensorDescsToProductTensor();
                }

                DML_ELEMENT_WISE_IDENTITY_OPERATOR_DESC operatorDesc = {};
                operatorDesc.InputTensor = inputDescs.data();
                operatorDesc.OutputTensor = outputDescs.data();

                SetDmlOperatorDesc({ DML_OPERATOR_ELEMENT_WISE_IDENTITY, &operatorDesc}, kernelCreationContext);
            }
            break;

        default:
            return;
        }
    }

    // Reproject all inputs and output to the intermediate product tensor.
    // e.g.
    // 
    //      Equation: i,j->ij
    // 
    //      [1,2,3] [4]    [4, 8,12]
    //              [5] -> [5,10,15]
    //              [6]    [6,12,18]
    //
    //      Inputs 0 and 1 are expanded to be directly broadcast-compatible.
    //
    //      [1,2,3] [4,4,4]    [4, 8,12]
    //      [1,2,3] [5,5,5] -> [5,10,15]
    //      [1,2,3] [6,6,6]    [6,12,18]
    //
    void ReprojectTensorDescsToProductTensor()
    {
        assert(!m_components.empty());
        assert(m_inputTensorDescs.size() + m_outputTensorDescs.size() == m_components.size());

        for (size_t i = 0, count = m_inputTensorDescs.size(); i < count; ++i)
        {
            auto inputLabels = m_components[i].GetLabels(m_labelIndices);
            ReprojectTensorDescToProductTensor(/*inout*/ m_inputTensorDescs[i], inputLabels, /*isReduced*/ false);
        }
        auto outputLabels = m_components.back().GetLabels(m_labelIndices);
        ReprojectTensorDescToProductTensor(/*inout*/ m_outputTensorDescs.front(), outputLabels, /*isReduced*/ true);
    }

    // Transpose/broadcast the given tensor for shape compatibility to the internal product tensor.
    // e.g.
    //
    //      Original tensor shape:   [2,3,4]
    //      Original tensor strides: [12,4,1]    // packed strides right-to-left
    //      Product tensor shape:    [3,5,4,2]   // transposed, with 1 additional axis not in input tensor
    //      Reprojected shape:       [3,5,4,2]   or [3,1,4,2] when isReduced is true
    //      Reprojected strides:     [4,0,1,12]
    //
    void ReprojectTensorDescToProductTensor(
        /*inout*/ TensorDesc& tensorDesc,
        gsl::span<const uint32_t> axisLabels,
        bool isReduced // Return 1's for any missing dimensions not in axisLabels.
    )
    {
        assert(m_productDimensions.size() == m_uniqueLabelCount);
        const size_t newRank = m_productDimensions.size();

        // Compute the default strides of the tensor (non-transposed).
        tensorDesc.EnsureStridesExist();
        const auto originalSizes = tensorDesc.GetSizes();
        const auto originalStrides = tensorDesc.GetStrides();
        assert(originalSizes.size() >= axisLabels.size());
        assert(originalStrides.size() >= axisLabels.size());

        // Set default sizes for shape compatibility with the product tensor, and
        // set strides to 0's initially to broadcast any missing dimensions.
        std::vector<uint32_t> newSizes;
        std::vector<uint32_t> newStrides(newRank, 0u); // Default to 0 to broadcast missing entries.
        if (isReduced)
        {
            newSizes.resize(newRank, 1u); // Fill with 1's initially for any missing dimensions (reduced).
        }
        else
        {
            newSizes = m_productDimensions; // Use the product tensor shape directly. Missing axes will be broadcasted.
        }

        // Scatter the original sizes and strides into the corresponding product tensor axis.
        for (size_t i = 0, count = axisLabels.size(); i < count; ++i)
        {
            uint32_t productAxis = axisLabels[i];
            if (productAxis < newRank)
            {
                newSizes[productAxis] = originalSizes[i];
                newStrides[productAxis] += originalStrides[i]; // Add to handle diagonal cases like i,j,i->i,j
            }
        }
        tensorDesc.SetDimensionsAndStrides(newSizes, newStrides);
        tensorDesc.EnsureDimensionCount(1, TensorAxis::RightAligned);
    }

    // Reproject a tensor to the given axis arrangement.
    // The new tensor will have rank == newAxes.size().
    // e.g.
    //
    //      product tensor shape = [2,3,4,5,6] // m_productDimensions
    //      newAxes              = [4,2,0,1]
    //      new tensor shape     = [6,4,2,3]
    //
    void ReprojectTensorDescToGivenAxes(
        /*inout*/ TensorDesc& tensorDesc,
        gsl::span<const uint32_t> axisLabels,
        gsl::span<const uint32_t> newAxes
    )
    {
        // First, reproject the original dimensions up to the product tensor.
        ReprojectTensorDescToProductTensor(/*inout*/ tensorDesc, axisLabels, /*isReduced*/ false);
        tensorDesc.PermuteDimensions(newAxes, TensorAxis::LeftAligned);
    }
};

void CALLBACK QueryEinSum(IMLOperatorSupportQueryContextPrivate* context, bool* isSupported)
{
    *isSupported = false;

    MLOperatorAttributes attributes(context);
    EinSumHelper helper(attributes);
    auto recognizedOperatorType = helper.GetRecognizedOperatorType();

    static_assert(EinSumHelper::RecognizedOperatorType::Total == static_cast<EinSumHelper::RecognizedOperatorType>(12), "Update this function.");
    *isSupported = (recognizedOperatorType != EinSumHelper::RecognizedOperatorType::None);
}

DML_OP_DEFINE_CREATION_FUNCTION(Einsum12, VersionedKernel<DmlOperatorEinSum, 12>);

} // namespace Dml
