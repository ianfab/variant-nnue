﻿#ifndef _NNUE_TRAINER_INPUT_SLICE_H_
#define _NNUE_TRAINER_INPUT_SLICE_H_

#include "trainer.h"

#include "learn/learn.h"

#include "nnue/layers/input_slice.h"

// Specialization of NNUE evaluation function learning class template for InputSlice
namespace Eval::NNUE {

    // Learning: Input layer
    class SharedInputTrainer {
    public:
        // factory function
        static std::shared_ptr<SharedInputTrainer> create(
            FeatureTransformer* ft) {

            static std::shared_ptr<SharedInputTrainer> instance;

            if (!instance) {
                instance.reset(new SharedInputTrainer(ft));
            }

            ++instance->num_referrers_;

            return instance;
        }

        // Set options such as hyperparameters
        void send_message(Message* message) {
            if (num_calls_ == 0) {
                current_operation_ = Operation::kSendMessage;
                feature_transformer_trainer_->send_message(message);
            }

            assert(current_operation_ == Operation::kSendMessage);

            if (++num_calls_ == num_referrers_) {
                num_calls_ = 0;
                current_operation_ = Operation::kNone;
            }
        }

        // Initialize the parameters with random numbers
        template <typename RNG>
        void initialize(RNG& rng) {
            if (num_calls_ == 0) {
                current_operation_ = Operation::kInitialize;
                feature_transformer_trainer_->initialize(rng);
            }

            assert(current_operation_ == Operation::kInitialize);

            if (++num_calls_ == num_referrers_) {
                num_calls_ = 0;
                current_operation_ = Operation::kNone;
            }
        }

        // forward propagation
        const LearnFloatType* propagate(const std::vector<Example>& batch) {
            if (gradients_.size() < kInputDimensions * batch.size()) {
                gradients_.resize(kInputDimensions * batch.size());
            }

            batch_size_ = static_cast<IndexType>(batch.size());

            if (num_calls_ == 0) {
                current_operation_ = Operation::kPropagate;
                output_ = feature_transformer_trainer_->propagate(batch);
            }

            assert(current_operation_ == Operation::kPropagate);

            if (++num_calls_ == num_referrers_) {
                num_calls_ = 0;
                current_operation_ = Operation::kNone;
            }

            return output_;
        }

        // backpropagation
        void backpropagate(const LearnFloatType* gradients,
                           LearnFloatType learning_rate) {

            if (num_referrers_ == 1) {
                feature_transformer_trainer_->backpropagate(gradients, learning_rate);
                return;
            }

            if (num_calls_ == 0) {
                current_operation_ = Operation::kBackPropagate;
                for (IndexType b = 0; b < batch_size_; ++b) {
                    const IndexType batch_offset = kInputDimensions * b;
                    for (IndexType i = 0; i < kInputDimensions; ++i) {
                        gradients_[batch_offset + i] = static_cast<LearnFloatType>(0.0);
                    }
                }
            }

            assert(current_operation_ == Operation::kBackPropagate);

            for (IndexType b = 0; b < batch_size_; ++b) {
                const IndexType batch_offset = kInputDimensions * b;
                for (IndexType i = 0; i < kInputDimensions; ++i) {
                    gradients_[batch_offset + i] += gradients[batch_offset + i];
                }
            }

            if (++num_calls_ == num_referrers_) {
                feature_transformer_trainer_->backpropagate(
                    gradients_.data(), learning_rate);
                num_calls_ = 0;
                current_operation_ = Operation::kNone;
            }
        }

    private:
        // constructor
        SharedInputTrainer(FeatureTransformer* ft) :
            batch_size_(0),
            num_referrers_(0),
            num_calls_(0),
            current_operation_(Operation::kNone),
            feature_transformer_trainer_(Trainer<FeatureTransformer>::create(
                ft)),
            output_(nullptr) {
        }

        // number of input/output dimensions
        static constexpr IndexType kInputDimensions =
            FeatureTransformer::kOutputDimensions;

        // type of processing
        enum class Operation {
            kNone,
            kSendMessage,
            kInitialize,
            kPropagate,
            kBackPropagate,
        };

        // number of samples in mini-batch
        IndexType batch_size_;

        // number of layers sharing this layer as input
        std::uint32_t num_referrers_;

        // Number of times the current process has been called
        std::uint32_t num_calls_;

        // current processing type
        Operation current_operation_;

        // Trainer of input feature converter
        const std::shared_ptr<Trainer<FeatureTransformer>>
            feature_transformer_trainer_;

        // pointer to output shared for forward propagation
        const LearnFloatType* output_;

        // buffer for back propagation
        std::vector<LearnFloatType> gradients_;
    };

    // Learning: Input layer
    template <IndexType OutputDimensions, IndexType Offset>
    class Trainer<Layers::InputSlice<OutputDimensions, Offset>> {
    private:
        // Type of layer to learn
        using LayerType = Layers::InputSlice<OutputDimensions, Offset>;

    public:
        // factory function
        static std::shared_ptr<Trainer> create(
            LayerType* /*target_layer*/, FeatureTransformer* ft) {

            return std::shared_ptr<Trainer>(new Trainer(ft));
        }

        // Set options such as hyperparameters
        void send_message(Message* message) {
            shared_input_trainer_->send_message(message);
        }

        // Initialize the parameters with random numbers
        template <typename RNG>
        void initialize(RNG& rng) {
            shared_input_trainer_->initialize(rng);
        }

        // forward propagation
        const LearnFloatType* propagate(const std::vector<Example>& batch) {
            if (output_.size() < kOutputDimensions * batch.size()) {
              output_.resize(kOutputDimensions * batch.size());
              gradients_.resize(kInputDimensions * batch.size());
            }

            batch_size_ = static_cast<IndexType>(batch.size());

            const auto input = shared_input_trainer_->propagate(batch);
            for (IndexType b = 0; b < batch_size_; ++b) {
                const IndexType input_offset = kInputDimensions * b;
                const IndexType output_offset = kOutputDimensions * b;
#if defined(USE_BLAS)
                cblas_scopy(kOutputDimensions, &input[input_offset + Offset], 1,
                            &output_[output_offset], 1);
#else
                for (IndexType i = 0; i < kOutputDimensions; ++i) {
                    output_[output_offset + i] = input[input_offset + Offset + i];
                }
#endif
            }

            return output_.data();
        }

        // backpropagation
        void backpropagate(const LearnFloatType* gradients,
                           LearnFloatType learning_rate) {

            for (IndexType b = 0; b < batch_size_; ++b) {
                const IndexType input_offset = kInputDimensions * b;
                const IndexType output_offset = kOutputDimensions * b;
                for (IndexType i = 0; i < kInputDimensions; ++i) {
                    if ((int)i < (int)Offset || i >= Offset + kOutputDimensions) {
                        gradients_[input_offset + i] = static_cast<LearnFloatType>(0.0);
                    } else {
                        gradients_[input_offset + i] = gradients[output_offset + i - Offset];
                    }
                }
            }
            shared_input_trainer_->backpropagate(gradients_.data(), learning_rate);
        }

    private:
        // constructor
        Trainer(FeatureTransformer* ft):
            batch_size_(0),
            shared_input_trainer_(SharedInputTrainer::create(ft)) {
        }

        // number of input/output dimensions
        static constexpr IndexType kInputDimensions =
            FeatureTransformer::kOutputDimensions;
        static constexpr IndexType kOutputDimensions = OutputDimensions;
        static_assert(Offset + kOutputDimensions <= kInputDimensions, "");

        // number of samples in mini-batch
        IndexType batch_size_;

        // Trainer of shared input layer
        const std::shared_ptr<SharedInputTrainer> shared_input_trainer_;

        // Forward propagation buffer
        std::vector<LearnFloatType> output_;

        // buffer for back propagation
        std::vector<LearnFloatType> gradients_;
    };

}  // namespace Eval::NNUE

#endif
