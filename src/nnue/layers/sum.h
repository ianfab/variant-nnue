﻿#ifndef _NNUE_LAYERS_SUM_H_
#define _NNUE_LAYERS_SUM_H_

#include "nnue/nnue_common.h"

// Definition of layer Sum of NNUE evaluation function
namespace Eval::NNUE::Layers {

    // Layer that sums the output of multiple layers
    template <typename FirstPreviousLayer, typename... RemainingPreviousLayers>
    class Sum : public Sum<RemainingPreviousLayers...> {
    private:
        using Head = FirstPreviousLayer;
        using Tail = Sum<RemainingPreviousLayers...>;

     public:
        // Input/output type
        using InputType = typename Head::OutputType;

        using OutputType = InputType;

        static_assert(std::is_same<InputType, typename Tail::InputType>::value, "");

        // number of input/output dimensions
        static constexpr IndexType kInputDimensions = Head::kOutputDimensions;

        static constexpr IndexType kOutputDimensions = kInputDimensions;

        static_assert(kInputDimensions == Tail::kInputDimensions ,"");

        // Size of forward propagation buffer used in this layer
        static constexpr std::size_t kSelfBufferSize =
            ceil_to_multiple(kOutputDimensions * sizeof(OutputType), kCacheLineSize);

        // Size of the forward propagation buffer used from the input layer to this layer
        static constexpr std::size_t kBufferSize =
            std::max(Head::kBufferSize + kSelfBufferSize, Tail::kBufferSize);

        // Hash value embedded in the evaluation function file
        static constexpr std::uint32_t get_hash_value() {
            std::uint32_t hash_value = 0xBCE400B4u;
            hash_value ^= Head::get_hash_value() >> 1;
            hash_value ^= Head::get_hash_value() << 31;
            hash_value ^= Tail::get_hash_value() >> 2;
            hash_value ^= Tail::get_hash_value() << 30;
            return hash_value;
        }

        // A string that represents the structure from the input layer to this layer
        static std::string get_structure_string() {
            return "Sum[" +
                std::to_string(kOutputDimensions) + "](" + get_summands_string() + ")";
        }

        // read parameters
        bool read_parameters(std::istream& stream) {
            if (!Tail::read_parameters(stream))
                return false;

            return previous_layer_.read_parameters(stream);
        }

        // write parameters
        bool write_parameters(std::ostream& stream) const {
            if (!Tail::write_parameters(stream))
                return false;

            return previous_layer_.write_parameters(stream);
        }

        // forward propagation
        const OutputType* propagate(
            const TransformedFeatureType* transformed_features, char* buffer) const {

            Tail::propagate(transformed_features, buffer);

            const auto head_output = previous_layer_.propagate(
                transformed_features, buffer + kSelfBufferSize);

            const auto output = reinterpret_cast<OutputType*>(buffer);

            for (IndexType i = 0; i <kOutputDimensions; ++i) {
                output[i] += head_output[i];
            }

            return output;
        }

    protected:
        // A string that represents the list of layers to be summed
        static std::string get_summands_string() {
            return Head::get_structure_string() + "," + Tail::get_summands_string();
        }

        // Make the learning class a friend
        friend class Trainer<Sum>;

        // the layer immediately before this layer
        FirstPreviousLayer previous_layer_;
    };

    // Layer that sums the output of multiple layers (when there is one template argument)
    template <typename PreviousLayer>
    class Sum<PreviousLayer> {
    public:
        // Input/output type
        using InputType = typename PreviousLayer::OutputType;

        using OutputType = InputType;

        // number of input/output dimensions
        static constexpr IndexType kInputDimensions =
            PreviousLayer::kOutputDimensions;

        static constexpr IndexType kOutputDimensions = kInputDimensions;

        // Size of the forward propagation buffer used from the input layer to this layer
        static constexpr std::size_t kBufferSize = PreviousLayer::kBufferSize;

        // Hash value embedded in the evaluation function file
        static constexpr std::uint32_t get_hash_value() {
            std::uint32_t hash_value = 0xBCE400B4u;
            hash_value ^= PreviousLayer::get_hash_value() >> 1;
            hash_value ^= PreviousLayer::get_hash_value() << 31;
            return hash_value;
        }

        // A string that represents the structure from the input layer to this layer
        static std::string get_structure_string() {
            return "Sum[" +
                std::to_string(kOutputDimensions) + "](" + get_summands_string() + ")";
        }

        // read parameters
        bool read_parameters(std::istream& stream) {
            return previous_layer_.read_parameters(stream);
        }

        // write parameters
        bool write_parameters(std::ostream& stream) const {
            return previous_layer_.write_parameters(stream);
        }

        // forward propagation
        const OutputType* propagate(
            const TransformedFeatureType* transformed_features, char* buffer) const {

            return previous_layer_.propagate(transformed_features, buffer);
        }

    protected:
        // A string that represents the list of layers to be summed
        static std::string get_summands_string() {
            return PreviousLayer::get_structure_string();
        }

        // Make the learning class a friend
        friend class Trainer<Sum>;

        // the layer immediately before this layer
        PreviousLayer previous_layer_;
    };

}  // namespace Eval::NNUE::Layers

#endif
