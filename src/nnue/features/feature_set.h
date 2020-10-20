/*
    Stockfish, a UCI chess playing engine derived from Glaurung 2.1
    Copyright (C) 2004-2020 The Stockfish developers (see AUTHORS file)

    Stockfish is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Stockfish is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// A class template that represents the input feature set of the NNUE evaluation function

#ifndef NNUE_FEATURE_SET_H_INCLUDED
#define NNUE_FEATURE_SET_H_INCLUDED

#include "features_common.h"

#include <array>

namespace Eval::NNUE::Features {

    // Class template that represents a list of values
    template <typename T, T... Values>
    struct CompileTimeList;

    template <typename T, T First, T... Remaining>
    struct CompileTimeList<T, First, Remaining...> {
        static constexpr bool contains(T value) {
            return value == First || CompileTimeList<T, Remaining...>::contains(value);
        }

        static constexpr std::array<T, sizeof...(Remaining) + 1>
            kValues = {{First, Remaining...}};
    };

    template <typename T, T First, T... Remaining>
    constexpr std::array<T, sizeof...(Remaining) + 1>
        CompileTimeList<T, First, Remaining...>::kValues;

    template <typename T>
    struct CompileTimeList<T> {
        static constexpr bool contains(T /*value*/) {
            return false;
        }
        static constexpr std::array<T, 0> kValues = { {} };
    };

    // Class template that adds to the beginning of the list
    template <typename T, typename ListType, T Value>
    struct AppendToList;

    template <typename T, T... Values, T AnotherValue>
    struct AppendToList<T, CompileTimeList<T, Values...>, AnotherValue> {
        using Result = CompileTimeList<T, AnotherValue, Values...>;
    };

    // Class template for adding to a sorted, unique list
    template <typename T, typename ListType, T Value>
    struct InsertToSet;

    template <typename T, T First, T... Remaining, T AnotherValue>
    struct InsertToSet<T, CompileTimeList<T, First, Remaining...>, AnotherValue> {
        using Result =
            std::conditional_t<
                CompileTimeList<T, First, Remaining...>::contains(AnotherValue),
                CompileTimeList<T, First, Remaining...>,
                std::conditional_t<
                    (AnotherValue < First),
                    CompileTimeList<T, AnotherValue, First, Remaining...>,
                    typename AppendToList<T, typename InsertToSet<
                        T, CompileTimeList<T, Remaining...>, AnotherValue>::Result,
                        First
                    >::Result
                >
            >;
    };

    template <typename T, T Value>
    struct InsertToSet<T, CompileTimeList<T>, Value> {
        using Result = CompileTimeList<T, Value>;
    };

    // Base class of feature set
    template <typename Derived>
    class FeatureSetBase {

       public:
        // Get a list of indices for active features
        template <typename IndexListType>
        static void append_active_indices(
            const Position& pos, TriggerEvent trigger, IndexListType active[2]) {

            for (Color perspective : { WHITE, BLACK }) {
                Derived::collect_active_indices(
                    pos, trigger, perspective, &active[perspective]);
            }
        }

        // Get a list of indices for recently changed features
        template <typename PositionType, typename IndexListType>
        static void append_changed_indices(
            const PositionType& pos,
            TriggerEvent trigger,
            IndexListType removed[2],
            IndexListType added[2],
            bool reset[2]) {

            const auto& dp = pos.state()->dirtyPiece;

            for (Color perspective : { WHITE, BLACK }) {
                switch (trigger) {
                    case TriggerEvent::kNone:
                        break;
                    case TriggerEvent::kFriendKingMoved:
                        if (dp.dirty_num == 0) continue;
                        reset[perspective] = dp.piece[0] == make_piece(perspective, KING);
                        break;
                    case TriggerEvent::kEnemyKingMoved:
                        if (dp.dirty_num == 0) continue;
                        reset[perspective] = dp.piece[0] == make_piece(~perspective, KING);
                        break;
                    case TriggerEvent::kAnyKingMoved:
                        if (dp.dirty_num == 0) continue;
                        reset[perspective] = type_of(dp.piece[0]) == KING;
                        break;
                    case TriggerEvent::kAnyPieceMoved:
                        reset[perspective] = true;
                        break;
                    default:
                        assert(false);
                        break;
                }

                if (reset[perspective]) {
                    Derived::collect_active_indices(
                        pos, trigger, perspective, &added[perspective]);
                } else {
                    Derived::collect_changed_indices(
                        pos, trigger, perspective,
                        &removed[perspective], &added[perspective]);
                }
            }
        }
    };

    // Class template that represents the feature set
    // do internal processing in reverse order of template arguments in order to linearize the amount of calculation at runtime
    template <typename FirstFeatureType, typename... RemainingFeatureTypes>
    class FeatureSet<FirstFeatureType, RemainingFeatureTypes...> :
      public FeatureSetBase<
          FeatureSet<FirstFeatureType, RemainingFeatureTypes...>
      > {

    private:
        using Head = FirstFeatureType;
        using Tail = FeatureSet<RemainingFeatureTypes...>;

    public:
        // Hash value embedded in the evaluation function file
        static constexpr std::uint32_t kHashValue =
            Head::kHashValue ^ (Tail::kHashValue << 1) ^ (Tail::kHashValue >> 31);

        // number of feature dimensions
        static constexpr IndexType kDimensions =
            Head::kDimensions + Tail::kDimensions;

        // The maximum value of the number of indexes whose value is 1 at the same time among the feature values
        static constexpr IndexType kMaxActiveDimensions =
            Head::kMaxActiveDimensions + Tail::kMaxActiveDimensions;

        // List of timings to perform all calculations instead of difference calculation
        using SortedTriggerSet = typename InsertToSet<TriggerEvent,
            typename Tail::SortedTriggerSet, Head::kRefreshTrigger>::Result;

        static constexpr auto kRefreshTriggers = SortedTriggerSet::kValues;

        // Get the feature quantity name
        static std::string get_name() {
            return std::string(Head::kName) + "+" + Tail::get_name();
        }

    private:
        // Get a list of indices with a value of 1 among the features
        template <typename IndexListType>
        static void collect_active_indices(
            const Position& pos,
            const TriggerEvent trigger,
            const Color perspective,
            IndexListType* const active) {

            Tail::collect_active_indices(pos, trigger, perspective, active);
            if (Head::kRefreshTrigger == trigger) {
                const auto start = active->size();
                Head::append_active_indices(pos, perspective, active);

                for (auto i = start; i < active->size(); ++i) {
                    (*active)[i] += Tail::kDimensions;
                }
            }
        }

        // Get a list of indices whose values have changed from the previous one in the feature quantity
        template <typename IndexListType>
        static void collect_changed_indices(
            const Position& pos,
            const TriggerEvent trigger,
            const Color perspective,
            IndexListType* const removed,
            IndexListType* const added) {

            Tail::collect_changed_indices(pos, trigger, perspective, removed, added);
            if (Head::kRefreshTrigger == trigger) {
                const auto start_removed = removed->size();
                const auto start_added = added->size();
                Head::append_changed_indices(pos, perspective, removed, added);

                for (auto i = start_removed; i < removed->size(); ++i) {
                    (*removed)[i] += Tail::kDimensions;
                }

                for (auto i = start_added; i < added->size(); ++i) {
                    (*added)[i] += Tail::kDimensions;
                }
            }
        }

        // Make the base class and the class template that recursively uses itself a friend
        friend class FeatureSetBase<FeatureSet>;

        template <typename... FeatureTypes>
        friend class FeatureSet;
    };

    // Class template that represents the feature set
    template <typename FeatureType>
    class FeatureSet<FeatureType> : public FeatureSetBase<FeatureSet<FeatureType>> {

    public:
        // Hash value embedded in the evaluation file
        static constexpr std::uint32_t kHashValue = FeatureType::kHashValue;

        // Number of feature dimensions
        static constexpr IndexType kDimensions = FeatureType::kDimensions;

        // Maximum number of simultaneously active features
        static constexpr IndexType kMaxActiveDimensions =
            FeatureType::kMaxActiveDimensions;

        // Trigger for full calculation instead of difference calculation
        using SortedTriggerSet =
            CompileTimeList<TriggerEvent, FeatureType::kRefreshTrigger>;

        static constexpr auto kRefreshTriggers = SortedTriggerSet::kValues;

        // Get the feature quantity name
        static std::string get_name() {
            return FeatureType::kName;
        }

    private:
        // Get a list of indices for active features
        static void collect_active_indices(
            const Position& pos,
            const TriggerEvent trigger,
            const Color perspective,
            IndexList* const active) {

            if (FeatureType::kRefreshTrigger == trigger) {
              FeatureType::append_active_indices(pos, perspective, active);
            }
        }

        // Get a list of indices for recently changed features
        static void collect_changed_indices(
            const Position& pos,
            const TriggerEvent trigger,
            const Color perspective,
            IndexList* const removed,
            IndexList* const added) {

            if (FeatureType::kRefreshTrigger == trigger) {
              FeatureType::append_changed_indices(pos, perspective, removed, added);
            }
        }

        // Make the base class and the class template that recursively uses itself a friend
        friend class FeatureSetBase<FeatureSet>;

        template <typename... FeatureTypes>
        friend class FeatureSet;
    };

}  // namespace Eval::NNUE::Features

#endif // #ifndef NNUE_FEATURE_SET_H_INCLUDED
