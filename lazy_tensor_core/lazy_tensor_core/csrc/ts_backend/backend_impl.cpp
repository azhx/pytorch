#include <torch/csrc/lazy/backend/backend_device.h>
#include "lazy_tensor_core/csrc/ts_backend/backend_impl.h"

#include "lazy_tensor_core/csrc/ts_backend/ts_lowering_context.h"
#include "lazy_tensor_core/csrc/ts_backend/ts_shape_inference.h"
#include "lazy_tensors/computation_client/sys_util.h"

namespace torch_lazy_tensors {
namespace compiler {
using BackendDeviceType = torch::lazy::BackendDeviceType;

struct TSBackendDeviceType : public BackendDeviceType {
  TSBackendDeviceType() = delete;
  TSBackendDeviceType(c10::DeviceType deviceType) {
    TORCH_CHECK(supported_device_types_.find((int8_t)deviceType) !=
                supported_device_types_.end());
    type = (int8_t)deviceType;
  }

  std::string toString() const override {
    return c10::DeviceTypeName((c10::DeviceType)type);
  }

  c10::DeviceType c10Type() const { return (c10::DeviceType)type; }

 private:
  static const std::set<int8_t> supported_device_types_;
};
const std::set<int8_t> TSBackendDeviceType::supported_device_types_ = {
    (int8_t)at::kCPU, (int8_t)at::kCUDA};

class TSBackendImpl : public BackendImplInterface {
 public:
  TSBackendImpl() : default_device_type_(at::kCPU) {
    auto type = lazy_tensors::sys_util::GetEnvBool("LTC_TS_CUDA", false)
                    ? at::kCUDA
                    : at::kCPU;
    default_device_type_ = TSBackendDeviceType(type);
  }
  std::unique_ptr<ir::LoweringContext> CreateLoweringContext(
      const std::string& name, torch::lazy::BackendDevice device,
      c10::ArrayRef<torch::lazy::Node*> post_order,
      torch::lazy::Util::EmissionMap emit_status) const override {
    return std::make_unique<ts_backend::TSLoweringContext>(
        name, device, post_order, emit_status);
  }

  std::unique_ptr<ir::LoweringContext> CreateLoweringContext(
      const std::string& name, torch::lazy::BackendDevice device) const override {
    return std::make_unique<ts_backend::TSLoweringContext>(name, device);
  }

  std::vector<std::string> GetCompilationDevices(
      const std::string& device,
      c10::ArrayRef<std::string> devices) const override {
    return std::vector<std::string>(devices.begin(), devices.end());
  }

  at::Tensor MakeTensorFromComputationData(
      const torch::lazy::BackendDataPtr data,
      c10::optional<at::ScalarType> logical_scalar_type) const override {
    const auto ts_data = std::static_pointer_cast<TSData>(data);
    return ts_data->data();
  }

  torch::lazy::BackendDataPtr MakeComputationDataFromTensor(
      const at::Tensor& tensor, const torch::lazy::Shape& shape,
      const torch::lazy::BackendDevice& device) const override {
    at::TensorOptions options = tensor.options().device(default_device_type_.c10Type());
    return std::make_shared<TSData>(tensor.to(options), shape, device);
  }

  std::string GetComputationBackendText(
      const ComputationPtr computation) const override {
    auto ts_computation =
        static_cast<torch_lazy_tensors::compiler::ts_backend::TSComputation*>(
            computation.get());
    return ts_computation->graph()->toString();
  }

  //////////////computation client interfaces///////////////////////

 public:
  class TSData : public torch::lazy::BackendData {
   public:
    TSData(const at::Tensor& data, const torch::lazy::Shape& shape,
           const torch::lazy::BackendDevice& device)
        : torch::lazy::BackendData(device, shape), data_(data) {}

    TSData(const torch::lazy::Shape& shape,
           const torch::lazy::BackendDevice& device)
        : torch::lazy::BackendData(device, shape) {}

    Handle GetHandle() override { return reinterpret_cast<int64_t>(this); }

    void Assign(const torch::lazy::BackendData& data) override {
      data_ = static_cast<const TSData&>(data).data_;
    }

    bool HasValue() const override { return data_.defined(); }

    at::Tensor data() { return data_; }

   private:
    at::Tensor data_;
  };

  torch::lazy::BackendDataPtr CreateDataPlaceholder(
      const torch::lazy::BackendDevice& device,
      const torch::lazy::Shape& shape) const override;

  std::vector<ComputationPtr> Compile(
      std::vector<ComputationPtr> instances) const override;

  std::vector<torch::lazy::BackendDataPtr> ExecuteComputation(
      Computation& computation,
      c10::ArrayRef<torch::lazy::BackendDataPtr> arguments,
      const torch::lazy::BackendDevice& device) const override;

  std::shared_ptr<torch::lazy::BackendDeviceType> GetDefaultDeviceType()
      const override {
    return std::make_shared<BackendDeviceType>(default_device_type_);
  }

  at::DeviceType EagerFallbackDeviceType() const override;

  void SetDefaultDeviceType(std::string type) override {
    default_device_type_ = TSBackendDeviceType(c10::Device(type).type());
    // The first CUDA usage could happen via lazy tensors. Initialize CUDA here
    // to account for that, at::scalar_tensor constructor triggers everything we
    // need.
    static auto init_cuda = default_device_type_.c10Type() == at::kCUDA
                                ? c10::optional<at::Tensor>(at::scalar_tensor(
                                      0, at::TensorOptions().device(at::kCUDA)))
                                : c10::nullopt;
  }

  std::vector<torch::lazy::BackendDevice> GetBackendDevices() const override;

  torch::lazy::BackendDevice GetBackendDevice(
      c10::Device device) const override;

  void SetRngSeed(size_t seed) const override {
    LOG(FATAL) << "Not implemented yet.";
  }

  // std::map<std::string, Metric> GetMetrics() const override { return {}; }

  // MemoryInfo GetMemoryInfo(const std::string& device) override {
  //   LOG(FATAL) << "Not implemented yet.";
  // }

  void PrepareToExit() const override;

 private:
  TSBackendDeviceType default_device_type_;
};

torch::lazy::BackendDataPtr TSBackendImpl::CreateDataPlaceholder(
    const torch::lazy::BackendDevice& device,
    const torch::lazy::Shape& shape) const {
  return std::make_shared<TSBackendImpl::TSData>(shape, device);
}

std::vector<ComputationPtr> TSBackendImpl::Compile(
    std::vector<ComputationPtr> instances) const {
  for (const auto& instance : instances) {
    auto ts_computation =
        static_cast<ts_backend::TSComputation*>(instance.get());
  }
  return instances;
}

std::vector<torch::lazy::BackendDataPtr> TSBackendImpl::ExecuteComputation(
    Computation& computation,
    c10::ArrayRef<torch::lazy::BackendDataPtr> arguments,
    const torch::lazy::BackendDevice& device) const {
  torch::jit::GraphExecutor& graph_executor =
      static_cast<compiler::ts_backend::TSComputation&>(computation)
          .graph_executor();
  std::vector<torch::jit::IValue> stack;
  for (auto argument : arguments) {
    const auto ts_data =
        std::static_pointer_cast<TSBackendImpl::TSData>(argument);
    CHECK((c10::DeviceType)default_device_type_.type != at::kCUDA ||
          ts_data->data().device().type() == at::kCUDA);
    stack.emplace_back(ts_data->data());
  }
  graph_executor.run(stack);
  std::vector<torch::lazy::BackendDataPtr> results;
  for (torch::jit::IValue component : stack) {
    at::Tensor result = component.toTensor();
    at::IntArrayRef result_sizes = result.sizes();
    torch::lazy::Shape shape(
        result.scalar_type(),
        std::vector<int64_t>(result_sizes.begin(), result_sizes.end()));
    results.push_back(
        std::make_shared<TSBackendImpl::TSData>(result, shape, device));
  }
  return results;
}

std::vector<torch::lazy::BackendDevice> TSBackendImpl::GetBackendDevices()
    const {
  std::vector<torch::lazy::BackendDevice> devices;
  // TODO(whc) figure out how to query available devices from pytorch
  devices.emplace_back(GetBackendDevice(c10::Device(c10::kCPU, 0)));
  devices.emplace_back(GetBackendDevice(c10::Device(c10::kCUDA, 0)));
  return devices;
}

torch::lazy::BackendDevice TSBackendImpl::GetBackendDevice(
    c10::Device device) const {
  // Note, we ignore the device type specified by the c10::Device since it is
  // expected to be a virtual device (lazy::), but we need to change this when
  // we support lazy as a mode
  return torch::lazy::BackendDevice(GetDefaultDeviceType(), device.index());
}

void TSBackendImpl::PrepareToExit() const {}

c10::DeviceType TSBackendImpl::EagerFallbackDeviceType() const {
  // For TS backend, hardware device _is_ eager device
  return (c10::DeviceType)GetDefaultDeviceType()->type;
}

compiler::BackendImplInterface* GetTSBackendImpl() {
  static compiler::TSBackendImpl* ts_backend_impl =
      new compiler::TSBackendImpl();
  return ts_backend_impl;
}

void InitTorchScriptBackend() {
  static std::unique_ptr<compiler::BackendRegistrar> s_registrar;
  s_registrar.reset(
      new compiler::BackendRegistrar(compiler::GetTSBackendImpl()));
}
};  // namespace compiler
}  // namespace torch_lazy_tensors
