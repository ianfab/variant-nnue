﻿#include "gensfen.h"

#include "packed_sfen.h"
#include "multi_think.h"
#include "sfen_stream.h"

#include "misc.h"
#include "position.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "apiutil.h"

#include "extra/nnue_data_binpack_format.h"

#include "nnue/evaluate_nnue.h"
#include "nnue/evaluate_nnue_learner.h"

#include "syzygy/tbprobe.h"

#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <random>
#include <regex>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

using namespace std;

namespace Learner
{
    static bool write_out_draw_game_in_training_data_generation = true;
    static bool detect_draw_by_consecutive_low_score = true;
    static bool detect_draw_by_insufficient_mating_material = true;

    static SfenOutputType sfen_output_type = SfenOutputType::Bin;

    // Helper class for exporting Sfen
    struct SfenWriter
    {
        // Amount of sfens required to flush the buffer.
        static constexpr size_t SFEN_WRITE_SIZE = 5000;

        // Current status is output after
        // each (SFEN_WRITE_SIZE * STATUS_OUTPUT_PERIOD) sfens
        static constexpr uint64_t STATUS_OUTPUT_PERIOD = 40;

        // File name to write and number of threads to create
        SfenWriter(string filename_, int thread_num)
        {
            sfen_buffers_pool.reserve((size_t)thread_num * 10);
            sfen_buffers.resize(thread_num);

            output_file_stream = create_new_sfen_output(filename_, sfen_output_type);
            filename = filename_;

            finished = false;
        }

        ~SfenWriter()
        {
            finished = true;
            file_worker_thread.join();
            output_file_stream.reset();

#if !defined(NDEBUG)
            {
                // All buffers should be empty since file_worker_thread
                // should have written everything before exiting.
                for (const auto& p : sfen_buffers) { assert(p == nullptr); (void)p ; }
                assert(sfen_buffers_pool.empty());
            }
#endif
        }

        void write(size_t thread_id, const PackedSfenValue& psv)
        {
            // We have a buffer for each thread and add it there.
            // If the buffer overflows, write it to a file.

            // This buffer is prepared for each thread.
            auto& buf = sfen_buffers[thread_id];

            // Secure since there is no buf at the first time
            // and immediately after writing the thread buffer.
            if (!buf)
            {
                buf = std::make_unique<PSVector>();
                buf->reserve(SFEN_WRITE_SIZE);
            }

            // Buffer is exclusive to this thread.
            // There is no need for a critical section.
            buf->push_back(psv);

            if (buf->size() >= SFEN_WRITE_SIZE)
            {
                // If you load it in sfen_buffers_pool, the worker will do the rest.

                // Critical section since sfen_buffers_pool is shared among threads.
                std::unique_lock<std::mutex> lk(mutex);
                sfen_buffers_pool.emplace_back(std::move(buf));
            }
        }

        // Move what remains in the buffer for your thread to a buffer for writing to a file.
        void finalize(size_t thread_id)
        {
            std::unique_lock<std::mutex> lk(mutex);

            auto& buf = sfen_buffers[thread_id];

            // There is a case that buf==nullptr, so that check is necessary.
            if (buf && buf->size() != 0)
            {
                sfen_buffers_pool.emplace_back(std::move(buf));
            }
        }

        // Start the write_worker thread.
        void start_file_write_worker()
        {
            file_worker_thread = std::thread([&] { this->file_write_worker(); });
        }

        // Dedicated thread to write to file
        void file_write_worker()
        {
            auto startTime = now();

            auto output_status = [&]()
            {
                // Also output the current time to console.
                const auto nowTime = now();
                const TimePoint elapsed = nowTime - startTime + 1;

                sync_cout << endl
                    << sfen_write_count << " sfens, "
                    << sfen_write_count * 1000 / elapsed << " sfens/second, "
                    << "at " << now_string() << sync_endl;
            };

            while (!finished || sfen_buffers_pool.size())
            {
                vector<std::unique_ptr<PSVector>> buffers;
                {
                    std::unique_lock<std::mutex> lk(mutex);

                    // Atomically swap take the filled buffers and
                    // create a new buffer pool for threads to fill.
                    buffers = std::move(sfen_buffers_pool);
                    sfen_buffers_pool = std::vector<std::unique_ptr<PSVector>>();
                }

                if (!buffers.size())
                {
                    // Poor man's condition variable.
                    sleep(100);
                }
                else
                {
                    for (auto& buf : buffers)
                    {
                        output_file_stream->write(*buf);

                        sfen_write_count += buf->size();

                        // Add the processed number here, and if it exceeds save_every,
                        // change the file name and reset this counter.
                        sfen_write_count_current_file += buf->size();
                        if (sfen_write_count_current_file >= save_every)
                        {
                            sfen_write_count_current_file = 0;

                            // Sequential number attached to the file
                            int n = (int)(sfen_write_count / save_every);

                            // Rename the file and open it again.
                            // Add ios::app in consideration of overwriting.
                            // (Depending on the operation, it may not be necessary.)
                            string new_filename = filename + "_" + std::to_string(n);
                            output_file_stream = create_new_sfen_output(new_filename, sfen_output_type);
                            cout << endl << "output sfen file = " << new_filename << endl;
                        }

                        // Output '.' every time when writing a game record.
                        std::cout << ".";

                        // Output the number of phases processed
                        // every STATUS_OUTPUT_PERIOD times
                        // Finally, the remainder of the teacher phase
                        // of each thread is written out,
                        // so halfway numbers are displayed, but is it okay?
                        // If you overuse the threads to the maximum number
                        // of logical cores, the console will be clogged,
                        // so it may be beneficial to increase that value.
                        if ((++batch_counter % STATUS_OUTPUT_PERIOD) == 0)
                        {
                            output_status();
                        }
                    }
                }
            }

            // Output the status again after whole processing is done.
            output_status();
        }

        void set_save_interval(uint64_t v)
        {
            save_every = v;
        }

    private:

        std::unique_ptr<BasicSfenOutputStream> output_file_stream;

        // A new net is saved after every save_every sfens are processed.
        uint64_t save_every = std::numeric_limits<uint64_t>::max();

        // File name passed in the constructor
        std::string filename;

        // Thread to write to the file
        std::thread file_worker_thread;

        // Flag that all threads have finished
        atomic<bool> finished;

        // Counter for time stamp output
        uint64_t batch_counter = 0;

        // buffer before writing to file
        // sfen_buffers is the buffer for each thread
        // sfen_buffers_pool is a buffer for writing.
        // After loading the phase in the former buffer by SFEN_WRITE_SIZE,
        // transfer it to the latter.
        std::vector<std::unique_ptr<PSVector>> sfen_buffers;
        std::vector<std::unique_ptr<PSVector>> sfen_buffers_pool;

        // Mutex required to access sfen_buffers_pool
        std::mutex mutex;

        // Number of sfens written in total, and the
        // number of sfens written in the current file.
        uint64_t sfen_write_count = 0;
        uint64_t sfen_write_count_current_file = 0;
    };

    // -----------------------------------
    // worker that creates the game record (for each thread)
    // -----------------------------------

    // Class to generate sfen with multiple threads
    struct MultiThinkGenSfen : public MultiThink
    {
        // Hash to limit the export of identical sfens
        static constexpr uint64_t GENSFEN_HASH_SIZE = 64 * 1024 * 1024;
        // It must be 2**N because it will be used as the mask to calculate hash_index.
        static_assert((GENSFEN_HASH_SIZE& (GENSFEN_HASH_SIZE - 1)) == 0);

        MultiThinkGenSfen(int search_depth_min_, int search_depth_max_, SfenWriter& sw_, const std::string& seed) :
            MultiThink(seed),
            search_depth_min(search_depth_min_),
            search_depth_max(search_depth_max_),
            sfen_writer(sw_)
        {
            hash.resize(GENSFEN_HASH_SIZE);

            // Output seed to veryfy by the user if it's not identical by chance.
            std::cout << prng << std::endl;
        }

        void start_file_write_worker()
        {
            sfen_writer.start_file_write_worker();
        }

        void thread_worker(size_t thread_id) override;

        optional<int8_t> get_current_game_result(
            Position& pos,
            const vector<int>& move_hist_scores) const;

        vector<uint8_t> generate_random_move_flags();

        bool commit_psv(PSVector& a_psv, size_t thread_id, int8_t lastTurnIsWin);

        optional<Move> choose_random_move(
            Position& pos,
            std::vector<uint8_t>& random_move_flag,
            int ply,
            int& random_move_c);

        // Min and max depths for search during gensfen
        int search_depth_min;
        int search_depth_max;

        // Number of the nodes to be searched.
        // 0 represents no limits.
        uint64_t nodes;

        // Upper limit of evaluation value of generated situation
        int eval_limit;

        // minimum ply with random move
        // maximum ply with random move
        // Number of random moves in one station
        int random_move_minply;
        int random_move_maxply;
        int random_move_count;

        // Move kings with a probability of 1/N when randomly moving like Apery software.
        // When you move the king again, there is a 1/N chance that it will randomly moved
        // once in the opponent's turn.
        // Apery has N=2. Specifying 0 here disables this function.
        int random_move_like_apery;

        // For when using multi pv instead of random move.
        // random_multi_pv is the number of candidates for MultiPV.
        // When adopting the move of the candidate move, the difference
        // between the evaluation value of the move of the 1st place
        // and the evaluation value of the move of the Nth place is.
        // Must be in the range random_multi_pv_diff.
        // random_multi_pv_depth is the search depth for MultiPV.
        int random_multi_pv;
        int random_multi_pv_diff;
        int random_multi_pv_depth;

        // The minimum and maximum ply (number of steps from
        // the initial phase) of the sfens to write out.
        int write_minply;
        int write_maxply;

        // sfen exporter
        SfenWriter& sfen_writer;

        vector<Key> hash; // 64MB*sizeof(HASH_KEY) = 512MB
    };

    optional<int8_t> MultiThinkGenSfen::get_current_game_result(
        Position& pos,
        const vector<int>& move_hist_scores) const
    {
        // Variables for draw adjudication.
        // Todo: Make this as an option.

        // start the adjudication when ply reaches this value
        constexpr int adj_draw_ply = 80;

        // 4 move scores for each side have to be checked
        constexpr int adj_draw_cnt = 8;

        // move score in CP
        constexpr int adj_draw_score = 0;

        // For the time being, it will be treated as a
        // draw at the maximum number of steps to write.
        const int ply = move_hist_scores.size();

        std::function <int (Value)> sign = [](Value v) {
            return v > 0 ? 1 : v < 0 ? -1 : 0;
        };

        // has it reached the max length or is a draw
        Value v = VALUE_DRAW;
        if (ply >= write_maxply || pos.is_game_end(v))
        {
            return sign(v);
        }

        if(pos.this_thread()->rootMoves.empty())
        {
            // If there is no legal move
            return pos.checkers()
                ? sign(pos.checkmate_value()) /* mate */
                : sign(pos.stalemate_value()) /* stalemate */;
        }

        // Adjudicate game to a draw if the last 4 scores of each engine is 0.
        if (detect_draw_by_consecutive_low_score)
        {
            if (ply >= adj_draw_ply)
            {
                int num_cons_plies_within_draw_score = 0;
                bool is_adj_draw = false;

                for (auto it = move_hist_scores.rbegin();
                    it != move_hist_scores.rend(); ++it)
                {
                    if (abs(*it) <= adj_draw_score)
                    {
                        num_cons_plies_within_draw_score++;
                    }
                    else
                    {
                        // Draw scores must happen on consecutive plies
                        break;
                    }

                    if (num_cons_plies_within_draw_score >= adj_draw_cnt)
                    {
                        is_adj_draw = true;
                        break;
                    }
                }

                if (is_adj_draw)
                {
                    return 0;
                }
            }
        }

        // Draw by insufficient mating material
        if (detect_draw_by_insufficient_mating_material
            && hasInsufficientMaterial(WHITE, pos)
            && hasInsufficientMaterial(BLACK, pos))
        {
            return 0;
        }

        return nullopt;
    }

    // Write out the phases loaded in sfens to a file.
    // lastTurnIsWin: win/loss in the next phase after the final phase in sfens
    // 1 when winning. -1 when losing. Pass 0 for a draw.
    // Return value: true if the specified number of
    // sfens has already been reached and the process ends.
    bool MultiThinkGenSfen::commit_psv(PSVector& sfens, size_t thread_id, int8_t lastTurnIsWin)
    {
        if (!write_out_draw_game_in_training_data_generation && lastTurnIsWin == 0)
        {
            // We didn't write anything so why quit.
            return false;
        }

        int8_t is_win = lastTurnIsWin;

        // From the final stage (one step before) to the first stage, give information on the outcome of the game for each stage.
        // The phases stored in sfens are assumed to be continuous (in order).
        bool quit = false;
        int num_sfens_to_commit = 0;
        for (auto it = sfens.rbegin(); it != sfens.rend(); ++it)
        {
            // If is_win == 0 (draw), multiply by -1 and it will remain 0 (draw)
            is_win = -is_win;
            it->game_result = is_win;

            // See how many sfens were already written and get the next id.
            // Exit if requested number of sfens reached.
            auto now_loop_count = get_next_loop_count();
            if (now_loop_count == LOOP_COUNT_FINISHED)
            {
                quit = true;
                break;
            }

            ++num_sfens_to_commit;
        }

        // Write sfens in move order to make potential compression easier
        for (auto it = sfens.end() - num_sfens_to_commit; it != sfens.end(); ++it)
        {
            // Write out one sfen.
            sfen_writer.write(thread_id, *it);
        }

        return quit;
    }

    optional<Move> MultiThinkGenSfen::choose_random_move(
        Position& pos,
        std::vector<uint8_t>& random_move_flag,
        int ply,
        int& random_move_c)
    {
        optional<Move> random_move;

        // Randomly choose one from legal move
        if (
            // 1. Random move of random_move_count times from random_move_minply to random_move_maxply
            (random_move_minply != -1 && ply < (int)random_move_flag.size() && random_move_flag[ply]) ||
            // 2. A mode to perform random move of random_move_count times after leaving the startpos
            (random_move_minply == -1 && random_move_c < random_move_count))
        {
            ++random_move_c;

            // It's not a mate, so there should be one legal move...
            if (random_multi_pv == 0)
            {
                // Normal random move
                MoveList<LEGAL> list(pos);

                // I don't really know the goodness and badness of making this the Apery method.
                if (random_move_like_apery == 0
                    || prng.rand(random_move_like_apery) != 0)
                {
                    // Normally one move from legal move
                    random_move = list.at((size_t)prng.rand((uint64_t)list.size()));
                }
                else
                {
                    // if you can move the king, move the king
                    Move moves[8]; // Near 8
                    Move* p = &moves[0];
                    for (auto& m : list)
                    {
                        if (type_of(pos.moved_piece(m)) == KING)
                        {
                            *(p++) = m;
                        }
                    }

                    size_t n = p - &moves[0];
                    if (n != 0)
                    {
                        // move to move the king
                        random_move = moves[prng.rand(n)];

                        // In Apery method, at this time there is a 1/2 chance
                        // that the opponent will also move randomly
                        if (prng.rand(2) == 0)
                        {
                            // Is it a simple hack to add a "1" next to random_move_flag[ply]?
                            random_move_flag.insert(random_move_flag.begin() + ply + 1, 1, true);
                        }
                    }
                    else
                    {
                        // Normally one move from legal move
                        random_move = list.at((size_t)prng.rand((uint64_t)list.size()));
                    }
                }
            }
            else
            {
                Search::search(pos, random_multi_pv_depth, random_multi_pv);

                // Select one from the top N hands of root Moves
                auto& rm = pos.this_thread()->rootMoves;

                uint64_t s = min((uint64_t)rm.size(), (uint64_t)random_multi_pv);
                for (uint64_t i = 1; i < s; ++i)
                {
                    // The difference from the evaluation value of rm[0] must
                    // be within the range of random_multi_pv_diff.
                    // It can be assumed that rm[x].score is arranged in descending order.
                    if (rm[0].score > rm[i].score + random_multi_pv_diff)
                    {
                        s = i;
                        break;
                    }
                }

                random_move = rm[prng.rand(s)].pv[0];
            }
        }

        return random_move;
    }

    vector<uint8_t> MultiThinkGenSfen::generate_random_move_flags()
    {
        vector<uint8_t> random_move_flag;

        // Depending on random move selection parameters setup
        // the array of flags that indicates whether a random move
        // be taken at a given ply.

        // Make an array like a[0] = 0 ,a[1] = 1, ...
        // Fisher-Yates shuffle and take out the first N items.
        // Actually, I only want N pieces, so I only need
        // to shuffle the first N pieces with Fisher-Yates.

        vector<int> a;
        a.reserve((size_t)random_move_maxply);

        // random_move_minply ,random_move_maxply is specified by 1 origin,
        // Note that we are handling 0 origin here.
        for (int i = std::max(random_move_minply - 1, 0); i < random_move_maxply; ++i)
        {
            a.push_back(i);
        }

        // In case of Apery random move, insert() may be called random_move_count times.
        // Reserve only the size considering it.
        random_move_flag.resize((size_t)random_move_maxply + random_move_count);

        // A random move that exceeds the size() of a[] cannot be applied, so limit it.
        for (int i = 0; i < std::min(random_move_count, (int)a.size()); ++i)
        {
            swap(a[i], a[prng.rand((uint64_t)a.size() - i) + i]);
            random_move_flag[a[i]] = true;
        }

        return random_move_flag;
    }

    // thread_id = 0..Threads.size()-1
    void MultiThinkGenSfen::thread_worker(size_t thread_id)
    {
        // For the time being, it will be treated as a draw
        // at the maximum number of steps to write.
        // Maximum StateInfo + Search PV to advance to leaf buffer
        std::vector<StateInfo, AlignedAllocator<StateInfo>> states(
            write_maxply + MAX_PLY /* == search_depth_min + α */);

        StateInfo si;

        // end flag
        bool quit = false;

        // repeat until the specified number of times
        while (!quit)
        {
            // It is necessary to set a dependent thread for Position.
            // When parallelizing, Threads (since this is a vector<Thread*>,
            // Do the same for up to Threads[0]...Threads[thread_num-1].
            auto th = Threads[thread_id];

            auto& pos = th->rootPos;
            pos.set(variants.find(Options["UCI_Variant"])->second, variants.find(Options["UCI_Variant"])->second->startFen, false, &si, th);

            int resign_counter = 0;
            bool should_resign = prng.rand(10) > 1;
            // Vector for holding the sfens in the current simulated game.
            PSVector a_psv;
            a_psv.reserve(write_maxply + MAX_PLY);

            // Precomputed flags. Used internally by choose_random_move.
            vector<uint8_t> random_move_flag = generate_random_move_flags();

            // A counter that keeps track of the number of random moves
            // When random_move_minply == -1, random moves are
            // performed continuously, so use it at this time.
            // Used internally by choose_random_move.
            int actual_random_move_count = 0;

            // Save history of move scores for adjudication
            vector<int> move_hist_scores;

            auto flush_psv = [&](int8_t result) {
                quit = commit_psv(a_psv, thread_id, result);
            };

            for (int ply = 0; ; ++ply)
            {
                Move next_move = MOVE_NONE;

                // Current search depth
                const int depth = search_depth_min + (int)prng.rand(search_depth_max - search_depth_min + 1);

                // Starting search calls init_for_search
                auto [search_value, search_pv] = Search::search(pos, depth, 1, nodes);

                // This has to be performed after search because it needs to know
                // rootMoves which are filled in init_for_search.
                const auto result = get_current_game_result(pos, move_hist_scores);
                if (result.has_value())
                {
                    flush_psv(result.value());
                    break;
                }

                // Always adjudivate by eval limit.
                // Also because of this we don't have to check for TB/MATE scores
                if (abs(search_value) >= eval_limit)
                {
                    resign_counter++;
                    if ((should_resign && resign_counter >= 4) || abs(search_value) >= VALUE_KNOWN_WIN) {
                        flush_psv((search_value >= eval_limit) ? 1 : -1);
                        break;
                    }
                } else {
                    resign_counter = 0;
                }
                // Verification of a strange move
                if (search_pv.size() > 0
                    && (search_pv[0] == MOVE_NONE || search_pv[0] == MOVE_NULL))
                {
                    // (???)
                    // MOVE_WIN is checking if it is the declaration victory stage before this
                    // The declarative winning move should never come back here.
                    // Also, when MOVE_RESIGN, search_value is a one-stop score, which should be the minimum value of eval_limit (-31998)...
                    cout << "Error! : " << pos.fen() << next_move << search_value << endl;
                    break;
                }

                // Save the move score for adjudication.
                move_hist_scores.push_back(search_value);

                // Discard stuff before write_minply is reached
                // because it can harm training due to overfitting.
                // Initial positions would be too common.
                if (ply < write_minply - 1)
                {
                    a_psv.clear();
                    goto SKIP_SAVE;
                }

                // Look into the position hashtable to see if the same
                // position was seen before.
                // This is a good heuristic to exlude already seen
                // positions without many false positives.
                {
                    auto key = pos.key();
                    auto hash_index = (size_t)(key & (GENSFEN_HASH_SIZE - 1));
                    auto old_key = hash[hash_index];
                    if (key == old_key)
                    {
                        goto SKIP_SAVE;
                    }
                    else
                    {
                        // Replace with the current key.
                        hash[hash_index] = key;
                    }
                }

                // Pack the current position into a packed sfen and save it into the buffer.
                {
                    a_psv.emplace_back(PackedSfenValue());
                    auto& psv = a_psv.back();

                    // Here we only write the position data.
                    // Result is added after the whole game is done.
                    pos.sfen_pack(psv.sfen);

                    psv.score = search_value;

                    psv.gamePly = ply;

                    // Take out the first PV move. This should be present unless depth 0.
                    assert(search_pv.size() >= 1);
                    psv.move = search_pv[0];
                }

            SKIP_SAVE:;

                // For some reason, We could not get PV (hit the substitution table etc. and got stuck?)
                // so go to the next game. It's a rare case, so you can ignore it.
                if (search_pv.size() == 0)
                {
                    break;
                }

                // Update the next move according to best search result.
                next_move = search_pv[0];

                // Random move.
                auto random_move = choose_random_move(pos, random_move_flag, ply, actual_random_move_count);
                if (random_move.has_value())
                {
                    next_move = random_move.value();

                    // We don't have the whole game yet, but it ended,
                    // so the writing process ends and the next game starts.
                    if (!is_ok(next_move))
                    {
                        break;
                    }
                }

                // Do move.
                pos.do_move(next_move, states[ply]);

                if (pos.is_immediate_game_end())
                    break;

            } // for (int ply = 0; ; ++ply)

        } // while(!quit)

        sfen_writer.finalize(thread_id);
    }

    // -----------------------------------
    // Command to generate a game record (master thread)
    // -----------------------------------

    // Command to generate a game record
    void gen_sfen(Position&, istringstream& is)
    {
        // number of threads (given by USI setoption)
        uint32_t thread_num = (uint32_t)Options["Threads"];

        // Number of generated game records default = 8 billion phases (Ponanza specification)
        uint64_t loop_max = 8000000000UL;

        // Stop the generation when the evaluation value reaches this value.
        int eval_limit = 3000;

        // search depth
        int search_depth_min = 3;
        int search_depth_max = INT_MIN;

        // Number of nodes to be searched.
        uint64_t nodes = 0;

        // minimum ply, maximum ply and number of random moves
        int random_move_minply = 1;
        int random_move_maxply = 24;
        int random_move_count = 5;

        // A function to move the random move mainly like Apery
        // If this is set to 3, the ball will move with a probability of 1/3.
        int random_move_like_apery = 0;

        // If you search with multipv instead of random move and choose from among them randomly, set random_multi_pv = 1 or more.
        int random_multi_pv = 0;
        int random_multi_pv_diff = 32000;
        int random_multi_pv_depth = INT_MIN;

        // The minimum and maximum ply (number of steps from the initial phase) of the phase to write out.
        int write_minply = 16;
        int write_maxply = 400;

        // File name to write
        string output_file_name = "generated_kifu";

        string token;

        // Save to file in this unit.
        // File names are serialized like file_1.bin, file_2.bin.
        uint64_t save_every = UINT64_MAX;

        // Add a random number to the end of the file name.
        bool random_file_name = false;

        std::string sfen_format = "binpack";
        std::string seed;

        while (true)
        {
            token = "";
            is >> token;
            if (token == "")
                break;

            if (token == "depth")
                is >> search_depth_min;
            else if (token == "depth2")
                is >> search_depth_max;
            else if (token == "nodes")
                is >> nodes;
            else if (token == "loop")
                is >> loop_max;
            else if (token == "output_file_name")
                is >> output_file_name;
            else if (token == "eval_limit")
            {
                is >> eval_limit;
                // Limit the maximum to a one-stop score. (Otherwise you might not end the loop)
                eval_limit = std::min(eval_limit, (int)mate_in(2));
            }
            else if (token == "random_move_minply")
                is >> random_move_minply;
            else if (token == "random_move_maxply")
                is >> random_move_maxply;
            else if (token == "random_move_count")
                is >> random_move_count;
            else if (token == "random_move_like_apery")
                is >> random_move_like_apery;
            else if (token == "random_multi_pv")
                is >> random_multi_pv;
            else if (token == "random_multi_pv_diff")
                is >> random_multi_pv_diff;
            else if (token == "random_multi_pv_depth")
                is >> random_multi_pv_depth;
            else if (token == "write_minply")
                is >> write_minply;
            else if (token == "write_maxply")
                is >> write_maxply;
            else if (token == "save_every")
                is >> save_every;
            else if (token == "random_file_name")
                is >> random_file_name;
            // Accept also the old option name.
            else if (token == "use_draw_in_training_data_generation" || token == "write_out_draw_game_in_training_data_generation")
                is >> write_out_draw_game_in_training_data_generation;
            // Accept also the old option name.
            else if (token == "use_game_draw_adjudication" || token == "detect_draw_by_consecutive_low_score")
                is >> detect_draw_by_consecutive_low_score;
            else if (token == "detect_draw_by_insufficient_mating_material")
                is >> detect_draw_by_insufficient_mating_material;
            else if (token == "sfen_format")
                is >> sfen_format;
            else if (token == "seed")
                is >> seed;
            else if (token == "set_recommended_uci_options")
            {
                UCI::setoption("Contempt", "0");
                UCI::setoption("Skill Level", "20");
                UCI::setoption("UCI_Chess960", "false");
                UCI::setoption("UCI_AnalyseMode", "false");
                UCI::setoption("UCI_LimitStrength", "false");
                UCI::setoption("PruneAtShallowDepth", "false");
                UCI::setoption("EnableTranspositionTable", "true");
            }
            else
                cout << "Error! : Illegal token " << token << endl;
        }

        if (!sfen_format.empty())
        {
            if (sfen_format == "bin")
                sfen_output_type = SfenOutputType::Bin;
            else if (sfen_format == "binpack")
                sfen_output_type = SfenOutputType::Binpack;
            else
            {
                cout << "Unknown sfen format `" << sfen_format << "`. Using bin\n";
            }
        }

        // If search depth2 is not set, leave it the same as search depth.
        if (search_depth_max == INT_MIN)
            search_depth_max = search_depth_min;
        if (random_multi_pv_depth == INT_MIN)
            random_multi_pv_depth = search_depth_min;

        if (random_file_name)
        {
            // Give a random number to output_file_name at this point.
            // Do not use std::random_device().  Because it always the same integers on MinGW.
            PRNG r(seed);
            // Just in case, reassign the random numbers.
            for (int i = 0; i < 10; ++i)
                r.rand(1);
            auto to_hex = [](uint64_t u) {
                std::stringstream ss;
                ss << std::hex << u;
                return ss.str();
            };
            // I don't want to wear 64bit numbers by accident, so I'next_move going to make a 64bit number 2 just in case.
            output_file_name = output_file_name + "_" + to_hex(r.rand<uint64_t>()) + to_hex(r.rand<uint64_t>());
        }

        std::cout << "gensfen : " << endl
            << "  search_depth_min = " << search_depth_min << " to " << search_depth_max << endl
            << "  nodes = " << nodes << endl
            << "  loop_max = " << loop_max << endl
            << "  eval_limit = " << eval_limit << endl
            << "  thread_num (set by USI setoption) = " << thread_num << endl
            << "  random_move_minply     = " << random_move_minply << endl
            << "  random_move_maxply     = " << random_move_maxply << endl
            << "  random_move_count      = " << random_move_count << endl
            << "  random_move_like_apery = " << random_move_like_apery << endl
            << "  random_multi_pv        = " << random_multi_pv << endl
            << "  random_multi_pv_diff   = " << random_multi_pv_diff << endl
            << "  random_multi_pv_depth  = " << random_multi_pv_depth << endl
            << "  write_minply           = " << write_minply << endl
            << "  write_maxply           = " << write_maxply << endl
            << "  output_file_name       = " << output_file_name << endl
            << "  save_every             = " << save_every << endl
            << "  random_file_name       = " << random_file_name << endl
            << "  write_out_draw_game_in_training_data_generation = " << write_out_draw_game_in_training_data_generation << endl
            << "  detect_draw_by_consecutive_low_score = " << detect_draw_by_consecutive_low_score << endl
            << "  detect_draw_by_insufficient_mating_material = " << detect_draw_by_insufficient_mating_material << endl;

        // Show if the training data generator uses NNUE.
        Eval::NNUE::verify_eval_file_loaded();

        Threads.main()->ponder = false;

        // About Search::Limits
        // Be careful because this member variable is global and affects other threads.
        {
          auto& limits = Search::Limits;

          // Make the search equivalent to the "go infinite" command. (Because it is troublesome if time management is done)
          limits.infinite = true;

          // Since PV is an obstacle when displayed, erase it.
          limits.silent = true;

          // If you use this, it will be compared with the accumulated nodes of each thread. Therefore, do not use it.
          limits.nodes = 0;

          // depth is also processed by the one passed as an argument of Learner::search().
          limits.depth = 0;
        }

        // Create and execute threads as many as Options["Threads"].
        {
            SfenWriter sfen_writer(output_file_name, thread_num);
            sfen_writer.set_save_interval(save_every);

            MultiThinkGenSfen multi_think(search_depth_min, search_depth_max, sfen_writer, seed);
            multi_think.nodes = nodes;
            multi_think.set_loop_max(loop_max);
            multi_think.eval_limit = eval_limit;
            multi_think.random_move_minply = random_move_minply;
            multi_think.random_move_maxply = random_move_maxply;
            multi_think.random_move_count = random_move_count;
            multi_think.random_move_like_apery = random_move_like_apery;
            multi_think.random_multi_pv = random_multi_pv;
            multi_think.random_multi_pv_diff = random_multi_pv_diff;
            multi_think.random_multi_pv_depth = random_multi_pv_depth;
            multi_think.write_minply = write_minply;
            multi_think.write_maxply = write_maxply;
            multi_think.start_file_write_worker();
            multi_think.go_think();

            // Since we are joining with the destructor of SfenWriter, please give a message that it has finished after the join
            // Enclose this in a block because it should be displayed.
        }

        std::cout << "gensfen finished." << endl;
    }
}
