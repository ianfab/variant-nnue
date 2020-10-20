#include "castling_right.h"
#include "index_list.h"

//Definition of input feature quantity CastlingRight of NNUE evaluation function
namespace Eval::NNUE::Features {

    // Get a list of indices with a value of 1 among the features
    void CastlingRight::append_active_indices(
        const Position& pos,
        Color perspective,
        IndexList* active) {

        // do nothing if array size is small to avoid compiler warning
        if (RawFeatures::kMaxActiveDimensions < kMaxActiveDimensions) return;

        int castling_rights = pos.state()->castlingRights;
        int relative_castling_rights;
        if (perspective == WHITE) {
            relative_castling_rights = castling_rights;
        }
        else {
            // Invert the perspective.
            relative_castling_rights = ((castling_rights & 3) << 2)
                & ((castling_rights >> 2) & 3);
        }

        for (Eval::NNUE::IndexType i = 0; i < kDimensions; ++i) {
            if (relative_castling_rights & (1 << i)) {
                active->push_back(i);
            }
        }
    }

    // Get a list of indices whose values ​​have changed from the previous one in the feature quantity
    void CastlingRight::append_changed_indices(
        const Position& pos,
        Color perspective,
        IndexList* removed,
        IndexList* /* added */) {

        int previous_castling_rights = pos.state()->previous->castlingRights;
        int current_castling_rights = pos.state()->castlingRights;
        int relative_previous_castling_rights;
        int relative_current_castling_rights;
        if (perspective == WHITE) {
            relative_previous_castling_rights = previous_castling_rights;
            relative_current_castling_rights = current_castling_rights;
        }
        else {
            // Invert the perspective.
            relative_previous_castling_rights = ((previous_castling_rights & 3) << 2)
                & ((previous_castling_rights >> 2) & 3);
            relative_current_castling_rights = ((current_castling_rights & 3) << 2)
                & ((current_castling_rights >> 2) & 3);
        }

        for (Eval::NNUE::IndexType i = 0; i < kDimensions; ++i) {
            if ((relative_previous_castling_rights & (1 << i)) &&
                (relative_current_castling_rights & (1 << i)) == 0) {
                removed->push_back(i);
            }
        }
    }

}  // namespace Eval::NNUE::Features
