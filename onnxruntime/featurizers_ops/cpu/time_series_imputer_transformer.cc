// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/common/common.h"
#include "core/framework/data_types.h"
#include "core/framework/op_kernel.h"

#include <cstdlib>
#include <limits>

#include "Featurizers/TimeSeriesImputerFeaturizer.h"
#include "Archive.h"

namespace ft = Microsoft::Featurizer::Featurizers;

namespace onnxruntime {
namespace featurizers {

namespace timeseries_imputer_details {

inline std::chrono::system_clock::time_point ToTimePoint(int64_t secs) {
  return std::chrono::system_clock::from_time_t(secs);
}

inline int64_t ToSecs(const std::chrono::system_clock::time_point& tp) {
  using namespace std::chrono;
  return duration_cast<seconds>(tp.time_since_epoch()).count();
}

template <typename T>
struct ToString {
  std::string operator()(T val) const {
    return std::to_string(val);
  }
};

template <>
struct ToString<std::string> {
  const std::string& operator()(const std::string& val) const {
    return val;
  }
};

template <typename T>
struct ToStringOptional {
  nonstd::optional<std::string> operator()(T val) const {
    nonstd::optional<std::string> result;
    if (std::isnan(val)) {
      return result;
    }
    result = std::to_string(val);
    return result;
  }
};

template <>
struct ToStringOptional<std::string> {
  nonstd::optional<std::string> operator()(std::string val) const {
    return (val.empty()) ? nonstd::optional<std::string>() : nonstd::optional<std::string>(std::move(val));
  }
};

template <typename T>
struct FromString;

template <>
struct FromString<std::string> {
  const std::string& operator()(const std::string& val) const {
    return val;
  }
};

template <>
struct FromString<float> {
  float operator()(const std::string& val) const {
    char* str_end = nullptr;
    const char* str = val.c_str();
    float result = std::strtof(str, &str_end);
    if (str == str_end) {
      ORT_THROW("Resulting key string is not convertible to float: ", val);
    }
    return result;
  }
};

template <>
struct FromString<double> {
  double operator()(const std::string& val) const {
    const char* str = val.c_str();
    char* str_end = nullptr;
    double result = std::strtod(str, &str_end);
    if (str == str_end) {
      ORT_THROW("Resulting key string is not convertible to double: ", val);
    }
    return result;
  }
};
template <typename T>
struct FromStringOptional {
  T operator()(const nonstd::optional<std::string>& val) const {
    if (val.has_value()) {
      return FromString<T>()(*val);
    }
    return std::numeric_limits<T>::quiet_NaN();
  }
};

template <>
struct FromStringOptional<std::string> {
  std::string operator()(const nonstd::optional<std::string>& val) const {
    if (val.has_value()) {
      return *val;
    }
    return std::string();
  }
};
}  // namespace timeseries_imputer_details

template <typename T>
struct TimeSeriesImputerTransformerImpl {
  void operator()(OpKernelContext* ctx, int64_t rows) {
    const auto& state = *ctx->Input<Tensor>(0);
    const uint8_t* const state_data = state.template Data<uint8_t>();

    const auto& times = *ctx->Input<Tensor>(1);
    const auto& keys = *ctx->Input<Tensor>(2);
    const auto& data = *ctx->Input<Tensor>(3);

    const int64_t keys_per_row = keys.Shape()[1];
    const int64_t columns = data.Shape()[1];

    using namespace timeseries_imputer_details;

    using OutputType = std::tuple<bool, std::chrono::system_clock::time_point,
                                  std::vector<std::string>, std::vector<nonstd::optional<std::string>>>;
    std::vector<OutputType> output_rows;
    std::function<void(OutputType)> callback_fn;
    callback_fn = [&output_rows](OutputType value) -> void {
      output_rows.emplace_back(std::move(value));
    };

    Microsoft::Featurizer::Archive archive(state_data, state.Shape().Size());
    ft::Components::TimeSeriesImputerEstimator::Transformer transformer(archive);

    const int64_t* times_data = times.template Data<int64_t>();
    const T* const keys_data = keys.template Data<T>();
    const T* const data_data = data.template Data<T>();

    // for each row get timestamp, get all keys, get all data and feed it
    for (int64_t row = 0; row < rows; ++row) {
      const T* const key_row_data = keys_data + (row * keys_per_row);
      const T* const keys_row_end = key_row_data + keys_per_row;
      std::vector<std::string> str_keys;
      std::transform(key_row_data, keys_row_end, std::back_inserter(str_keys),
                     ToString<T>());

      std::vector<nonstd::optional<std::string>> str_data;
      const T* const data_row = data_data + (row * columns);
      const T* const data_row_end = data_row + columns;
      std::transform(data_row, data_row_end, std::back_inserter(str_data),
                     ToStringOptional<T>());

      auto tuple_row = std::make_tuple(ToTimePoint(*times_data), std::move(str_keys), std::move(str_data));

      transformer.execute(tuple_row, callback_fn);
      ++times_data;
    }

    transformer.flush(callback_fn);

    // Compute output shapes now
    // Number of outputs is the number of rows,
    int64_t output_rows_num = static_cast<int64_t>(output_rows.size());
    TensorShape rows_shape({output_rows_num});
    TensorShape keys_shape({output_rows_num, keys_per_row});
    TensorShape data_shape({output_rows_num, columns});

    auto* added_output = ctx->Output(0, rows_shape)->template MutableData<bool>();
    auto* time_output = ctx->Output(1, rows_shape)->template MutableData<int64_t>();
    auto* keys_output = ctx->Output(2, keys_shape)->template MutableData<T>();
    auto* data_output = ctx->Output(3, data_shape)->template MutableData<T>();

    for (const auto& out : output_rows) {
      *added_output++ = std::get<0>(out);
      *time_output++ = ToSecs(std::get<1>(out));
      const auto& imputed_keys = std::get<2>(out);
      ORT_ENFORCE(static_cast<int64_t>(imputed_keys.size()) == keys_per_row,
                  "resulting number of keys: ", imputed_keys.size(), " expected: ", keys_per_row);
      const auto& imputed_data = std::get<3>(out);
      ORT_ENFORCE(static_cast<int64_t>(imputed_data.size()) == columns,
                  "resulting number of columns: ", imputed_data.size(), " expected: ", columns);
      keys_output = std::transform(imputed_keys.cbegin(), imputed_keys.cend(), keys_output,
                                   FromString<T>());
      data_output = std::transform(imputed_data.cbegin(), imputed_data.cend(), data_output,
                                   FromStringOptional<T>());
    }
  }
};

class TimeSeriesImputerTransformer final : public OpKernel {
 public:
  explicit TimeSeriesImputerTransformer(const OpKernelInfo& info) : OpKernel(info) {
  }

  static Status CheckBatches(int64_t rows, const TensorShape& shape) {
    if (shape.NumDimensions() == 2) {
      ORT_RETURN_IF_NOT(rows == shape[0], "Number of rows does not match");
    } else {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Expect shape of [R][C]");
    }
    return Status::OK();
  }

  Status Compute(OpKernelContext* ctx) const override {
    const auto& times = *ctx->Input<Tensor>(1);
    const auto& times_shape = times.Shape();
    ORT_RETURN_IF_NOT(times_shape.NumDimensions() == 1, "Times must have shape [B][R] or [R]");
    int64_t rows = times_shape[0];

    const auto& keys = *ctx->Input<Tensor>(2);
    ORT_RETURN_IF_ERROR(CheckBatches(rows, keys.Shape()));
    const auto& data = *ctx->Input<Tensor>(3);
    ORT_RETURN_IF_ERROR(CheckBatches(rows, data.Shape()));

    auto data_type = data.GetElementType();
    ORT_RETURN_IF_NOT(keys.GetElementType() == data_type, "Keys and data must have the same datatype");

    TimeSeriesImputerTransformerImpl<std::string>()(ctx, rows);
    return Status::OK();
  }
};

ONNX_OPERATOR_KERNEL_EX(
    TimeSeriesImputerTransformer,
    kMSFeaturizersDomain,
    1,
    kCpuExecutionProvider,
    KernelDefBuilder()
        .TypeConstraint("T0", DataTypeImpl::GetTensorType<uint8_t>())
        .TypeConstraint("T1", DataTypeImpl::GetTensorType<int64_t>())
        .TypeConstraint("T2", DataTypeImpl::GetTensorType<std::string>()),
    TimeSeriesImputerTransformer);
}  // namespace featurizers
}  // namespace onnxruntime
