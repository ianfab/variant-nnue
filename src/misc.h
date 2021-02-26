/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2021 The Stockfish developers (see AUTHORS file)

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

#ifndef MISC_H_INCLUDED
#define MISC_H_INCLUDED

#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>
#include <utility>
#include <cmath>
#include <cctype>
#include <sstream>
#include <cstdint>

#include "types.h"

const std::string engine_info(bool to_uci = false, bool to_xboard = false);
const std::string compiler_info();
void prefetch(void* addr);
void start_logger(const std::string& fname);
void* std_aligned_alloc(size_t alignment, size_t size);
void std_aligned_free(void* ptr);
void* aligned_large_pages_alloc(size_t size); // memory aligned by page size, min alignment: 4096 bytes
void aligned_large_pages_free(void* mem); // nop if mem == nullptr

void dbg_hit_on(bool b);
void dbg_hit_on(bool c, bool b);
void dbg_mean_of(int v);
void dbg_print();

typedef std::chrono::milliseconds::rep TimePoint; // A value in milliseconds
static_assert(sizeof(TimePoint) == sizeof(int64_t), "TimePoint should be 64 bits");
inline TimePoint now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>
        (std::chrono::steady_clock::now().time_since_epoch()).count();
}

template<class Entry, int Size>
struct HashTable {
  Entry* operator[](Key key) { return &table[(uint32_t)key & (Size - 1)]; }

private:
  std::vector<Entry> table = std::vector<Entry>(Size); // Allocate on the heap
};


enum SyncCout { IO_LOCK, IO_UNLOCK };
std::ostream& operator<<(std::ostream&, SyncCout);

#define sync_cout std::cout << IO_LOCK
#define sync_endl std::endl << IO_UNLOCK

// `ptr` must point to an array of size at least
// `sizeof(T) * N + alignment` bytes, where `N` is the
// number of elements in the array.
template <uintptr_t Alignment, typename T>
T* align_ptr_up(T* ptr)
{
  static_assert(alignof(T) < Alignment);

  const uintptr_t ptrint = reinterpret_cast<uintptr_t>(reinterpret_cast<char*>(ptr));
  return reinterpret_cast<T*>(reinterpret_cast<char*>((ptrint + (Alignment - 1)) / Alignment * Alignment));
}

/// xorshift64star Pseudo-Random Number Generator
/// This class is based on original code written and dedicated
/// to the public domain by Sebastiano Vigna (2014).
/// It has the following characteristics:
///
///  -  Outputs 64-bit numbers
///  -  Passes Dieharder and SmallCrush test batteries
///  -  Does not require warm-up, no zeroland to escape
///  -  Internal state is a single 64-bit integer
///  -  Period is 2^64 - 1
///  -  Speed: 1.60 ns/call (Core i7 @3.40GHz)
///
/// For further analysis see
///   <http://vigna.di.unimi.it/ftp/papers/xorshift.pdf>

static uint64_t string_hash(const std::string& str)
{
  uint64_t h = 525201411107845655ull;

  for (auto c : str) {
    h ^= static_cast<uint64_t>(c);
    h *= 0x5bd1e9955bd1e995ull;
    h ^= h >> 47;
  }

  return h;
}

class PRNG {

  uint64_t s;

  uint64_t rand64() {

    s ^= s >> 12, s ^= s << 25, s ^= s >> 27;
    return s * 2685821657736338717LL;
  }

public:
  PRNG() { set_seed_from_time(); }
  PRNG(uint64_t seed) : s(seed) { assert(seed); }
  PRNG(const std::string& seed) { set_seed(seed); }

  template<typename T> T rand() { return T(rand64()); }

  /// Special generator used to fast init magic numbers.
  /// Output values only have 1/8th of their bits set on average.
  template<typename T> T sparse_rand()
  { return T(rand64() & rand64() & rand64()); }
  // Returns a random number from 0 to n-1. (Not uniform distribution, but this is enough in reality)
  uint64_t rand(uint64_t n) { return rand<uint64_t>() % n; }

  // Return the random seed used internally.
  uint64_t get_seed() const { return s; }

  void set_seed(uint64_t seed) { s = seed; }

  void set_seed_from_time()
  {
      set_seed(std::chrono::system_clock::now().time_since_epoch().count());
  }

  void set_seed(const std::string& str)
  {
    if (str.empty())
    {
      set_seed_from_time();
    }
    else if (std::all_of(str.begin(), str.end(), [](char c) { return std::isdigit(c);} )) {
      set_seed(std::stoull(str));
    }
    else
    {
      set_seed(string_hash(str));
    }
  }
};

// Display a random seed. (For debugging)
inline std::ostream& operator<<(std::ostream& os, PRNG& prng)
{
  os << "PRNG::seed = " << std::hex << prng.get_seed() << std::dec;
  return os;
}

inline uint64_t mul_hi64(uint64_t a, uint64_t b) {
#if defined(__GNUC__) && defined(IS_64BIT)
    __extension__ typedef unsigned __int128 uint128;
    return ((uint128)a * (uint128)b) >> 64;
#else
    uint64_t aL = (uint32_t)a, aH = a >> 32;
    uint64_t bL = (uint32_t)b, bH = b >> 32;
    uint64_t c1 = (aL * bL) >> 32;
    uint64_t c2 = aH * bL + c1;
    uint64_t c3 = aL * bH + (uint32_t)c2;
    return aH * bH + (c2 >> 32) + (c3 >> 32);
#endif
}

/// Under Windows it is not possible for a process to run on more than one
/// logical processor group. This usually means to be limited to use max 64
/// cores. To overcome this, some special platform specific API should be
/// called to set group affinity for each thread. Original code from Texel by
/// Peter Österlund.

namespace WinProcGroup {
  void bindThisThread(size_t idx);
}
// sleep for the specified number of milliseconds.
extern void sleep(int ms);

// Returns a string that represents the current time. (Used for log output when learning evaluation function)
std::string now_string();

// When compiled with gcc/clang such as msys2, Windows Subsystem for Linux,
// In C++ std::ifstream, ::read() is a wrapper for that because it is not possible to read and write files larger than 2GB in one shot.
//
// callback_func of the argument of read_file_to_memory() uses the file size as an argument when the file can be opened
// It will be called back, so if you allocate a buffer and pass a function that returns the first pointer, it will be read there.
// These functions return non-zero on error, such as when the file cannot be found.
//
// Also, if the buffer cannot be allocated in the callback function or if the file size is different from the expected file size,
// Return nullptr. At this time, read_file_to_memory() interrupts reading and returns with an error.

std::uint64_t get_file_size(std::fstream& fs);
int read_file_to_memory(std::string filename, std::function<void* (uint64_t)> callback_func);
int write_memory_to_file(std::string filename, void* ptr, uint64_t size);

// --------------------
// async version of PRNG
// --------------------

// async version of PRNG
struct AsyncPRNG
{
  AsyncPRNG() : prng() { }
  AsyncPRNG(uint64_t seed) : prng(seed) { assert(seed); }
  AsyncPRNG(const std::string& seed) : prng(seed) { }
  // [ASYNC] Extract one random number.
  template<typename T> T rand() {
    std::unique_lock<std::mutex> lk(mutex);
    return prng.rand<T>();
  }

  // [ASYNC] Returns a random number from 0 to n-1. (Not uniform distribution, but this is enough in reality)
  uint64_t rand(uint64_t n) {
    std::unique_lock<std::mutex> lk(mutex);
    return prng.rand(n);
  }

  // Return the random seed used internally.
  uint64_t get_seed() const { return prng.get_seed(); }

protected:
  std::mutex mutex;
  PRNG prng;
};

// Display a random seed. (For debugging)
inline std::ostream& operator<<(std::ostream& os, AsyncPRNG& prng)
{
  os << "AsyncPRNG::seed = " << std::hex << prng.get_seed() << std::dec;
  return os;
}

// --------------------
//       Math
// --------------------

// Mathematical function used for progress calculation and learning
namespace Math {
    inline double sigmoid(double x)
    {
        return 1.0 / (1.0 + std::exp(-x));
    }

    inline double dsigmoid(double x)
    {
        // Sigmoid function
        // f(x) = 1/(1+exp(-x))
        // the first derivative is
        // f'(x) = df/dx = f(x)・{ 1-f(x)}
        // becomes

        return sigmoid(x) * (1.0 - sigmoid(x));
    }

	// Clip v so that it fits between [lo,hi].
	// * In Stockfish, this function is written in bitboard.h.
	template<class T> constexpr const T& clamp(const T& v, const T& lo, const T& hi) {
		return v < lo ? lo : v > hi ? hi : v;
	}
}

namespace Algo {
    // Fisher-Yates
    template <typename Rng, typename T>
    void shuffle(std::vector<T>& buf, Rng&& prng)
    {
        const auto size = buf.size();
        for (uint64_t i = 0; i < size; ++i)
            std::swap(buf[i], buf[prng.rand(size - i) + i]);
    }

    // split the string
    inline std::vector<std::string> split(const std::string& input, char delimiter) {
        std::istringstream stream(input);
        std::string field;
        std::vector<std::string> fields;

        while (std::getline(stream, field, delimiter)) {
            fields.push_back(field);
        }

        return fields;
    }
}

// --------------------
//       Path
// --------------------

// Something like Path class in C#. File name manipulation.
// Match with the C# method name.
struct Path
{
	// Combine the path name and file name and return it.
	// If the folder name is not an empty string, append it if there is no'/' or'\\' at the end.
	static std::string combine(const std::string& folder, const std::string& filename)
	{
		if (folder.length() >= 1 && *folder.rbegin() != '/' && *folder.rbegin() != '\\')
			return folder + "/" + filename;

		return folder + filename;
	}

	// Get the file name part (excluding the folder name) from the full path expression.
	static std::string get_file_name(const std::string& path)
	{
		// I don't know which "\" or "/" is used.
		auto path_index1 = path.find_last_of("\\") + 1;
		auto path_index2 = path.find_last_of("/") + 1;
		auto path_index = std::max(path_index1, path_index2);

		return path.substr(path_index);
	}
};

// It is ignored when new even though alignas is specified & because it is ignored when the STL container allocates memory,
// A custom allocator used for that.
template <typename T>
class AlignedAllocator {
public:
  using value_type = T;

  AlignedAllocator() {}
  AlignedAllocator(const AlignedAllocator&) {}
  AlignedAllocator(AlignedAllocator&&) {}

  template <typename U> AlignedAllocator(const AlignedAllocator<U>&) {}

  T* allocate(std::size_t n) { return (T*)std_aligned_alloc(alignof(T), n * sizeof(T)); }
  void deallocate(T* p, std::size_t ) { std_aligned_free(p); }
};

// --------------------
//  Dependency Wrapper
// --------------------

namespace Dependency
{
  // In the Linux environment, if you getline() the text file is'\r\n'
  // Since'\r' remains at the end, write a wrapper to remove this'\r'.
  // So when calling getline() on fstream,
  // just write getline() instead of std::getline() and use this function.
  extern bool getline(std::ifstream& fs, std::string& s);
}

namespace CommandLine {
  void init(int argc, char* argv[]);

  extern std::string binaryDirectory;  // path of the executable directory
  extern std::string workingDirectory; // path of the working directory
}

#endif // #ifndef MISC_H_INCLUDED
