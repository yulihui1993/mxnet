/*!
 *  Copyright (c) 2015 by Contributors
 * \file elementwise_binary_broadcast_op.h
 * \brief Function defintion of elementwise unary operators
 */
#ifndef MXNET_OPERATOR_TENSOR_ELEMWISE_BINARY_BROADCAST_OP_H_
#define MXNET_OPERATOR_TENSOR_ELEMWISE_BINARY_BROADCAST_OP_H_

#include <mxnet/operator_util.h>
#include <algorithm>
#include <vector>
#include <string>
#include <utility>
#include "../mshadow_op.h"
#include "../elemwise_op_common.h"
#include "./elemwise_binary_op.h"
#include "../operator_common.h"

namespace mxnet {
namespace op {
inline bool BinaryBroadcastShape(const nnvm::NodeAttrs& attrs,
                                 std::vector<TShape> *in_attrs,
                                 std::vector<TShape> *out_attrs) {
  CHECK_EQ(in_attrs->size(), 2);
  CHECK_EQ(out_attrs->size(), 1);
  TShape& lhs = (*in_attrs)[0];
  TShape& rhs = (*in_attrs)[1];

  // avoid pre-mature shape inference.
  if (lhs.ndim() == 0 || rhs.ndim() == 0) return false;

  if (lhs == rhs) {
    SHAPE_ASSIGN_CHECK(*out_attrs, 0, lhs);
    return true;
  }

  TShape out(std::max(lhs.ndim(), rhs.ndim()));
  std::fill_n(out.begin(), out.ndim(), 0);
  index_t bl = out.ndim() - lhs.ndim();
  index_t br = out.ndim() - rhs.ndim();
  for (index_t i = 0; i < out.ndim(); ++i) {
    index_t l = 1, r = 1;
    if (i >= bl) l = lhs[i-bl];
    if (i >= br) r = rhs[i-br];
    if (l != r) {
      if (l == 0 || r == 0) {
        out[i] = 0;
      } else {
        CHECK(l == 1 || r == 1)
          << "operands could not be broadcast together with shapes " << lhs << " " << rhs;
        out[i] = std::max(l, r);
      }
    } else {
      out[i] = l;
    }
  }
  SHAPE_ASSIGN_CHECK(*out_attrs, 0, out);
  return true;
}

inline bool BinaryBroadcastShapeCompact(const TShape& lshape, const TShape& rshape,
                                        const TShape& oshape, TShape *new_lshape,
                                        TShape *new_rshape, TShape *new_oshape) {
  if (lshape == rshape) return false;
  index_t odim = std::max<index_t>(oshape.ndim(), MXNET_SPECIAL_MAX_NDIM);
  *new_lshape = TShape(odim);
  *new_rshape = TShape(odim);
  *new_oshape = TShape(odim);
  index_t bl = oshape.ndim() - lshape.ndim();
  index_t br = oshape.ndim() - rshape.ndim();
  index_t j = 0, lprod = 1, rprod = 1, oprod = 1;
  for (index_t i = 0; i < oshape.ndim(); ++i) {
    index_t l = 1, r = 1, o = oshape[i];
    if (i >= bl) l = lshape[i-bl];
    if (i >= br) r = rshape[i-br];
    if ((lprod != rprod || l != r) &&
        lprod*l > 1 && rprod*r > 1) {
      (*new_lshape)[j] = lprod;
      (*new_rshape)[j] = rprod;
      (*new_oshape)[j] = oprod;
      lprod = rprod = oprod = 1; ++j;
    }
    lprod *= l;
    rprod *= r;
    oprod *= o;
  }
  if (lprod > 1 || rprod > 1) {
    (*new_lshape)[j] = lprod;
    (*new_rshape)[j] = rprod;
    (*new_oshape)[j] = oprod;
    ++j;
  }
  if (j <= 2) {
    new_lshape->assign(&(*new_lshape)[0], &(*new_lshape)[2]);
    new_rshape->assign(&(*new_rshape)[0], &(*new_rshape)[2]);
    new_oshape->assign(&(*new_oshape)[0], &(*new_oshape)[2]);
  } else if (j <= MXNET_SPECIAL_MAX_NDIM) {
    new_lshape->assign(&(*new_lshape)[0], &(*new_lshape)[MXNET_SPECIAL_MAX_NDIM]);
    new_rshape->assign(&(*new_rshape)[0], &(*new_rshape)[MXNET_SPECIAL_MAX_NDIM]);
    new_oshape->assign(&(*new_oshape)[0], &(*new_oshape)[MXNET_SPECIAL_MAX_NDIM]);
  } else {
    LOG(FATAL) << "Too many broadcast dimensions with operands " << lshape << " " << rshape;
  }
  return true;
}

template<typename xpu, int ndim, typename DType, typename OP>
inline void BinaryBroadcastComputeImpl(const OpContext& ctx,
                                              const std::vector<TBlob>& inputs,
                                              const std::vector<OpReqType>& req,
                                              const std::vector<TBlob>& outputs,
                                              const TShape& new_lshape,
                                              const TShape& new_rshape,
                                              const TShape& new_oshape) {
  using namespace mshadow;
  using namespace mshadow::expr;
  Stream<xpu> *s = ctx.get_stream<xpu>();
  Tensor<xpu, ndim, DType> out =
    outputs[0].get_with_shape<xpu, ndim, DType>(new_oshape.get<ndim>(), s);
  Tensor<xpu, ndim, DType> lhs =
    inputs[0].get_with_shape<xpu, ndim, DType>(new_lshape.get<ndim>(), s);
  Tensor<xpu, ndim, DType> rhs =
    inputs[1].get_with_shape<xpu, ndim, DType>(new_rshape.get<ndim>(), s);
  ASSIGN_DISPATCH(out, req[0], F<OP>(broadcast_to(lhs, new_oshape), broadcast_to(rhs, new_oshape)));
}

template<typename xpu, typename OP>
void BinaryBroadcastCompute(const nnvm::NodeAttrs& attrs,
                            const OpContext& ctx,
                            const std::vector<TBlob>& inputs,
                            const std::vector<OpReqType>& req,
                            const std::vector<TBlob>& outputs) {
  TShape new_lshape, new_rshape, new_oshape;
  bool need_bc = BinaryBroadcastShapeCompact(inputs[0].shape_, inputs[1].shape_, outputs[0].shape_,
                                             &new_lshape, &new_rshape, &new_oshape);
  if (!need_bc) {
    BinaryCompute<xpu, OP>(attrs, ctx, inputs, req, outputs);
  } else {
    MSHADOW_TYPE_SWITCH(outputs[0].type_flag_, DType, {
      if (new_oshape.ndim() == 2) {
        BinaryBroadcastComputeImpl<xpu, 2, DType, OP>(
          ctx, inputs, req, outputs, new_lshape, new_rshape, new_oshape);
      } else {
        BinaryBroadcastComputeImpl<xpu, MXNET_SPECIAL_MAX_NDIM, DType, OP>(
          ctx, inputs, req, outputs, new_lshape, new_rshape, new_oshape);
      }
    });
  }
}

template<typename Reducer, typename xpu, typename SrcExp, int ndim, typename DType>
void ReduceToAssign(mshadow::Tensor<xpu, ndim, DType> out,
                    const OpReqType req, const SrcExp &src_) {
  using namespace mshadow;
  using namespace mshadow::expr;
  Shape<ndim> src_shape = ShapeCheck<ndim, SrcExp>::Check(src_);
  Shape<ndim> axes;
  index_t reducing_size = 1, remaining_size = 1;
  int i = 0;
  for (int k = 0; k < ndim; ++k)
    if (src_shape[k] != out.shape_[k])
      ++i;
  for (int j = ndim-1, k = ndim-1; k >= 0; --k) {
    if (src_shape[k] == out.shape_[k]) {
      axes[j--] = k;
      remaining_size *= src_shape[k];
    } else {
      axes[--i] = k;
      reducing_size *= src_shape[k];
    }
  }
  if (reducing_size == 1) {
    ASSIGN_DISPATCH(out, req, F<mshadow_op::identity>(src_));
  } else {
    ASSIGN_DISPATCH(out.FlatTo1D(), req,
      (reduce_except_dim<1, Reducer>(reshape(transpose(src_, axes),
      Shape2(reducing_size, remaining_size)))));
  }
}

template<typename Reducer, typename xpu, typename SrcExp, typename DType>
void ReduceToAssign(mshadow::Tensor<xpu, 2, DType> out, const OpReqType req, const SrcExp &src_) {
  using namespace mshadow;
  using namespace mshadow::expr;
  Shape<2> src_shape = ShapeCheck<2, SrcExp>::Check(src_);
  if (src_shape == out.shape_) {
    ASSIGN_DISPATCH(out, req, F<mshadow_op::identity>(src_));
  } else if (src_shape[0] == out.shape_[0]) {
    ASSIGN_DISPATCH(out.FlatTo1D(), req, (reduce_except_dim<0, Reducer>(src_)));
  } else if (src_shape[1] == out.shape_[1]) {
    ASSIGN_DISPATCH(out.FlatTo1D(), req, (reduce_except_dim<1, Reducer>(src_)));
  } else {
    ASSIGN_DISPATCH(out.FlatTo1D(), req,
      (reduce_except_dim<1, Reducer>(reshape(src_,
      Shape2(src_shape.Size(), 1)))));
  }
}

template<typename xpu, int ndim, typename DType, typename LOP, typename ROP>
inline void BinaryBroadcastBackwardUseNoneImpl(const OpContext& ctx,
                                               const std::vector<TBlob>& inputs,
                                               const std::vector<OpReqType>& req,
                                               const std::vector<TBlob>& outputs,
                                               const TShape& new_lshape,
                                               const TShape& new_rshape,
                                               const TShape& new_oshape) {
  using namespace mshadow;
  using namespace mshadow::expr;
  Stream<xpu> *s = ctx.get_stream<xpu>();
  Tensor<xpu, ndim, DType> ograd =
    inputs[0].get_with_shape<xpu, ndim, DType>(new_oshape.get<ndim>(), s);
  Tensor<xpu, ndim, DType> lgrad =
    outputs[0].get_with_shape<xpu, ndim, DType>(new_lshape.get<ndim>(), s);
  Tensor<xpu, ndim, DType> rgrad =
    outputs[1].get_with_shape<xpu, ndim, DType>(new_rshape.get<ndim>(), s);
  ReduceToAssign<red::sum>(lgrad, req[0], F<LOP>(ograd));
  ReduceToAssign<red::sum>(rgrad, req[1], F<ROP>(ograd));
}

template<typename xpu, typename LOP, typename ROP>
void BinaryBroadcastBackwardUseNone(const nnvm::NodeAttrs& attrs,
                                    const OpContext& ctx,
                                    const std::vector<TBlob>& inputs,
                                    const std::vector<OpReqType>& req,
                                    const std::vector<TBlob>& outputs) {
  TShape new_lshape, new_rshape, new_oshape;
  bool need_bc = BinaryBroadcastShapeCompact(outputs[0].shape_, outputs[1].shape_, inputs[0].shape_,
                                             &new_lshape, &new_rshape, &new_oshape);
  if (!need_bc) {
    BinaryBackwardUseNone<xpu, LOP, ROP>(attrs, ctx, inputs, req, outputs);
  } else {
    MSHADOW_TYPE_SWITCH(outputs[0].type_flag_, DType, {
      if (new_oshape.ndim() == 2) {
        BinaryBroadcastBackwardUseNoneImpl<xpu, 2, DType, LOP, ROP>(
          ctx, inputs, req, outputs, new_lshape, new_rshape, new_oshape);
      } else {
        BinaryBroadcastBackwardUseNoneImpl<xpu, MXNET_SPECIAL_MAX_NDIM, DType, LOP, ROP>(
          ctx, inputs, req, outputs, new_lshape, new_rshape, new_oshape);
      }
    });
  }
}

template<typename xpu, int ndim, typename DType, typename LOP, typename ROP>
inline void BinaryBroadcastBackwardUseInImpl(const OpContext& ctx,
                                             const std::vector<TBlob>& inputs,
                                             const std::vector<OpReqType>& req,
                                             const std::vector<TBlob>& outputs,
                                             const TShape& new_lshape,
                                             const TShape& new_rshape,
                                             const TShape& new_oshape) {
  using namespace mshadow;
  using namespace mshadow::expr;
  Stream<xpu> *s = ctx.get_stream<xpu>();
  Tensor<xpu, ndim, DType> ograd =
    inputs[0].get_with_shape<xpu, ndim, DType>(new_oshape.get<ndim>(), s);
  Tensor<xpu, ndim, DType> lhs =
    inputs[1].get_with_shape<xpu, ndim, DType>(new_lshape.get<ndim>(), s);
  Tensor<xpu, ndim, DType> rhs =
    inputs[2].get_with_shape<xpu, ndim, DType>(new_rshape.get<ndim>(), s);
  Tensor<xpu, ndim, DType> lgrad =
    outputs[0].get_with_shape<xpu, ndim, DType>(new_lshape.get<ndim>(), s);
  Tensor<xpu, ndim, DType> rgrad =
    outputs[1].get_with_shape<xpu, ndim, DType>(new_rshape.get<ndim>(), s);
  ReduceToAssign<red::sum>(lgrad, req[0],
    ograd*F<LOP>(broadcast_to(lhs, new_oshape), broadcast_to(rhs, new_oshape)));
  ReduceToAssign<red::sum>(rgrad, req[1],
    ograd*F<ROP>(broadcast_to(lhs, new_oshape), broadcast_to(rhs, new_oshape)));
}

template<typename xpu, typename LOP, typename ROP>
void BinaryBroadcastBackwardUseIn(const nnvm::NodeAttrs& attrs,
                                  const OpContext& ctx,
                                  const std::vector<TBlob>& inputs,
                                  const std::vector<OpReqType>& req,
                                  const std::vector<TBlob>& outputs) {
  TShape new_lshape, new_rshape, new_oshape;
  bool need_bc = BinaryBroadcastShapeCompact(outputs[0].shape_, outputs[1].shape_, inputs[0].shape_,
                                             &new_lshape, &new_rshape, &new_oshape);
  if (!need_bc) {
    BinaryBackwardUseIn<xpu, LOP, ROP>(attrs, ctx, inputs, req, outputs);
  } else {
    MSHADOW_TYPE_SWITCH(outputs[0].type_flag_, DType, {
      if (new_oshape.ndim() == 2) {
        BinaryBroadcastBackwardUseInImpl<xpu, 2, DType, LOP, ROP>(
          ctx, inputs, req, outputs, new_lshape, new_rshape, new_oshape);
      } else {
        BinaryBroadcastBackwardUseInImpl<xpu, MXNET_SPECIAL_MAX_NDIM, DType, LOP, ROP>(
          ctx, inputs, req, outputs, new_lshape, new_rshape, new_oshape);
      }
    });
  }
}

template<typename xpu, int ndim, typename DType, typename LOP, typename ROP>
inline void BinaryBroadcastBackwardUseOutImpl(const OpContext& ctx,
                                              const std::vector<TBlob>& inputs,
                                              const std::vector<OpReqType>& req,
                                              const std::vector<TBlob>& outputs,
                                              const TShape& new_lshape,
                                              const TShape& new_rshape,
                                              const TShape& new_oshape) {
  using namespace mshadow;
  using namespace mshadow::expr;
  Stream<xpu> *s = ctx.get_stream<xpu>();
  Tensor<xpu, ndim, DType> ograd =
    inputs[0].get_with_shape<xpu, ndim, DType>(new_oshape.get<ndim>(), s);
  Tensor<xpu, ndim, DType> out =
    inputs[1].get_with_shape<xpu, ndim, DType>(new_oshape.get<ndim>(), s);
  Tensor<xpu, ndim, DType> lgrad =
    outputs[0].get_with_shape<xpu, ndim, DType>(new_lshape.get<ndim>(), s);
  Tensor<xpu, ndim, DType> rgrad =
    outputs[1].get_with_shape<xpu, ndim, DType>(new_rshape.get<ndim>(), s);
  ReduceToAssign<red::sum>(lgrad, req[0], ograd*F<LOP>(out));
  ReduceToAssign<red::sum>(rgrad, req[1], ograd*F<ROP>(out));
}

template<typename xpu, typename LOP, typename ROP>
void BinaryBroadcastBackwardUseOut(const nnvm::NodeAttrs& attrs,
                                   const OpContext& ctx,
                                   const std::vector<TBlob>& inputs,
                                   const std::vector<OpReqType>& req,
                                   const std::vector<TBlob>& outputs) {
  TShape new_lshape, new_rshape, new_oshape;
  bool need_bc = BinaryBroadcastShapeCompact(outputs[0].shape_, outputs[1].shape_, inputs[0].shape_,
                                             &new_lshape, &new_rshape, &new_oshape);
  if (!need_bc) {
    BinaryBackwardUseOut<xpu, LOP, ROP>(attrs, ctx, inputs, req, outputs);
  } else {
    MSHADOW_TYPE_SWITCH(outputs[0].type_flag_, DType, {
      if (new_oshape.ndim() == 2) {
        BinaryBroadcastBackwardUseOutImpl<xpu, 2, DType, LOP, ROP>(
          ctx, inputs, req, outputs, new_lshape, new_rshape, new_oshape);
      } else {
        BinaryBroadcastBackwardUseOutImpl<xpu, MXNET_SPECIAL_MAX_NDIM, DType, LOP, ROP>(
          ctx, inputs, req, outputs, new_lshape, new_rshape, new_oshape);
      }
    });
  }
}

#define MXNET_OPERATOR_REGISTER_BINARY_BROADCAST(name)                \
  NNVM_REGISTER_OP(name)                                              \
  .set_num_inputs(2)                                                  \
  .set_num_outputs(1)                                                 \
  .set_attr<nnvm::FListInputNames>("FListInputNames",                 \
    [](const NodeAttrs& attrs) {                                      \
      return std::vector<std::string>{"lhs", "rhs"};                  \
    })                                                                \
  .set_attr<nnvm::FInferShape>("FInferShape", BinaryBroadcastShape)   \
  .set_attr<nnvm::FInferType>("FInferType", ElemwiseType<2, 1>)       \
  .set_attr<nnvm::FInplaceOption>("FInplaceOption",                   \
    [](const NodeAttrs& attrs){                                       \
      return std::vector<std::pair<int, int> >{{0, 0}, {1, 0}};       \
    })                                                                \
  .add_argument("lhs", "NDArray", "first input")                      \
  .add_argument("rhs", "NDArray", "second input")

}  // namespace op
}  // namespace mxnet
#endif  // MXNET_OPERATOR_TENSOR_ELEMWISE_BINARY_BROADCAST_OP_H_
