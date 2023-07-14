// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cmath>

#include "column/type_traits.h"
#include "exprs/agg/aggregate.h"
#include "gutil/casts.h"

namespace starrocks {

template <typename T>
struct DeltaMethodAggregateState {
    T x_sum{};
    T y_sum{};
    T x_square_sum{};
    T y_square_sum{};
    T x_y_sum{};
    int64_t count = 0;
};

template <LogicalType LT, bool is_sample, typename T = RunTimeCppType<TYPE_DOUBLE>,
          LogicalType ResultLT = TYPE_DOUBLE, typename TResult = RunTimeCppType<ResultLT>>
class DeltaMethodBaseAggregateFunction
        : public AggregateFunctionBatchHelper<DeltaMethodAggregateState<T>,
                                              DeltaMethodBaseAggregateFunction<LT, is_sample, T, ResultLT, TResult>> {
public:
    using InputColumnType = RunTimeColumnType<LT>;
    using InputCppType = RunTimeCppType<LT>;
    using ResultColumnType = RunTimeColumnType<ResultLT>;

    void reset(FunctionContext* ctx, const Columns& args, AggDataPtr state) const override {
        this->data(state).x_sum = {};
        this->data(state).y_sum = {};
        this->data(state).x_square_sum = {};
        this->data(state).y_square_sum = {};
        this->data(state).x_y_sum = {};
        this->data(state).count = 0;
    }

    void update(FunctionContext* ctx, const Column** columns, AggDataPtr __restrict state,
                size_t row_num) const override {
        DCHECK(columns[0]->is_numeric() || columns[0]->is_decimal());

        this->data(state).count += 1;

        T x;
        T y;
        const auto* x_column = down_cast<const InputColumnType*>(columns[0]);
        const auto* y_column = down_cast<const InputColumnType*>(columns[1]);
        x = x_column->get_data()[row_num];
        this->data(state).x_sum += x;
        this->data(state).x_square_sum += x * x;

        y = y_column->get_data()[row_num];
        this->data(state).y_sum += y;
        this->data(state).y_square_sum += y * y;

//        LOG(WARNING) << "update x: " << x << ", y: " << y;

        this->data(state).x_y_sum += x * y;

    }

    void update_batch_single_state_with_frame(FunctionContext* ctx, AggDataPtr __restrict state, const Column** columns,
                                              int64_t peer_group_start, int64_t peer_group_end, int64_t frame_start,
                                              int64_t frame_end) const override {
        for (size_t i = frame_start; i < frame_end; ++i) {
            update(ctx, columns, state, i);
        }
    }

    void merge(FunctionContext* ctx, const Column* column, AggDataPtr __restrict state, size_t row_num) const override {
        DCHECK(column->is_binary());
        Slice slice = column->get(row_num).get_slice();
        auto x_sum = unaligned_load<T>(slice.data);
        auto y_sum = unaligned_load<T>(slice.data + sizeof(T));
        auto x_square_sum = unaligned_load<T>(slice.data + sizeof(T) * 2);
        auto y_square_sum = unaligned_load<T>(slice.data + sizeof(T) * 3);
        auto x_y_sum = unaligned_load<T>(slice.data + sizeof(T) * 4);
        int64_t count = *reinterpret_cast<int64_t*>(slice.data + sizeof(T) * 5);


        this->data(state).x_sum += x_sum;
        this->data(state).y_sum += y_sum;
        this->data(state).x_square_sum += x_square_sum;
        this->data(state).y_square_sum += y_square_sum;
        this->data(state).x_y_sum += x_y_sum;
        this->data(state).count += count;
    }

    void serialize_to_column(FunctionContext* ctx, ConstAggDataPtr __restrict state, Column* to) const override {
        DCHECK(to->is_binary());
        auto* column = down_cast<BinaryColumn*>(to);
        Bytes& bytes = column->get_bytes();

        size_t old_size = bytes.size();
        size_t new_size = old_size + sizeof(T) * 5 + sizeof(int64_t);
        bytes.resize(new_size);

        memcpy(bytes.data() + old_size, &(this->data(state).x_sum), sizeof(T));
        memcpy(bytes.data() + old_size + sizeof(T), &(this->data(state).y_sum), sizeof(T));
        memcpy(bytes.data() + old_size + sizeof(T) * 2, &(this->data(state).x_square_sum), sizeof(T));
        memcpy(bytes.data() + old_size + sizeof(T) * 3, &(this->data(state).y_square_sum), sizeof(T));
        memcpy(bytes.data() + old_size + sizeof(T) * 4, &(this->data(state).x_y_sum), sizeof(T));
        memcpy(bytes.data() + old_size + sizeof(T) * 5, &(this->data(state).count), sizeof(int64_t));

        column->get_offset().emplace_back(new_size);
    }

    void convert_to_serialize_format(FunctionContext* ctx, const Columns& src, size_t chunk_size,
                                     ColumnPtr* dst) const override {
        DCHECK((*dst)->is_binary());
        auto* dst_column = down_cast<BinaryColumn*>((*dst).get());
        Bytes& bytes = dst_column->get_bytes();
        size_t old_size = bytes.size();

        size_t one_element_size = sizeof(T) * 5 + sizeof(int64_t);
        bytes.resize(one_element_size * chunk_size);
        dst_column->get_offset().resize(chunk_size + 1);

        const auto* src_column = down_cast<const InputColumnType*>(src[0].get());

        T x_sum = {};
        T y_sum = 0;
        T x_square_sum = 0;
        T y_square_sum = 0;
        T x_y_sum = 0;



        int64_t count = 1;
        for (size_t i = 0; i < chunk_size; ++i) {
            x_sum = src_column->get_data()[i];
            memcpy(bytes.data() + old_size, &x_sum, sizeof(T));
            memcpy(bytes.data() + old_size + sizeof(T), &y_sum, sizeof(T));
            memcpy(bytes.data() + old_size + sizeof(T) * 2, &x_square_sum, sizeof(T));
            memcpy(bytes.data() + old_size + sizeof(T) * 3, &y_square_sum, sizeof(T));
            memcpy(bytes.data() + old_size + sizeof(T) * 4, &x_y_sum, sizeof(T));
            memcpy(bytes.data() + old_size + sizeof(T) * 5, &count, sizeof(int64_t));
            old_size += one_element_size;
            dst_column->get_offset()[i + 1] = old_size;
        }
    }

    std::string get_name() const override { return "deviation from average"; }
};



template <LogicalType LT, bool is_sample, typename T = RunTimeCppType<TYPE_DOUBLE>,
          LogicalType ResultLT = TYPE_DOUBLE, typename TResult = RunTimeCppType<ResultLT>>
class DeltaMethodAggregateFunction final : public DeltaMethodBaseAggregateFunction<LT, is_sample, T, ResultLT, TResult> {
public:
    using ResultColumnType =
            typename DeltaMethodBaseAggregateFunction<LT, is_sample, T, ResultLT, TResult>::ResultColumnType;

    void finalize_to_column(FunctionContext* ctx, ConstAggDataPtr __restrict state, Column* to) const override {
        DCHECK(to->is_numeric() || to->is_decimal());
        double count = this->data(state).count * 1.0;
        T x_sum = this->data(state).x_sum;
        T y_sum = this->data(state).y_sum;
        T x_square_sum = this->data(state).x_square_sum;
        T y_square_sum = this->data(state).y_square_sum;
        T x_y_sum = this->data(state).x_y_sum;

//        LOG(WARNING) << "finalize_to_column x_sum: " << x_sum;
//        LOG(WARNING) << "finalize_to_column y_sum: " << y_sum;
//        LOG(WARNING) << "finalize_to_column x_square_sum: " << x_square_sum;
//        LOG(WARNING) << "finalize_to_column y_square_sum: " << y_square_sum;
//        LOG(WARNING) << "finalize_to_column x_y_sum: " << x_y_sum;
//        LOG(WARNING) << "finalize_to_column count: " << count;

        DecimalV2Value result;
        if constexpr (lt_is_decimalv2<LT>) {
            if (count > 1) {
                result.assign_from_double(1.2345);
                down_cast<ResultColumnType*>(to)->append(result);
            } else {
                down_cast<ResultColumnType*>(to)->append(DecimalV2Value(0));
            }
        } else {
            if (count > 1) {
                double avg_x = x_sum / count;
                double avg_y = y_sum / count;
                double avg_x_y = x_y_sum / count;

                double var_x = (x_square_sum - count * avg_x * avg_x) / (count - 1);
                double var_y = (y_square_sum - count * avg_y * avg_y) / (count - 1);

                double cov_x_y = (avg_x_y - avg_x * avg_y) * count / (count - 1);

                const double value = (var_x / pow(avg_y, 2) +
                                     var_y * pow(avg_x, 2) / pow(avg_y, 4) -
                        2 * cov_x_y * avg_x / pow(avg_y, 3)) / count;
                result.assign_from_double(value);
                down_cast<ResultColumnType*>(to)->append(value);
            } else {
                down_cast<ResultColumnType*>(to)->append(0);
            }
        }
    }

    void get_values(FunctionContext* ctx, ConstAggDataPtr __restrict state, Column* dst, size_t start,
                    size_t end) const override {
        DCHECK_GT(end, start);

        DecimalV2Value result;
        double count = this->data(state).count * 1.0;
        T x_sum = this->data(state).x_sum;
        T y_sum = this->data(state).y_sum;
        T x_square_sum = this->data(state).x_square_sum;
        T y_square_sum = this->data(state).y_square_sum;
        T x_y_sum = this->data(state).x_y_sum;

        if constexpr (lt_is_decimalv2<LT>) {

            if (count > 1) {
                result.assign_from_double(1.2345);
            } else {
                result = DecimalV2Value(0, 0);
            }

        } else {
            if (count > 1) {
                double avg_x = x_sum / count;
                double avg_y = y_sum / count;
                double avg_x_y = x_y_sum / count;

                double var_x = (x_square_sum - count * avg_x * avg_x) / (count - 1);
                double var_y = (y_square_sum - count * avg_y * avg_y) / (count - 1);

                double cov_x_y = (avg_x_y - avg_x * avg_y) * count / (count - 1);

                const double value = (var_x / pow(avg_y, 2) +
                                     var_y * pow(avg_x, 2) / pow(avg_y, 4) -
                                     2 * cov_x_y * avg_x / pow(avg_y, 3)) / count;
                result.assign_from_double(value);
            } else {
                result = DecimalV2Value(0, 0);
            }
        }

        auto* column = down_cast<ResultColumnType*>(dst);
        for (size_t i = start; i < end; ++i) {
            column->get_data()[i] = result;
        }
    }

    std::string get_name() const override { return "delta_method"; }
};

} // namespace starrocks
