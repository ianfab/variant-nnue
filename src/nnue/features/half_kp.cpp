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

//Definition of input features HalfKP of NNUE evaluation function

#include "half_kp.h"
#include "index_list.h"

namespace Eval::NNUE::Features {

    // Orient a square according to perspective (flip rank for black)
    inline Square orient(Color perspective, Square s) {
        return Square(int(s) ^ (bool(perspective) * SQ_H8));
    }

    // Find the index of the feature quantity from the king position and PieceSquare
    template <Side AssociatedKing>
    inline IndexType HalfKP<AssociatedKing>::make_index(
        Color perspective,
        Square s,
        Piece pc,
        Square ksq) {

        return IndexType(orient(perspective, s) + kpp_board_index[pc][perspective] + PS_END * ksq);
    }

    // Get a list of indices for active features
    template <Side AssociatedKing>
    void HalfKP<AssociatedKing>::append_active_indices(
        const Position& pos,
        Color perspective,
        IndexList* active) {

        Square ksq = orient(
            perspective,
            pos.square<KING>(
                AssociatedKing == Side::kFriend ? perspective : ~perspective));

        Bitboard bb = pos.pieces() & ~pos.pieces(KING);
        while (bb) {
            Square s = pop_lsb(&bb);
            active->push_back(make_index(perspective, s, pos.piece_on(s), ksq));
        }
    }

    // Get a list of indices for recently changed features
    template <Side AssociatedKing>
    void HalfKP<AssociatedKing>::append_changed_indices(
        const Position& pos,
        Color perspective,
        IndexList* removed,
        IndexList* added) {

        Square ksq = orient(
            perspective,
            pos.square<KING>(
                AssociatedKing == Side::kFriend ? perspective : ~perspective));

        const auto& dp = pos.state()->dirtyPiece;
        for (int i = 0; i < dp.dirty_num; ++i) {
            Piece pc = dp.piece[i];

            if (type_of(pc) == KING)
                continue;

            if (dp.from[i] != SQ_NONE)
                removed->push_back(make_index(perspective, dp.from[i], pc, ksq));

            if (dp.to[i] != SQ_NONE)
                added->push_back(make_index(perspective, dp.to[i], pc, ksq));
        }
    }

    template class HalfKP<Side::kFriend>;
    template class HalfKP<Side::kEnemy>;

}  // namespace Eval::NNUE::Features
