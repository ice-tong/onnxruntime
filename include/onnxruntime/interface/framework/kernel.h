// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "interface/common/data_types.h"
#include "interface/framework/tensor.h"

namespace onnxruntime {

namespace interface {

struct IKernelContext {
  virtual ~IKernelContext() = default;
  virtual const void* InputData(int index) const = 0;
  virtual const int64_t* InputShape(int index, size_t* num_dims) const = 0;
  virtual void* AllocateOutput(int index, const TensorShape& shape) = 0;
};

struct IArg {
  virtual ~IArg() = default;
};

using ArgPtr = std::unique_ptr<IArg>;
using ArgPtrs = std::vector<ArgPtr>;
using TensorShape = std::vector<int64_t>;

template <typename T>
struct ITensor : public IArg {
  //using MyType = ITensor<T>;
  ITensor(IKernelContext* ctx, int index) : ctx_(ctx), index_(index){};
  const TensorShape& Shape() { return shape_; }

 protected:
  IKernelContext* ctx_ = {};
  int index_ = {};
  TensorShape shape_;
};

template <typename T>
struct TensorView : public ITensor<T> {
  TensorView(IKernelContext* ctx, int index) : ITensor<T>(ctx, index) {
    // assert ctx
    data_ = ctx->InputData(index);
    size_t num_dims = 0;
    const auto* dims = ctx->InputShape(index, num_dims);
    // assert dims
    shape_ = TensorShape{dims, dims + num_dims};
  }
  TensorView(const T* data, const TensorShape& shape) : data_(data), shape_(shape){};
  const void* Data() const {
    return data_;
  }

 protected:
  const T* data_ = {};
};

template <typename T>
struct Tensor : public ITensor<T> {
  Tensor(IKernelContext* ctx, size_t index) : ITensor<T>(ctx, index) {}
  Tensor(T* data, const TensorShape& shape) : data_(data), shape_(shape){};
  void* Allocate(const TensorShape shape) {
    if (data_) {
      return data_;
    } else {
      // assert ctx
      shape_ = shape;
      data_ = ctx->AllocateOutput(index_, shape_);
      return data_;
    }
  }

 protected:
  T* data_ = {};
};

struct IKernelInfo {
  virtual ~IKernelInfo() = default;
};

struct IKernel {
  //explicit IKernel(const IKernelInfo&){};
  //IKernel(const IKernel&) = delete;
  virtual ~IKernel() = default;
  virtual void Init(IKernelInfo&) {}; //todo - as constructor
  virtual onnxruntime::Status Compute(IKernelContext*) const = 0;

  template <size_t ith_input, size_t ith_output, typename... Ts>
  static typename std::enable_if<sizeof...(Ts) == 0, std::tuple<>>::type
  CreateTuple(IKernelContext*, ArgPtrs&) {
    return std::make_tuple();
  }

  // inputs
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  static typename std::enable_if<std::is_same<T, TensorView<float>&>::value, std::tuple<T, Ts...>>::type
  CreateTuple(IKernelContext* context, ArgPtrs& args) {
    args.push_back(std::make_unique<TensorView<float>>(context, ith_input));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*args.back().get())};
    auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, args);
    return std::tuple_cat(current, next);
  }

  // outputs
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  static typename std::enable_if<std::is_same<T, Tensor<float>&>::value, std::tuple<T, Ts...>>::type
  CreateTuple(IKernelContext* context, ArgPtrs& args) {
    args.push_back(std::make_unique<Tensor<float>>(context, ith_output));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*args.back().get())};
    auto next = CreateTuple<ith_input, ith_output + 1, Ts...>(context, args);
    return std::tuple_cat(current, next);
  }
};

template <typename... Args>
struct FnKernel : public IKernel {
  using ComputeFn = onnxruntime::Status (*)(Args...);
  FnKernel(ComputeFn compute_fn) : compute_fn_(compute_fn) {}

  onnxruntime::Status Compute(IKernelContext* context) const override {
    ArgPtrs args;
    auto t = CreateTuple<0, 0, Args...>(context, args);
    return std::apply([this](Args const&... t_args) { return compute_fn_(t_args...); }, t);
  }

 private:
  ComputeFn compute_fn_;
};

template <typename K>
struct StructKernel : public IKernel {
  template <typename... Args>
  using ComputeFn = onnxruntime::Status (K::*)(Args...);
  void Init(IKernelInfo& info) override {
    kernel_ = std::make_unique<K>(info);
  }
  onnxruntime::Status Compute(IKernelContext* context) const override {
    return InvokeCompute(&K::Compute, context);
  }

  template <typename... Args>
  onnxruntime::Status InvokeCompute(ComputeFn<Args...>, IKernelContext* context) const {
    ArgPtrs args;
    auto t = CreateTuple<0, 0, Args...>(context, args);
    return std::apply([this](Args const&... t_args) { return kernel_->Compute(t_args...); }, t);
  }
  std::unique_ptr<K> kernel_;
};

struct IKernelBuilder {
  // IKernelBuilder() = default;
  // IKernelBuilder(const IKernelBuilder&) = delete;
  explicit IKernelBuilder() = default;
  IKernelBuilder(const IKernelBuilder&) = delete;

  virtual ~IKernelBuilder() = default;
  virtual IKernelBuilder& Provider(const char*) = 0;
  virtual IKernelBuilder& SetDomain(const char*) = 0;
  virtual IKernelBuilder& SetName(const char*) = 0;
  virtual IKernelBuilder& SinceVersion(int, int) = 0;
  virtual IKernelBuilder& Alias(int, int) = 0;
  virtual IKernelBuilder& TypeConstraint(const char*, TensorDataType) = 0;

  template <size_t, size_t, typename... Ts>
  typename std::enable_if<sizeof...(Ts) >= 0, IKernelBuilder&>::type
  ParseArgs() {
    //todo - generate constraints by args...
    return *this;
  }

  template <typename... Args>
  IKernelBuilder& ParseFn(onnxruntime::Status (*compute_fn)(Args...)) {
    using KernelType = FnKernel<Args...>;
    kernel_ = std::make_unique<KernelType>(compute_fn);
    return ParseArgs<0, 0, Args...>();
  }

  template <typename K, typename... Args>
  IKernelBuilder& ParseStruct(onnxruntime::Status (K::*compute_fn)(Args...)) {
    using KernelType = StructKernel<K>;
    kernel_ = std::make_unique<KernelType>();
    return ParseArgs<0, 0, Args...>();
  }

  std::unique_ptr<IKernel> kernel_;
};

struct IKernelRegistry {
  virtual ~IKernelRegistry() = default;
  virtual IKernelBuilder& CreateBuilder() = 0;
  template <typename... Args>
  IKernelBuilder& RegisterKernel(const char* ep,
                                 const char* domain,
                                 const char* op,
                                 int since_ver,
                                 int end_ver,
                                 onnxruntime::Status (*compute_fn)(Args...)) {
    return CreateBuilder().Provider(ep).SetDomain(domain).SetName(op).SinceVersion(since_ver, end_ver).ParseFn<Args...>(compute_fn);
  }
  template <typename K, typename... Args>
  IKernelBuilder& RegisterKernel(const char* ep,
                                 const char* domain,
                                 const char* op,
                                 int since_ver,
                                 int end_ver) {
    return CreateBuilder().Provider(ep).SetDomain(domain).SetName(op).SinceVersion(since_ver, end_ver).ParseStruct<K, Args...>(&K::Compute);
  }
};

}  // namespace interface
}  // namespace onnxruntime