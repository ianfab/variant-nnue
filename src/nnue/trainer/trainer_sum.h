﻿#ifndef _NNUE_TRAINER_SUM_H_
#define _NNUE_TRAINER_SUM_H_

#include "trainer.h"

#include "learn/learn.h"

#include "nnue/layers/sum.h"

// Specialization of NNUE evaluation function learning class template for Sum
namespace Eval::NNUE {

    // Learning: A layer that sums the outputs of multiple layers
    template <typename FirstPreviousLayer, typename... RemainingPreviousLayers>
    class Trainer<Layers::Sum<FirstPreviousLayer, RemainingPreviousLayers...>> :
          Trainer<Layers::Sum<RemainingPreviousLayers...>> {
    private:
        // Type of layer to learn
        using LayerType = Layers::Sum<FirstPreviousLayer, RemainingPreviousLayers...>;
        using Tail = Trainer<Layers::Sum<RemainingPreviousLayers...>>;

    public:
        // factory function
        static std::shared_ptr<Trainer> create(
            LayerType* target_layer, FeatureTransformer* ft) {

            return std::shared_ptr<Trainer>(
                new Trainer(target_layer, ft));
        }

        // Set options such as hyperparameters
        void send_message(Message* message) {
            // The results of other member functions do not depend on the processing order, so
            // Tail is processed first for the purpose of simplifying the implementation, but
            // SendMessage processes Head first to make it easier to understand subscript correspondence
            previous_layer_trainer_->send_message(message);
            Tail::send_message(message);
        }

        // Initialize the parameters with random numbers
        template <typename RNG>
        void initialize(RNG& rng) {
            Tail::initialize(rng);
            previous_layer_trainer_->initialize(rng);
        }

        // forward propagation
        /*const*/ LearnFloatType* propagate(const std::vector<Example>& batch) {
            batch_size_ = static_cast<IndexType>(batch.size());
            auto output = Tail::propagate(batch);
            const auto head_output = previous_layer_trainer_->propagate(batch);

#if defined(USE_BLAS)
            cblas_saxpy(kOutputDimensions * batch_size_, 1.0,
                        head_output, 1, output, 1);
#else
            for (IndexType b = 0; b < batch_size_; ++b) {
                const IndexType batch_offset = kOutputDimensions * b;
                for (IndexType i = 0; i < kOutputDimensions; ++i) {
                    output[batch_offset + i] += head_output[batch_offset + i];
                }
            }

#endif
            return output;
        }

        // backpropagation
        void backpropagate(const LearnFloatType* gradients,
                           LearnFloatType learning_rate) {

            Tail::backpropagate(gradients, learning_rate);
            previous_layer_trainer_->backpropagate(gradients, learning_rate);
        }

    private:
        // constructor
        Trainer(LayerType* target_layer, FeatureTransformer* ft):
            Tail(target_layer, ft),
            batch_size_(0),
            previous_layer_trainer_(Trainer<FirstPreviousLayer>::create(
                &target_layer->previous_layer_, ft)),
            target_layer_(target_layer) {
        }

        // number of input/output dimensions
        static constexpr IndexType kOutputDimensions = LayerType::kOutputDimensions;

        // make subclass friend
        template <typename SumLayer>
        friend class Trainer;

        // number of samples in mini-batch
        IndexType batch_size_;

        // Trainer of the previous layer
        const std::shared_ptr<Trainer<FirstPreviousLayer>> previous_layer_trainer_;

        // layer to learn
        LayerType* const target_layer_;
    };


    // Learning: Layer that takes the sum of the outputs of multiple layers (when there is one template argument)
    template <typename PreviousLayer>
    class Trainer<Layers::Sum<PreviousLayer>> {
    private:
        // Type of layer to learn
        using LayerType = Layers::Sum<PreviousLayer>;

    public:
        // factory function
        static std::shared_ptr<Trainer> create(
            LayerType* target_layer, FeatureTransformer* ft) {

            return std::shared_ptr<Trainer>(
                new Trainer(target_layer, ft));
        }

        // Set options such as hyperparameters
        void send_message(Message* message) {
            previous_layer_trainer_->send_message(message);
        }

        // Initialize the parameters with random numbers
        template <typename RNG>
        void initialize(RNG& rng) {
            previous_layer_trainer_->initialize(rng);
        }

        // forward propagation
        /*const*/ LearnFloatType* propagate(const std::vector<Example>& batch) {
            if (output_.size() < kOutputDimensions * batch.size()) {
                output_.resize(kOutputDimensions * batch.size());
            }

            batch_size_ = static_cast<IndexType>(batch.size());
            const auto output = previous_layer_trainer_->propagate(batch);

#if defined(USE_BLAS)
            cblas_scopy(kOutputDimensions * batch_size_, output, 1, &output_[0], 1);
#else
            for (IndexType b = 0; b < batch_size_; ++b) {
                const IndexType batch_offset = kOutputDimensions * b;
                for (IndexType i = 0; i < kOutputDimensions; ++i) {
                    output_[batch_offset + i] = output[batch_offset + i];
                }
            }

#endif
            return output_.data();
        }

        // backpropagation
        void backpropagate(const LearnFloatType* gradients,
                           LearnFloatType learning_rate) {

            previous_layer_trainer_->backpropagate(gradients, learning_rate);
        }

    private:
        // constructor
        Trainer(LayerType* target_layer, FeatureTransformer* ft) :
            batch_size_(0),
            previous_layer_trainer_(Trainer<PreviousLayer>::create(
                &target_layer->previous_layer_, ft)),
            target_layer_(target_layer) {
        }

        // number of input/output dimensions
        static constexpr IndexType kOutputDimensions = LayerType::kOutputDimensions;

        // make subclass friend
        template <typename SumLayer>
        friend class Trainer;

        // number of samples in mini-batch
        IndexType batch_size_;

        // Trainer of the previous layer
        const std::shared_ptr<Trainer<PreviousLayer>> previous_layer_trainer_;

        // layer to learn
        LayerType* const target_layer_;

        // Forward propagation buffer
        std::vector<LearnFloatType> output_;
    };

}  // namespace Eval::NNUE

#endif
