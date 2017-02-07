/*!
 *  Copyright (c) 2015 by Contributors
 * \file elementwise_unary_op-inl.h
 * \brief Function defintion of elementwise unary operators
 */
#ifndef MXNET_OPERATOR_TENSOR_BROADCAST_REDUCE_OP_H_
#define MXNET_OPERATOR_TENSOR_BROADCAST_REDUCE_OP_H_

#include <mxnet/operator_util.h>
#include <vector>
#include <utility>
#include <algorithm>
#include "../mshadow_op.h"
#include "../elemwise_op_common.h"
#include "./elemwise_binary_broadcast_op.h"

namespace mxnet {
namespace op {
struct ReduceAxesParam : public dmlc::Parameter<ReduceAxesParam> {
  TShape axis;
  bool keepdims;
  DMLC_DECLARE_PARAMETER(ReduceAxesParam) {
    DMLC_DECLARE_FIELD(axis).set_default(TShape())
      .describe("Empty or unsigned or tuple. The axes to perform the reduction."
                "If left empty, a global reduction will be performed.");
    DMLC_DECLARE_FIELD(keepdims).set_default(false)
      .describe("If true, the axis which is reduced is left "
                "in the result as dimension with size one.");
  }
};

struct ReduceAxisParam : public dmlc::Parameter<ReduceAxisParam> {
  int axis;
  bool keepdims;
  DMLC_DECLARE_PARAMETER(ReduceAxisParam) {
    DMLC_DECLARE_FIELD(axis).set_default(-1)
      .describe("Empty or unsigned. The axis to perform the reduction."
                "If left empty, a global reduction will be performed.");
    DMLC_DECLARE_FIELD(keepdims).set_default(false)
      .describe("If true, the axis which is reduced is left "
                "in the result as dimension with size one.");
  }
};

struct BroadcastAxesParam : public dmlc::Parameter<BroadcastAxesParam> {
  TShape axis;
  TShape size;
  DMLC_DECLARE_PARAMETER(BroadcastAxesParam) {
    DMLC_DECLARE_FIELD(axis).set_default(TShape())
      .describe("The axes to perform the broadcasting.");
    DMLC_DECLARE_FIELD(size).set_default(TShape())
      .describe("Target sizes of the broadcasting axes.");
  }
};

struct BroadcastToParam : public dmlc::Parameter<BroadcastToParam> {
  TShape shape;
  DMLC_DECLARE_PARAMETER(BroadcastToParam) {
    DMLC_DECLARE_FIELD(shape).set_default(TShape())
      .describe("The shape of the desired array."
                " We can set the dim to zero if it's same as the original."
                " E.g `A = broadcast_to(B, shape=(10, 0, 0))` "
                "has the same meaning as `A = broadcast_axis(B, axis=0, size=10)`.");
  }
};

inline bool ReduceAxisShape(const nnvm::NodeAttrs& attrs,
                            std::vector<TShape> *in_attrs,
                            std::vector<TShape> *out_attrs) {
  CHECK_EQ(in_attrs->size(), 1);
  CHECK_EQ(out_attrs->size(), 1);
  TShape& ishape = (*in_attrs)[0];
  if (ishape.ndim() == 0) return false;
  const ReduceAxisParam& param = nnvm::get<ReduceAxisParam>(attrs.parsed);
  if (param.axis == -1 || ishape.ndim() == 1) {
    if (param.keepdims) {
      SHAPE_ASSIGN_CHECK(*out_attrs, 0, TShape(ishape.ndim()));
    } else {
      SHAPE_ASSIGN_CHECK(*out_attrs, 0, mshadow::Shape1(1));
    }
  } else {
    CHECK_LT(param.axis, static_cast<int>(ishape.ndim()))
        << "Reduction axis " << param.axis
        << " Exceeds input dimensions " << ishape;
    if (param.keepdims) {
      TShape oshape = ishape;
      oshape[param.axis] = 1;
      SHAPE_ASSIGN_CHECK(*out_attrs, 0, oshape);
    } else {
      TShape oshape(ishape.ndim() - 1);
      for (int i = 0; i < param.axis; ++i) oshape[i] = ishape[i];
      for (int i = param.axis+1; i < static_cast<int>(ishape.ndim()); ++i) {
        oshape[i-1] = ishape[i];
      }
      SHAPE_ASSIGN_CHECK(*out_attrs, 0, oshape);
    }
  }
  return true;
}

inline bool ReduceAxesShape(const nnvm::NodeAttrs& attrs,
                            std::vector<TShape> *in_attrs,
                            std::vector<TShape> *out_attrs) {
  CHECK_EQ(in_attrs->size(), 1);
  CHECK_EQ(out_attrs->size(), 1);
  if ((*in_attrs)[0].ndim() == 0) return false;
  const ReduceAxesParam& param = nnvm::get<ReduceAxesParam>(attrs.parsed);
  TShape &ishape = (*in_attrs)[0];
  TShape oshape;
  if (param.axis.ndim() == 0) {
    if (param.keepdims) {
      oshape = TShape(ishape.ndim());
    } else {
      oshape = TShape(1);
    }
  } else {
    if (param.keepdims) {
      oshape = ishape;
      for (index_t i = 0; i < param.axis.ndim(); ++i) {
        oshape[param.axis[i]] = 1;
      }
    } else {
      CHECK_LT(param.axis[param.axis.ndim()-1], ishape.ndim())
        << "Reduction axis " << param.axis[param.axis.ndim()-1]
        << " Exceeds input dimensions " << ishape;
      oshape = TShape(std::max<index_t>(1, ishape.ndim() - param.axis.ndim()));
      for (index_t i = 0, j = 0, k = 0; i < ishape.ndim(); ++i) {
        if (j < param.axis.ndim() && i == param.axis[j]) {
          ++j;
          continue;
        }
        oshape[k++] = ishape[i];
      }
    }
  }
  SHAPE_ASSIGN_CHECK(*out_attrs, 0, oshape);
  return true;
}

inline bool BroadcastAxesShape(const nnvm::NodeAttrs& attrs,
                               std::vector<TShape> *in_attrs,
                               std::vector<TShape> *out_attrs) {
  CHECK_EQ(in_attrs->size(), 1);
  CHECK_EQ(out_attrs->size(), 1);
  if ((*in_attrs)[0].ndim() == 0) return false;
  const BroadcastAxesParam& param = nnvm::get<BroadcastAxesParam>(attrs.parsed);
  CHECK_EQ(param.axis.ndim() , param.size.ndim());
  TShape &ishape = (*in_attrs)[0];
  TShape oshape = ishape;
  for (index_t i = 0; i < param.axis.ndim(); ++i) {
    CHECK_EQ(oshape[param.axis[i]], 1) << "Broadcasting axis must have size 1";
    oshape[param.axis[i]] = param.size[i];
  }
  SHAPE_ASSIGN_CHECK(*out_attrs, 0, oshape);
  return true;
}

inline bool BroadcastToShape(const nnvm::NodeAttrs& attrs,
                             std::vector<TShape> *in_attrs,
                            std::vector<TShape> *out_attrs) {
  CHECK_EQ(in_attrs->size(), 1);
  CHECK_EQ(out_attrs->size(), 1);
  TShape& ishape = (*in_attrs)[0];
  if (ishape.ndim() == 0) return false;
  const BroadcastToParam& param = nnvm::get<BroadcastToParam>(attrs.parsed);
  CHECK_EQ(ishape.ndim(), param.shape.ndim())
    << "Operand of shape " << ishape << " cannot be broadcasted to " << param.shape;
  for (index_t i = 0; i < ishape.ndim(); ++i) {
    CHECK(ishape[i] == param.shape[i] || ishape[i] == 1)
      << "Broadcasting axis must have size 1";
  }
  SHAPE_ASSIGN_CHECK(*out_attrs, 0, param.shape);
  return true;
}

inline void BroadcastReduceShapeCompact(const TShape& big, const TShape& small,
                                        TShape *new_big, TShape *new_small) {
  index_t idim = std::max<index_t>(big.ndim(), MXNET_SPECIAL_MAX_NDIM);
  *new_big = TShape(idim);
  *new_small = TShape(idim);
  index_t j = 0;
  if (small.Size() == 1) {
    (*new_big)[j++] = big.Size();
  } else {
    index_t bprod = 1, sprod = 1;
    for (index_t i = 0, k = 0; i < big.ndim(); ++i) {
      bool red_axis = big[i] != small[i];
      if ((red_axis && sprod > 1) || (!red_axis && bprod != sprod)) {
        (*new_big)[j] = bprod;
        (*new_small)[j] = sprod;
        bprod = sprod = 1; ++j;
      }
      bprod *= big[i];
      if (red_axis) {
        ++k;
      } else {
        sprod *= big[i];
      }
    }
    if (bprod > 1 || sprod > 1) {
      (*new_big)[j] = bprod;
      (*new_small)[j] = sprod;
      ++j;
    }
  }
  if (j <= 2) {
    new_small->assign(&(*new_small)[0], &(*new_small)[2]);
    new_big->assign(&(*new_big)[0], &(*new_big)[2]);
  } else if (j <= MXNET_SPECIAL_MAX_NDIM) {
    new_small->assign(&(*new_small)[0], &(*new_small)[MXNET_SPECIAL_MAX_NDIM]);
    new_big->assign(&(*new_big)[0], &(*new_big)[MXNET_SPECIAL_MAX_NDIM]);
  } else {
    LOG(FATAL) << "Too many reduction axes from " << big << " to " << small;
  }
}

template<typename xpu, typename reducer>
void SearchAxisCompute(const nnvm::NodeAttrs& attrs,
                       const OpContext& ctx,
                       const std::vector<TBlob>& inputs,
                       const std::vector<OpReqType>& req,
                       const std::vector<TBlob>& outputs) {
  using namespace mshadow;
  using namespace mshadow::expr;
  const ReduceAxisParam& param = nnvm::get<ReduceAxisParam>(attrs.parsed);
  Stream<xpu> *s = ctx.get_stream<xpu>();
  if (param.axis == -1) {
    LOG(FATAL) << "Global reduction not supported yet";
  } else {
    index_t leading = 1, trailing = 1;
    for (int i = 0; i < param.axis; ++i)
      leading *= inputs[0].shape_[i];
    for (int i = param.axis+1; i < inputs[0].ndim(); ++i)
      trailing *= inputs[0].shape_[i];
    MSHADOW_REAL_TYPE_SWITCH(outputs[0].type_flag_, DType, {
      Tensor<xpu, 2, DType> out = outputs[0].get_with_shape<xpu, 2, DType>(
        Shape2(leading, trailing), s);
      Tensor<xpu, 3, DType> in = inputs[0].get_with_shape<xpu, 3, DType>(
        Shape3(leading, inputs[0].shape_[param.axis], trailing), s);
      CHECK(req[0] != kAddTo) << "AddTo is not supported";
      ASSIGN_DISPATCH(out, req[0], (reduce_with_axis<reducer, true>(in, 1)));
    });
  }
}

template<typename xpu, typename reducer, bool normalize = false>
void ReduceAxesComputeImpl(const nnvm::NodeAttrs& attrs,
                           const OpContext& ctx,
                           const std::vector<TBlob>& inputs,
                           const std::vector<OpReqType>& req,
                           const std::vector<TBlob>& outputs,
                           const TShape& small) {
  using namespace mshadow;
  using namespace mshadow::expr;

  TShape src_shape, dst_shape;
  BroadcastReduceShapeCompact(inputs[0].shape_, small, &src_shape, &dst_shape);
  Stream<xpu> *s = ctx.get_stream<xpu>();
  MSHADOW_TYPE_SWITCH(outputs[0].type_flag_, DType, {
    if (dst_shape.ndim() == 2) {
      Tensor<xpu, 2, DType> out =
        outputs[0].get_with_shape<xpu, 2, DType>(dst_shape.get<2>(), s);
      Tensor<xpu, 2, DType> data =
        inputs[0].get_with_shape<xpu, 2, DType>(src_shape.get<2>(), s);
      ReduceToAssign<reducer>(out, req[0], data);
      if (normalize) out /= scalar<DType>(src_shape.Size()/dst_shape.Size());
    } else {
      const int ndim = MXNET_SPECIAL_MAX_NDIM;
      Tensor<xpu, ndim, DType> out =
        outputs[0].get_with_shape<xpu, ndim, DType>(dst_shape.get<ndim>(), s);
      Tensor<xpu, ndim, DType> data =
        inputs[0].get_with_shape<xpu, ndim, DType>(src_shape.get<ndim>(), s);
      ReduceToAssign<reducer>(out, req[0], data);
      if (normalize) out /= scalar<DType>(src_shape.Size()/dst_shape.Size());
    }
  });
}

template<typename xpu, typename reducer, bool normalize = false>
void ReduceAxesCompute(const nnvm::NodeAttrs& attrs,
                       const OpContext& ctx,
                       const std::vector<TBlob>& inputs,
                       const std::vector<OpReqType>& req,
                       const std::vector<TBlob>& outputs) {
  // using namespace mshadow;
  // using namespace mshadow::expr;
  const ReduceAxesParam& param = nnvm::get<ReduceAxesParam>(attrs.parsed);
  TShape small;
  if (!param.keepdims) {
    if (param.axis.ndim() == 0) {
      small = TShape(inputs[0].shape_.ndim());
    } else {
      small = inputs[0].shape_;
      for (index_t i = 0; i < param.axis.ndim(); ++i)
        small[param.axis[i]] = 1;
    }
  } else {
    small = outputs[0].shape_;
  }

  ReduceAxesComputeImpl<xpu, reducer, normalize>(attrs, ctx, inputs, req, outputs, small);
}

// works when shape inference of output is given
template<typename xpu, typename OP, bool normalize = false>
void ReduceAxesBackwardUseInOut(const nnvm::NodeAttrs& attrs,
                                const OpContext& ctx,
                                const std::vector<TBlob>& inputs,
                                const std::vector<OpReqType>& req,
                                const std::vector<TBlob>& outputs) {
  using namespace mshadow;
  using namespace mshadow::expr;
  const ReduceAxesParam& param = nnvm::get<ReduceAxesParam>(attrs.parsed);
  TShape small;
  if (param.axis.ndim() == 0) {
    small = TShape(outputs[0].shape_.ndim());
  } else {
    small = outputs[0].shape_;
    for (index_t i = 0; i < param.axis.ndim(); ++i)
      small[param.axis[i]] = 1;
  }

  TShape src_shape, dst_shape;
  BroadcastReduceShapeCompact(outputs[0].shape_, small, &src_shape, &dst_shape);
  Stream<xpu> *s = ctx.get_stream<xpu>();
  MSHADOW_TYPE_SWITCH(outputs[0].type_flag_, DType, {
    if (dst_shape.ndim() == 2) {
      Tensor<xpu, 2, DType> igrad =
        outputs[0].get_with_shape<xpu, 2, DType>(src_shape.get<2>(), s);
      Tensor<xpu, 2, DType> ograd =
        inputs[0].get_with_shape<xpu, 2, DType>(dst_shape.get<2>(), s);
      Tensor<xpu, 2, DType> data =
        inputs[1].get_with_shape<xpu, 2, DType>(src_shape.get<2>(), s);
      Tensor<xpu, 2, DType> out =
        inputs[2].get_with_shape<xpu, 2, DType>(dst_shape.get<2>(), s);
      ASSIGN_DISPATCH(igrad, req[0],
          broadcast_to(ograd, src_shape)*F<OP>(data, broadcast_to(out, src_shape)));
      if (normalize) igrad /= scalar<DType>(src_shape.Size()/dst_shape.Size());
    } else {
      const int ndim = MXNET_SPECIAL_MAX_NDIM;
      Tensor<xpu, ndim, DType> igrad =
        outputs[0].get_with_shape<xpu, ndim, DType>(src_shape.get<ndim>(), s);
      Tensor<xpu, ndim, DType> ograd =
        inputs[0].get_with_shape<xpu, ndim, DType>(dst_shape.get<ndim>(), s);
      Tensor<xpu, ndim, DType> data =
        inputs[1].get_with_shape<xpu, ndim, DType>(src_shape.get<ndim>(), s);
      Tensor<xpu, ndim, DType> out =
        inputs[2].get_with_shape<xpu, ndim, DType>(dst_shape.get<ndim>(), s);
      ASSIGN_DISPATCH(igrad, req[0],
          broadcast_to(ograd, src_shape)*F<OP>(data, broadcast_to(out, src_shape)));
      if (normalize) igrad /= scalar<DType>(src_shape.Size()/dst_shape.Size());
    }
  });
}

template<typename xpu>
inline void BroadcastComputeImpl(const nnvm::NodeAttrs& attrs,
                                 const OpContext& ctx,
                                 const std::vector<TBlob>& inputs,
                                 const std::vector<OpReqType>& req,
                                 const std::vector<TBlob>& outputs,
                                 const TShape& small) {
  using namespace mshadow;
  using namespace mshadow::expr;
  TShape src_shape, dst_shape;
  BroadcastReduceShapeCompact(outputs[0].shape_, small, &dst_shape, &src_shape);
  Stream<xpu> *s = ctx.get_stream<xpu>();
  MSHADOW_TYPE_SWITCH(outputs[0].type_flag_, DType, {
    if (dst_shape.ndim() == 2) {
      Tensor<xpu, 2, DType> out =
        outputs[0].get_with_shape<xpu, 2, DType>(dst_shape.get<2>(), s);
      Tensor<xpu, 2, DType> data =
        inputs[0].get_with_shape<xpu, 2, DType>(src_shape.get<2>(), s);
      ASSIGN_DISPATCH(out, req[0], broadcast_to(data, dst_shape));
    } else {
      const int ndim = MXNET_SPECIAL_MAX_NDIM;
      Tensor<xpu, ndim, DType> out =
        outputs[0].get_with_shape<xpu, ndim, DType>(dst_shape.get<ndim>(), s);
      Tensor<xpu, ndim, DType> data =
        inputs[0].get_with_shape<xpu, ndim, DType>(src_shape.get<ndim>(), s);
      ASSIGN_DISPATCH(out, req[0], broadcast_to(data, dst_shape));
    }
  });
}

template<typename xpu>
inline void BroadcastCompute(const nnvm::NodeAttrs& attrs,
                             const OpContext& ctx,
                             const std::vector<TBlob>& inputs,
                             const std::vector<OpReqType>& req,
                             const std::vector<TBlob>& outputs) {
  BroadcastComputeImpl<xpu>(attrs, ctx, inputs, req, outputs, inputs[0].shape_);
}

template<typename xpu, bool normalize = false>
inline void ReduceAxesBackwardUseNone(const nnvm::NodeAttrs& attrs,
                                      const OpContext& ctx,
                                      const std::vector<TBlob>& inputs,
                                      const std::vector<OpReqType>& req,
                                      const std::vector<TBlob>& outputs) {
  using namespace mshadow;
  using namespace mshadow::expr;
  const ReduceAxesParam& param = nnvm::get<ReduceAxesParam>(attrs.parsed);
  TShape small;
  if (param.axis.ndim() == 0) {
    small = TShape(outputs[0].shape_.ndim());
  } else {
    small = outputs[0].shape_;
    for (index_t i = 0; i < param.axis.ndim(); ++i)
      small[param.axis[i]] = 1;
  }
  BroadcastComputeImpl<xpu>(attrs, ctx, inputs, req, outputs, small);
  if (normalize)  {
    Stream<xpu> *s = ctx.get_stream<xpu>();
    MSHADOW_TYPE_SWITCH(outputs[0].type_flag_, DType, {
      Tensor<xpu, 1, DType> igrad = outputs[0].FlatTo1D<xpu, DType>(s);
      igrad /= scalar<DType>(outputs[0].Size()/inputs[0].Size());
    });
  }
}

template<typename PType>
inline void AxesParamParser(nnvm::NodeAttrs* attrs) {
  PType param;
  param.Init(attrs->dict);
  std::sort(&param.axis[0], &param.axis[param.axis.ndim()]);
  attrs->parsed = std::move(param);
}

struct ReduceGrad {
  const char *op_name;
  std::vector<nnvm::NodeEntry> operator()(const nnvm::NodePtr& n,
                                          const std::vector<nnvm::NodeEntry>& ograds) {
    return MakeGradNode(
        op_name, n,
        {ograds[0], n->inputs[0], nnvm::NodeEntry{n, 0, 0}},
        n->attrs.dict);
  }
};

template<typename xpu>
void L2NormCompute(const nnvm::NodeAttrs& attrs,
                   const OpContext& ctx,
                   const std::vector<TBlob>& inputs,
                   const std::vector<OpReqType>& req,
                   const std::vector<TBlob>& outputs) {
  mshadow::Stream<xpu> *s = ctx.get_stream<xpu>();
  MSHADOW_REAL_TYPE_SWITCH(outputs[0].type_flag_, DType, {
    mshadow::Tensor<xpu, 1, DType> out = outputs[0].get<xpu, 1, DType>(s);
    mshadow::Tensor<xpu, 1, DType> in = inputs[0].get_with_shape<xpu, 1, DType>(
      mshadow::Shape1(inputs[0].shape_.Size()), s);
    mshadow::VectorDot(out, in, in);
    ASSIGN_DISPATCH(out, req[0], mshadow::expr::F<mxnet::op::mshadow_op::square_root>(out));
  });
}

#define MXNET_OPERATOR_REGISTER_REDUCE_AXIS(name)               \
  NNVM_REGISTER_OP(name)                                        \
  .set_num_inputs(1)                                            \
  .set_num_outputs(1)                                           \
  .set_attr_parser(ParamParser<ReduceAxisParam>)                \
  .set_attr<nnvm::FInferShape>("FInferShape", ReduceAxisShape)  \
  .set_attr<nnvm::FInferType>("FInferType", ElemwiseType<1, 1>) \
  .add_argument("data", "NDArray", "Source input")               \
  .add_arguments(ReduceAxisParam::__FIELDS__())

#define MXNET_OPERATOR_REGISTER_REDUCE(name)                    \
  NNVM_REGISTER_OP(name)                                        \
  .set_num_inputs(1)                                            \
  .set_num_outputs(1)                                           \
  .set_attr_parser(AxesParamParser<ReduceAxesParam>)            \
  .set_attr<nnvm::FInferShape>("FInferShape", ReduceAxesShape)  \
  .set_attr<nnvm::FInferType>("FInferType", ElemwiseType<1, 1>) \
  .add_argument("data", "NDArray", "Source input")               \
  .add_arguments(ReduceAxesParam::__FIELDS__())

#define MXNET_OPERATOR_REGISTER_REDUCE_BACKWARD(name)               \
  NNVM_REGISTER_OP(name)                                            \
  .set_num_outputs(1)                                               \
  .set_attr_parser(AxesParamParser<ReduceAxesParam>)                \
  .set_attr<nnvm::TIsBackward>("TIsBackward", true)

#define MXNET_OPERATOR_REGISTER_BROADCAST(name)                 \
  NNVM_REGISTER_OP(name)                                        \
  .set_num_inputs(1)                                            \
  .set_num_outputs(1)                                           \
  .set_attr<nnvm::FInferType>("FInferType", ElemwiseType<1, 1>) \
  .set_attr<nnvm::FGradient>("FGradient",                       \
    [](const nnvm::NodePtr& n,                                  \
       const std::vector<nnvm::NodeEntry>& ograds) {            \
      return MakeGradNode("_broadcast_backward", n, ograds,     \
                          {{"keepdims", "true"}});              \
    })                                                          \
  .add_argument("data", "NDArray", "Source input")

}  // namespace op
}  // namespace mxnet
#endif  // MXNET_OPERATOR_TENSOR_BROADCAST_REDUCE_OP_H_
