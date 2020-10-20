#ifndef _SFEN_PACKER_H_
#define _SFEN_PACKER_H_

#include "types.h"

#include "learn/packed_sfen.h"

#include <cstdint>

class Position;
struct StateInfo;
class Thread;

namespace Learner {

    int set_from_packed_sfen(Position& pos, const PackedSfen& sfen, StateInfo* si, Thread* th);
    PackedSfen sfen_pack(Position& pos);
}

#endif