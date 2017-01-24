/*!
 *  Copyright (c) 2016 by Contributors
 * \file c_api_symbolic.cc
 * \brief C API of mxnet
 */

#include <mxnet/base.h>
#include <mxnet/c_api.h>
#include <mxnet/operator.h>
#include <mxnet/operator_util.h>
#include <mxnet/op_attr_types.h>
#include <nnvm/node.h>
#include <nnvm/op_attr_types.h>
#include "./c_api_common.h"
#include "../common/utils.h"

using namespace mxnet;

int MXImperativeInvoke(AtomicSymbolCreator creator,
                       int num_inputs,
                       NDArrayHandle *inputs,
                       int *num_outputs,
                       NDArrayHandle **outputs,
                       int num_params,
                       const char **param_keys,
                       const char **param_vals) {
  static auto& num_args = nnvm::Op::GetAttr<std::string>("key_var_num_args");
  static auto& infershape = nnvm::Op::GetAttr<nnvm::FInferShape>("FInferShape");
  static auto& infertype = nnvm::Op::GetAttr<nnvm::FInferType>("FInferType");
  static auto& visible_out = nnvm::Op::GetAttr<nnvm::FNumVisibleOutputs>("FNumVisibleOutputs");
  static auto& fcpu = nnvm::Op::GetAttr<FCompute>("FCompute<cpu>");
  static auto& fgpu = nnvm::Op::GetAttr<FCompute>("FCompute<gpu>");
  static auto& ndfunc = nnvm::Op::GetAttr<FNDArrayFunction>("FNDArrayFunction");
  static auto& createop = nnvm::Op::GetAttr<FCreateLayerOp>("FCreateLayerOp");
  static auto& mutate = nnvm::Op::GetAttr<nnvm::FMutateInputs>("FMutateInputs");
  static auto& tmp_resource = nnvm::Op::GetAttr<FResourceRequest>("FResourceRequest");
  const nnvm::Op* op = static_cast<nnvm::Op*>(creator);
  NDArray** outarray = *reinterpret_cast<NDArray***>(outputs);
  MXAPIThreadLocalEntry *ret = MXAPIThreadLocalStore::Get();

  API_BEGIN();
  nnvm::NodeAttrs attrs;
  attrs.op = op;
  for (int i = 0; i < num_params; ++i) {
    attrs.dict.emplace(param_keys[i], param_vals[i]);
  }

  if (num_args.count(op)) {
    attrs.dict.emplace(num_args[op], std::to_string(num_inputs));
  }
  if (op->attr_parser != nullptr) {
    op->attr_parser(&attrs);
  }
  int infered_num_inputs;
  if (op->get_num_inputs != nullptr) {
    infered_num_inputs = op->get_num_inputs(attrs);
  } else {
    infered_num_inputs = op->num_inputs;
  }
  CHECK_EQ(num_inputs, infered_num_inputs)
    << "Expecting " << infered_num_inputs << " inputs, got "
    << num_inputs << " in operator " << op->name;
  int infered_num_outputs;
  if (op->get_num_outputs != nullptr) {
    infered_num_outputs = op->get_num_outputs(attrs);
  } else {
    infered_num_outputs = op->num_outputs;
  }
  int num_visible_outputs = infered_num_outputs;
  if (visible_out.count(op)) {
    num_visible_outputs = visible_out[op](attrs);
    CHECK_LE(num_visible_outputs, infered_num_outputs);
  }

  std::vector<NDArray> ndinputs, ndoutputs;
  ndinputs.reserve(num_inputs);
  for (int i = 0; i < num_inputs; ++i) {
    ndinputs.emplace_back(*reinterpret_cast<NDArray*>(inputs[i]));
  }
  if (outarray == nullptr) {
    *num_outputs = num_visible_outputs;
    ndoutputs.resize(infered_num_outputs);
  } else {
    CHECK(*num_outputs == infered_num_outputs || *num_outputs == num_visible_outputs)
      << "Expecting " << infered_num_outputs << " (all) or "
      << num_visible_outputs << " (visible only) outputs, got "
      << *num_outputs << " in operator " << op->name;
    ndoutputs.reserve(infered_num_outputs);
    for (int i = 0; i < num_visible_outputs; ++i) {
      ndoutputs.emplace_back(std::move(*outarray[i]));
    }
    ndoutputs.resize(infered_num_outputs);
  }

  if (ndfunc.count(op)) {
    ndfunc[op](attrs, ndinputs, &ndoutputs);
  } else {
    // TODO(piiswrong): infer ctx
    Context ctx;
    if (num_inputs) {
      ctx = ndinputs[0].ctx();
    } else if (infered_num_outputs && !ndoutputs[0].is_none()) {
      ctx = ndoutputs[0].ctx();
    } else if (attrs.dict.find("ctx") != attrs.dict.end()) {
      ctx = Context::FromString(attrs.dict["ctx"]);
    } else {
      ctx = Context::CPU();
    }
    // Pinned context doesn't propagate
    if (ctx.dev_type == Context::kCPUPinned) {
      ctx = Context::CPU();
    }

    std::vector<TShape>& in_shapes = ret->arg_shapes;
    std::vector<TShape>& out_shapes = ret->out_shapes;
    in_shapes.clear();
    out_shapes.clear();

    for (auto& i : ndinputs) {
      in_shapes.emplace_back(i.shape());
    }
    for (auto& i : ndoutputs) {
      out_shapes.emplace_back(i.shape());
    }
    CHECK(infershape.count(op))
      << "Operator " << op->name << " is missing FInferShape attribute";
    CHECK(infershape[op](attrs, &in_shapes, &out_shapes));
    CHECK_EQ(out_shapes.size(), static_cast<size_t>(infered_num_outputs));

    std::vector<int>& in_types = ret->arg_types;
    std::vector<int>& out_types = ret->out_types;
    in_types.clear();
    out_types.clear();

    for (auto& i : ndinputs) {
      in_types.push_back(i.dtype());
    }
    for (auto& i : ndoutputs) {
      out_types.push_back(i.dtype());
    }
    CHECK(infertype.count(op))
      << "Operator " << op->name << " is missing FInferType attribute";
    CHECK(infertype[op](attrs, &in_types, &out_types));
    CHECK_EQ(out_types.size(), static_cast<size_t>(infered_num_outputs));

    for (int i = 0; i < infered_num_outputs; ++i) {
      if (ndoutputs[i].is_none()) {
        ndoutputs[i] = NDArray(out_shapes[i], ctx, true, out_types[i]);
      } else {
        CHECK_EQ(ndoutputs[i].shape(), out_shapes[i])
          << i << "th output has invalid shape. "
          << "Expecting " << out_shapes[i] << " got "
          << ndoutputs[i].shape() << " in operator " << op->name;
        CHECK_EQ(ndoutputs[i].dtype(), out_types[i])
          << i << "th output has invalid shape. "
          << "Expecting " << out_types[i] << " got "
          << ndoutputs[i].dtype()  << " in operator " << op->name;
      }
    }

    std::vector<engine::VarHandle> read_vars, write_vars;
    // request resources
    std::vector<Resource> requested;
    if (tmp_resource.count(op)) {
      int ntmp = 0;
      for (const auto& req : tmp_resource[op](attrs)) {
        switch (req.type) {
         case ResourceRequest::kTempSpace:
          ++ntmp;
         case ResourceRequest::kRandom:
          requested.push_back(ResourceManager::Get()->Request(ctx, req));
          write_vars.push_back(requested.back().var);
          break;
         default:
          LOG(FATAL) << "resource type not yet supported";
        }
      }
      CHECK_LE(ntmp, 1) << "Only support 1 temp space request";
    }

    std::vector<uint32_t> auxidx;
    for (auto& i : ndinputs) {
      read_vars.push_back(i.var());
    }
    for (auto& i : ndoutputs) {
      write_vars.push_back(i.var());
    }
    if (mutate.count(op)) {
      auxidx = mutate[op](attrs);
      std::sort(auxidx.begin(), auxidx.end());
      for (auto & i : auxidx) {
        write_vars.push_back(ndinputs[i].var());
      }
    }
    common::DeduplicateVarHandle(&read_vars, &write_vars);

    FCompute fn;
    if (ctx.dev_mask() == cpu::kDevMask && fcpu.count(op)) {
      fn = fcpu[op];
    } else if (ctx.dev_mask() == gpu::kDevMask && fgpu.count(op)) {
      fn = fgpu[op];
    }
    if (fn) {
      Engine::Get()->PushAsync(
        [ctx, attrs, fn, ndinputs, ndoutputs, requested](
            RunContext rctx,
            engine::CallbackOnComplete on_complete) {
          std::vector<TBlob> input_blobs, output_blobs;
          for (auto& i : ndinputs) {
            input_blobs.push_back(i.data());
          }
          for (auto& i : ndoutputs) {
            i.CheckAndAlloc();
            output_blobs.push_back(i.data());
          }
          OpContext opctx{false, rctx,
                          engine::CallbackOnComplete(),
                          requested};
          std::vector<OpReqType> req(output_blobs.size(), kWriteTo);
          fn(attrs, opctx, input_blobs, req, output_blobs);
          if (ctx.dev_mask() == gpu::kDevMask) {
            rctx.get_stream<gpu>()->Wait();
          }
          on_complete();
        }, ctx, read_vars, write_vars, FnProperty::kNormal,
        0, PROFILER_MESSAGE(op->name.c_str()));
    } else if (createop.count(op)) {
      Operator* opr = createop[op](attrs, ctx, in_shapes, in_types);
      struct Capture {
        engine::CallbackOnComplete on_complete;
        Operator *opr;
      };
      Engine::Get()->PushAsync(
        [ctx, opr, auxidx, ndinputs, ndoutputs, requested](
            RunContext rctx,
            engine::CallbackOnComplete on_complete) {
          std::vector<TBlob> input_blobs, aux_blobs, output_blobs;
          auto atop = auxidx.begin();
          for (size_t i = 0; i < ndinputs.size(); ++i) {
            if (atop != auxidx.end() && i == *atop) {
              aux_blobs.push_back(ndinputs[i].data());
              ++atop;
            } else {
              input_blobs.push_back(ndinputs[i].data());
            }
          }
          for (auto& i : ndoutputs) {
            i.CheckAndAlloc();
            output_blobs.push_back(i.data());
          }
          Capture* capture = new Capture({on_complete, opr});
          OpContext opctx{false, rctx,
                          Engine::Get()->CreateCallback(
                            [](Engine* engine, void *cpt_handle) {
                                Capture* cpt = static_cast<Capture*>(cpt_handle);
                                cpt->on_complete();
                                delete cpt->opr;
                                delete cpt;
                              }, static_cast<void*>(capture)),
                          requested};
          std::vector<OpReqType> req(output_blobs.size(), kWriteTo);
          opr->Forward(opctx, input_blobs, req, output_blobs, aux_blobs);
          if (opr->exec_type() != Operator::kAsync) {
            if (ctx.dev_mask() == gpu::kDevMask) {
              rctx.get_stream<gpu>()->Wait();
            }
            delete opr;
            delete capture;
            on_complete();
          }
        }, ctx, read_vars, write_vars, FnProperty::kNormal,
        0, PROFILER_MESSAGE(op->name.c_str()));
    } else {
      LOG(FATAL)
        << "Operator " << op->name
        << " cannot be run; requires at least one of"
        << " FCompute<xpu>, NDArrayFunction, FCreateOperator be registered";
    }
  }

  if (outarray == nullptr) {
    ret->ret_handles.clear();
    for (int i = 0; i < num_visible_outputs; ++i) {
      ret->ret_handles.push_back(
        reinterpret_cast<NDArrayHandle>(new NDArray(std::move(ndoutputs[i]))));
    }
    *outputs = dmlc::BeginPtr(ret->ret_handles);
  } else {
    for (int i = 0; i < *num_outputs; ++i) {
      *outarray[i] = std::move(ndoutputs[i]);
    }
  }
  API_END();
}
