﻿// Learning routines:
//
// 1) Automatic generation of game records in .bin format
// → "gensfen" command
//
// 2) Learning evaluation function parameters from the generated .bin files
// → "learn" command
//
// → Shuffle in the teacher phase is also an extension of this command.
// Example) "learn shuffle"
//
// 3) Automatic generation of fixed traces
// → "makebook think" command
// → implemented in extra/book/book.cpp
//
// 4) Post-station automatic review mode
// → I will not be involved in the engine because it is a problem that the GUI should assist.
// etc..

#include "learn.h"

#include "convert.h"
#include "multi_think.h"
#include "sfen_stream.h"

#include "misc.h"
#include "position.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "search.h"
#include "timeman.h"

#include "nnue/evaluate_nnue.h"
#include "nnue/evaluate_nnue_learner.h"

#include "syzygy/tbprobe.h"

#include <chrono>
#include <climits>
#include <cmath>    // std::exp(),std::pow(),std::log()
#include <cstring>  // memcpy()
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

#if defined (_OPENMP)
#include <omp.h>
#endif

extern double global_learning_rate;

using namespace std;

template <typename T>
T operator +=(std::atomic<T>& x, const T rhs)
{
    T old = x.load(std::memory_order_consume);

    // It is allowed that the value is rewritten from other thread at this timing.
    // The idea that the value is not destroyed is good.
    T desired = old + rhs;
    while (!x.compare_exchange_weak(old, desired, std::memory_order_release, std::memory_order_consume))
        desired = old + rhs;
    return desired;
}
template <typename T>
T operator -= (std::atomic<T>& x, const T rhs) { return x += -rhs; }

namespace Learner
{
    static bool use_draw_games_in_training = true;
    static bool use_draw_games_in_validation = true;
    static bool skip_duplicated_positions_in_training = true;

    static double winning_probability_coefficient = 1.0 / PawnValueEg / 4.0 * std::log(10.0);

    // Score scale factors. ex) If we set src_score_min_value = 0.0,
    // src_score_max_value = 1.0, dest_score_min_value = 0.0,
    // dest_score_max_value = 10000.0, [0.0, 1.0] will be scaled to [0, 10000].
    static double src_score_min_value = 0.0;
    static double src_score_max_value = 1.0;
    static double dest_score_min_value = 0.0;
    static double dest_score_max_value = 1.0;

    // Using stockfish's WDL with win rate model instead of sigmoid
    static bool use_wdl = false;

    // A function that converts the evaluation value to the winning rate [0,1]
    double winning_percentage(double value)
    {
        // 1/(1+10^(-Eval/4))
        // = 1/(1+e^(-Eval/4*ln(10))
        // = sigmoid(Eval/4*ln(10))
        return Math::sigmoid(value * winning_probability_coefficient);
    }

    // A function that converts the evaluation value to the winning rate [0,1]
    double winning_percentage_wdl(double value, int ply)
    {
        constexpr double wdl_total = 1000.0;
        constexpr double draw_score = 0.5;

        const double wdl_w = UCI::win_rate_model_double(value, ply);
        const double wdl_l = UCI::win_rate_model_double(-value, ply);
        const double wdl_d = wdl_total - wdl_w - wdl_l;

        return (wdl_w + wdl_d * draw_score) / wdl_total;
    }

    // A function that converts the evaluation value to the winning rate [0,1]
    double winning_percentage(double value, int ply)
    {
        if (use_wdl)
        {
            return winning_percentage_wdl(value, ply);
        }
        else
        {
            return winning_percentage(value);
        }
    }

    double calc_cross_entropy_of_winning_percentage(
        double deep_win_rate,
        double shallow_eval,
        int ply)
    {
        const double p = deep_win_rate;
        const double q = winning_percentage(shallow_eval, ply);
        return -p * std::log(q) - (1.0 - p) * std::log(1.0 - q);
    }

    double calc_d_cross_entropy_of_winning_percentage(
        double deep_win_rate,
        double shallow_eval,
        int ply)
    {
        constexpr double epsilon = 0.000001;

        const double y1 = calc_cross_entropy_of_winning_percentage(
            deep_win_rate, shallow_eval, ply);

        const double y2 = calc_cross_entropy_of_winning_percentage(
            deep_win_rate, shallow_eval + epsilon, ply);

        // Divide by the winning_probability_coefficient to
        // match scale with the sigmoidal win rate
        return ((y2 - y1) / epsilon) / winning_probability_coefficient;
    }

    // A constant used in elmo (WCSC27). Adjustment required.
    // Since elmo does not internally divide the expression, the value is different.
    // You can set this value with the learn command.
    // 0.33 is equivalent to the constant (0.5) used in elmo (WCSC27)
    double ELMO_LAMBDA = 0.33;
    double ELMO_LAMBDA2 = 0.33;
    double ELMO_LAMBDA_LIMIT = 32000;

    // Training Formula · Issue #71 · nodchip/Stockfish https://github.com/nodchip/Stockfish/issues/71
    double get_scaled_signal(double signal)
    {
        double scaled_signal = signal;

        // Normalize to [0.0, 1.0].
        scaled_signal =
            (scaled_signal - src_score_min_value)
            / (src_score_max_value - src_score_min_value);

        // Scale to [dest_score_min_value, dest_score_max_value].
        scaled_signal =
            scaled_signal * (dest_score_max_value - dest_score_min_value)
            + dest_score_min_value;

        return scaled_signal;
    }

    // Teacher winning probability.
    double calculate_p(double teacher_signal, int ply)
    {
        const double scaled_teacher_signal = get_scaled_signal(teacher_signal);
        return winning_percentage(scaled_teacher_signal, ply);
    }

    double calculate_lambda(double teacher_signal)
    {
        // If the evaluation value in deep search exceeds ELMO_LAMBDA_LIMIT
        // then apply ELMO_LAMBDA2 instead of ELMO_LAMBDA.
        const double lambda =
            (std::abs(teacher_signal) >= ELMO_LAMBDA_LIMIT)
            ? ELMO_LAMBDA2
            : ELMO_LAMBDA;

        return lambda;
    }

    double calculate_t(int game_result)
    {
        // Use 1 as the correction term if the expected win rate is 1,
        // 0 if you lose, and 0.5 if you draw.
        // game_result = 1,0,-1 so add 1 and divide by 2.
        const double t = double(game_result + 1) * 0.5;

        return t;
    }

    double calc_grad(Value teacher_signal, Value shallow, const PackedSfenValue& psv)
    {
        // elmo (WCSC27) method
        // Correct with the actual game wins and losses.
        const double q = winning_percentage(shallow, psv.gamePly);
        const double p = calculate_p(teacher_signal, psv.gamePly);
        const double t = calculate_t(psv.game_result);
        const double lambda = calculate_lambda(teacher_signal);

        double grad;
        if (use_wdl)
        {
            const double dce_p = calc_d_cross_entropy_of_winning_percentage(p, shallow, psv.gamePly);
            const double dce_t = calc_d_cross_entropy_of_winning_percentage(t, shallow, psv.gamePly);
            grad = lambda * dce_p + (1.0 - lambda) * dce_t;
        }
        else
        {
            // Use the actual win rate as a correction term.
            // This is the idea of ​​elmo (WCSC27), modern O-parts.
            grad = lambda * (q - p) + (1.0 - lambda) * (q - t);
        }

        return grad;
    }

    // Calculate cross entropy during learning
    // The individual cross entropy of the win/loss term and win
    // rate term of the elmo expression is returned
    // to the arguments cross_entropy_eval and cross_entropy_win.
    void calc_cross_entropy(
        Value teacher_signal,
        Value shallow,
        const PackedSfenValue& psv,
        double& cross_entropy_eval,
        double& cross_entropy_win,
        double& cross_entropy,
        double& entropy_eval,
        double& entropy_win,
        double& entropy)
    {
        // Teacher winning probability.
        const double q = winning_percentage(shallow, psv.gamePly);
        const double p = calculate_p(teacher_signal, psv.gamePly);
        const double t = calculate_t(psv.game_result);
        const double lambda = calculate_lambda(teacher_signal);

        constexpr double epsilon = 0.000001;

        const double m = (1.0 - lambda) * t + lambda * p;

        cross_entropy_eval =
            (-p * std::log(q + epsilon) - (1.0 - p) * std::log(1.0 - q + epsilon));
        cross_entropy_win =
            (-t * std::log(q + epsilon) - (1.0 - t) * std::log(1.0 - q + epsilon));
        entropy_eval =
            (-p * std::log(p + epsilon) - (1.0 - p) * std::log(1.0 - p + epsilon));
        entropy_win =
            (-t * std::log(t + epsilon) - (1.0 - t) * std::log(1.0 - t + epsilon));

        cross_entropy =
            (-m * std::log(q + epsilon) - (1.0 - m) * std::log(1.0 - q + epsilon));
        entropy =
            (-m * std::log(m + epsilon) - (1.0 - m) * std::log(1.0 - m + epsilon));
    }

    // Other objective functions may be considered in the future...
    double calc_grad(Value shallow, const PackedSfenValue& psv)
    {
        return calc_grad((Value)psv.score, shallow, psv);
    }

    // Sfen reader
    struct SfenReader
    {
        // Number of phases used for calculation such as mse
        // mini-batch size = 1M is standard, so 0.2% of that should be negligible in terms of time.
        // Since search() is performed with depth = 1 in calculation of
        // move match rate, simple comparison is not possible...
        static constexpr uint64_t sfen_for_mse_size = 2000;

        // Number of phases buffered by each thread 0.1M phases. 4M phase at 40HT
        static constexpr size_t THREAD_BUFFER_SIZE = 10 * 1000;

        // Buffer for reading files (If this is made larger,
        // the shuffle becomes larger and the phases may vary.
        // If it is too large, the memory consumption will increase.
        // SFEN_READ_SIZE is a multiple of THREAD_BUFFER_SIZE.
        static constexpr const size_t SFEN_READ_SIZE = LEARN_SFEN_READ_SIZE;

        // hash to limit the reading of the same situation
        // Is there too many 64 million phases? Or Not really..
        // It must be 2**N because it will be used as the mask to calculate hash_index.
        static constexpr uint64_t READ_SFEN_HASH_SIZE = 64 * 1024 * 1024;

        // Do not use std::random_device().
        // Because it always the same integers on MinGW.
        SfenReader(int thread_num, const std::string& seed) :
            prng(seed)
        {
            packed_sfens.resize(thread_num);
            total_read = 0;
            total_done = 0;
            last_done = 0;
            next_update_weights = 0;
            save_count = 0;
            end_of_files = false;
            no_shuffle = false;
            stop_flag = false;

            hash.resize(READ_SFEN_HASH_SIZE);
        }

        ~SfenReader()
        {
            if (file_worker_thread.joinable())
                file_worker_thread.join();
        }

        // Load the phase for calculation such as mse.
        void read_for_mse()
        {
            auto th = Threads.main();
            Position& pos = th->rootPos;
            for (uint64_t i = 0; i < sfen_for_mse_size; ++i)
            {
                PackedSfenValue ps;
                if (!read_to_thread_buffer(0, ps))
                {
                    cout << "Error! read packed sfen , failed." << endl;
                    break;
                }

                sfen_for_mse.push_back(ps);

                // Get the hash key.
                StateInfo si;
                pos.set_from_packed_sfen(ps.sfen, &si, th);
                sfen_for_mse_hash.insert(pos.key());
            }
        }

        void read_validation_set(const string& file_name, int eval_limit)
        {
            auto input = open_sfen_input_file(file_name);

            while(!input->eof())
            {
                std::optional<PackedSfenValue> p_opt = input->next();
                if (p_opt.has_value())
                {
                    auto& p = *p_opt;

                    if (eval_limit < abs(p.score))
                        continue;

                    if (!use_draw_games_in_validation && p.game_result == 0)
                        continue;

                    sfen_for_mse.push_back(p);
                }
                else
                {
                    break;
                }
            }
        }

        // [ASYNC] Thread returns one aspect. Otherwise returns false.
        bool read_to_thread_buffer(size_t thread_id, PackedSfenValue& ps)
        {
            // If there are any positions left in the thread buffer
            // then retrieve one and return it.
            auto& thread_ps = packed_sfens[thread_id];

            // Fill the read buffer if there is no remaining buffer,
            // but if it doesn't even exist, finish.
            // If the buffer is empty, fill it.
            if ((thread_ps == nullptr || thread_ps->empty())
                && !read_to_thread_buffer_impl(thread_id))
                return false;

            // read_to_thread_buffer_impl() returned true,
            // Since the filling of the thread buffer with the
            // phase has been completed successfully
            // thread_ps->rbegin() is alive.

            ps = thread_ps->back();
            thread_ps->pop_back();

            // If you've run out of buffers, call delete yourself to free this buffer.
            if (thread_ps->empty())
            {
                thread_ps.reset();
            }

            return true;
        }

        // [ASYNC] Read some aspects into thread buffer.
        bool read_to_thread_buffer_impl(size_t thread_id)
        {
            while (true)
            {
                {
                    std::unique_lock<std::mutex> lk(mutex);
                    // If you can fill from the file buffer, that's fine.
                    if (packed_sfens_pool.size() != 0)
                    {
                        // It seems that filling is possible, so fill and finish.

                        packed_sfens[thread_id] = std::move(packed_sfens_pool.front());
                        packed_sfens_pool.pop_front();

                        total_read += THREAD_BUFFER_SIZE;

                        return true;
                    }
                }

                // The file to read is already gone. No more use.
                if (end_of_files)
                    return false;

                // Waiting for file worker to fill packed_sfens_pool.
                // The mutex isn't locked, so it should fill up soon.
                // Poor man's condition variable.
                sleep(1);
            }

        }

        // Start a thread that loads the phase file in the background.
        void start_file_read_worker()
        {
            file_worker_thread = std::thread([&] {
                this->file_read_worker();
                });
        }

        void file_read_worker()
        {
            auto open_next_file = [&]() {
                // no more
                for(;;)
                {
                    sfen_input_stream.reset();

                    if (filenames.empty())
                        return false;

                    // Get the next file name.
                    string filename = filenames.back();
                    filenames.pop_back();

                    sfen_input_stream = open_sfen_input_file(filename);
                    cout << "open filename = " << filename << endl;

                    // in case the file is empty or was deleted.
                    if (!sfen_input_stream->eof())
                        return true;
                }
            };

            if (sfen_input_stream == nullptr && !open_next_file())
            {
                cout << "..end of files." << endl;
                end_of_files = true;
                return;
            }

            while (true)
            {
                // Wait for the buffer to run out.
                // This size() is read only, so you don't need to lock it.
                while (!stop_flag && packed_sfens_pool.size() >= SFEN_READ_SIZE / THREAD_BUFFER_SIZE)
                    sleep(100);

                if (stop_flag)
                    return;

                PSVector sfens;
                sfens.reserve(SFEN_READ_SIZE);

                // Read from the file into the file buffer.
                while (sfens.size() < SFEN_READ_SIZE)
                {
                    std::optional<PackedSfenValue> p = sfen_input_stream->next();
                    if (p.has_value())
                    {
                        sfens.push_back(*p);
                    }
                    else if(!open_next_file())
                    {
                        // There was no next file. Abort.
                        cout << "..end of files." << endl;
                        end_of_files = true;
                        return;
                    }
                }

                // Shuffle the read phase data.
                if (!no_shuffle)
                {
                    Algo::shuffle(sfens, prng);
                }

                // Divide this by THREAD_BUFFER_SIZE. There should be size pieces.
                // SFEN_READ_SIZE shall be a multiple of THREAD_BUFFER_SIZE.
                assert((SFEN_READ_SIZE % THREAD_BUFFER_SIZE) == 0);

                auto size = size_t(SFEN_READ_SIZE / THREAD_BUFFER_SIZE);
                std::vector<std::unique_ptr<PSVector>> buffers;
                buffers.reserve(size);

                for (size_t i = 0; i < size; ++i)
                {
                    // Delete this pointer on the receiving side.
                    auto buf = std::make_unique<PSVector>();
                    buf->resize(THREAD_BUFFER_SIZE);
                    memcpy(
                        buf->data(),
                        &sfens[i * THREAD_BUFFER_SIZE],
                        sizeof(PackedSfenValue) * THREAD_BUFFER_SIZE);

                    buffers.emplace_back(std::move(buf));
                }

                {
                    std::unique_lock<std::mutex> lk(mutex);

                    // The mutex lock is required because the%
                    // contents of packed_sfens_pool are changed.

                    for (auto& buf : buffers)
                        packed_sfens_pool.emplace_back(std::move(buf));
                }
            }
        }

        // Determine if it is a phase for calculating rmse.
        // (The computational aspects of rmse should not be used for learning.)
        bool is_for_rmse(Key key) const
        {
            return sfen_for_mse_hash.count(key) != 0;
        }

        // sfen files
        vector<string> filenames;

        // number of phases read (file to memory buffer)
        atomic<uint64_t> total_read;

        // number of processed phases
        atomic<uint64_t> total_done;

        // number of cases processed so far
        uint64_t last_done;

        // If total_read exceeds this value, update_weights() and calculate mse.
        std::atomic<uint64_t> next_update_weights;

        uint64_t save_count;

        // Do not shuffle when reading the phase.
        bool no_shuffle;

        std::atomic<bool> stop_flag;

        vector<Key> hash;

        // test phase for mse calculation
        PSVector sfen_for_mse;

    protected:

        // worker thread reading file in background
        std::thread file_worker_thread;

        // Random number to shuffle when reading the phase
        PRNG prng;

        // Did you read the files and reached the end?
        atomic<bool> end_of_files;

        // handle of sfen file
        std::unique_ptr<BasicSfenInputStream> sfen_input_stream;

        // sfen for each thread
        // (When the thread is used up, the thread should call delete to release it.)
        std::vector<std::unique_ptr<PSVector>> packed_sfens;

        // Mutex when accessing packed_sfens_pool
        std::mutex mutex;

        // pool of sfen. The worker thread read from the file is added here.
        // Each worker thread fills its own packed_sfens[thread_id] from here.
        // * Lock and access the mutex.
        std::list<std::unique_ptr<PSVector>> packed_sfens_pool;

        // Hold the hash key so that the mse calculation phase is not used for learning.
        std::unordered_set<Key> sfen_for_mse_hash;
    };

    // Class to generate sfen with multiple threads
    struct LearnerThink : public MultiThink
    {
        LearnerThink(SfenReader& sr_, const std::string& seed) :
            MultiThink(seed),
            sr(sr_),
            stop_flag(false),
            save_only_once(false)
        {
            learn_sum_cross_entropy_eval = 0.0;
            learn_sum_cross_entropy_win = 0.0;
            learn_sum_cross_entropy = 0.0;
            learn_sum_entropy_eval = 0.0;
            learn_sum_entropy_win = 0.0;
            learn_sum_entropy = 0.0;

            newbob_decay = 1.0;
            newbob_num_trials = 2;
            auto_lr_drop = 0;
            last_lr_drop = 0;
            best_loss = std::numeric_limits<double>::infinity();
            latest_loss_sum = 0.0;
            latest_loss_count = 0;
        }

        virtual void thread_worker(size_t thread_id);

        // Start a thread that loads the phase file in the background.
        void start_file_read_worker()
        {
            sr.start_file_read_worker();
        }

        Value get_shallow_value(Position& task_pos);

        // save merit function parameters to a file
        bool save(bool is_final = false);

        // sfen reader
        SfenReader& sr;

        // Learning iteration counter
        uint64_t epoch = 0;

        // Mini batch size size. Be sure to set it on the side that uses this class.
        uint64_t mini_batch_size = LEARN_MINI_BATCH_SIZE;

        std::atomic<bool> stop_flag;

        // Option to exclude early stage from learning
        int reduction_gameply;

        // If the absolute value of the evaluation value of the deep search
        // of the teacher phase exceeds this value, discard the teacher phase.
        int eval_limit;

        // Flag whether to dig a folder each time the evaluation function is saved.
        // If true, do not dig the folder.
        bool save_only_once;

        // --- loss calculation

        // For calculation of learning data loss
        atomic<double> learn_sum_cross_entropy_eval;
        atomic<double> learn_sum_cross_entropy_win;
        atomic<double> learn_sum_cross_entropy;
        atomic<double> learn_sum_entropy_eval;
        atomic<double> learn_sum_entropy_win;
        atomic<double> learn_sum_entropy;

        shared_timed_mutex nn_mutex;
        double newbob_decay;
        int newbob_num_trials;
        uint64_t auto_lr_drop;
        uint64_t last_lr_drop;
        double best_loss;
        double latest_loss_sum;
        uint64_t latest_loss_count;
        std::string best_nn_directory;

        uint64_t eval_save_interval;
        uint64_t loss_output_interval;

        // Loss calculation.
        // done: Number of phases targeted this time
        void calc_loss(size_t thread_id, uint64_t done);

        // Define the loss calculation in ↑ as a task and execute it
        TaskDispatcher task_dispatcher;
    };

    Value LearnerThink::get_shallow_value(Position& task_pos)
    {
        // Evaluation value for shallow search
        // The value of evaluate() may be used, but when calculating loss, learn_cross_entropy and
        // Use qsearch() because it is difficult to compare the values.
        // EvalHash has been disabled in advance. (If not, the same value will be returned every time)
        const auto [_, pv] = Search::qsearch(task_pos);

        const auto rootColor = task_pos.side_to_move();

        std::vector<StateInfo, AlignedAllocator<StateInfo>> states(pv.size());
        for (size_t i = 0; i < pv.size(); ++i)
        {
            task_pos.do_move(pv[i], states[i]);
        }

        const Value shallow_value =
            (rootColor == task_pos.side_to_move())
            ? Eval::evaluate(task_pos)
            : -Eval::evaluate(task_pos);

        for (auto it = pv.rbegin(); it != pv.rend(); ++it)
            task_pos.undo_move(*it);

        return shallow_value;
    }

    void LearnerThink::calc_loss(size_t thread_id, uint64_t done)
    {
        // There is no point in hitting the replacement table,
        // so at this timing the generation of the replacement table is updated.
        // It doesn't matter if you have disabled the substitution table.
        TT.new_search();
        TimePoint elapsed = now() - Search::Limits.startTime + 1;

        cout << "PROGRESS: " << now_string() << ", ";
        cout << sr.total_done << " sfens, ";
        cout << sr.total_done * 1000 / elapsed  << " sfens/second";
        cout << ", iteration " << epoch;
        cout << ", learning rate = " << global_learning_rate << ", ";

        // For calculation of verification data loss
        atomic<double> test_sum_cross_entropy_eval, test_sum_cross_entropy_win, test_sum_cross_entropy;
        atomic<double> test_sum_entropy_eval, test_sum_entropy_win, test_sum_entropy;
        test_sum_cross_entropy_eval = 0;
        test_sum_cross_entropy_win = 0;
        test_sum_cross_entropy = 0;
        test_sum_entropy_eval = 0;
        test_sum_entropy_win = 0;
        test_sum_entropy = 0;

        // norm for learning
        atomic<double> sum_norm;
        sum_norm = 0;

        // The number of times the pv first move of deep
        // search matches the pv first move of search(1).
        atomic<int> move_accord_count;
        move_accord_count = 0;

        auto th = Threads[thread_id];
        auto& pos = th->rootPos;
        StateInfo si;
        pos.set(pos.variant(), StartFEN, false, &si, th);
        cout << "startpos eval = " << Eval::evaluate(pos) << endl;

        // It's better to parallelize here, but it's a bit
        // troublesome because the search before slave has not finished.
        // I created a mechanism to call task, so I will use it.

        // The number of tasks to do.
        atomic<int> task_count;
        task_count = (int)sr.sfen_for_mse.size();
        task_dispatcher.task_reserve(task_count);

        // Create a task to search for the situation and give it to each thread.
        for (const auto& ps : sr.sfen_for_mse)
        {
            // Assign work to each thread using TaskDispatcher.
            // A task definition for that.
            // It is not possible to capture pos used in ↑,
            // so specify the variables you want to capture one by one.
            auto task =
                [
                    this,
                    &ps,
                    &test_sum_cross_entropy_eval,
                    &test_sum_cross_entropy_win,
                    &test_sum_cross_entropy,
                    &test_sum_entropy_eval,
                    &test_sum_entropy_win,
                    &test_sum_entropy,
                    &sum_norm,
                    &task_count,
                    &move_accord_count
                ](size_t task_thread_id)
            {
                auto task_th = Threads[task_thread_id];
                auto& task_pos = task_th->rootPos;
                StateInfo task_si;
                if (task_pos.set_from_packed_sfen(ps.sfen, &task_si, task_th) != 0)
                {
                    // Unfortunately, as an sfen for rmse calculation, an invalid sfen was drawn.
                    cout << "Error! : illegal packed sfen " << task_pos.fen() << endl;
                }

                const Value shallow_value = get_shallow_value(task_pos);

                // Evaluation value of deep search
                auto deep_value = (Value)ps.score;

                // Note) This code does not consider when
                //       eval_limit is specified in the learn command.

                // --- calculation of cross entropy

                // For the time being, regarding the win rate and loss terms only in the elmo method
                // Calculate and display the cross entropy.

                double test_cross_entropy_eval, test_cross_entropy_win, test_cross_entropy;
                double test_entropy_eval, test_entropy_win, test_entropy;
                calc_cross_entropy(
                    deep_value,
                    shallow_value,
                    ps,
                    test_cross_entropy_eval,
                    test_cross_entropy_win,
                    test_cross_entropy,
                    test_entropy_eval,
                    test_entropy_win,
                    test_entropy);

                // The total cross entropy need not be abs() by definition.
                test_sum_cross_entropy_eval += test_cross_entropy_eval;
                test_sum_cross_entropy_win += test_cross_entropy_win;
                test_sum_cross_entropy += test_cross_entropy;
                test_sum_entropy_eval += test_entropy_eval;
                test_sum_entropy_win += test_entropy_win;
                test_sum_entropy += test_entropy;
                sum_norm += (double)abs(shallow_value);

                // Determine if the teacher's move and the score of the shallow search match
                {
                    const auto [value, pv] = Search::search(task_pos, 1);
                    if ((uint16_t)pv[0] == ps.move)
                        move_accord_count.fetch_add(1, std::memory_order_relaxed);
                }

                // Reduced one task because I did it
                --task_count;
            };

            // Throw the defined task to slave.
            task_dispatcher.push_task_async(task);
        }

        // join yourself as a slave
        task_dispatcher.on_idle(thread_id);

        // wait for all tasks to complete
        while (task_count)
            sleep(1);

        latest_loss_sum += test_sum_cross_entropy - test_sum_entropy;
        latest_loss_count += sr.sfen_for_mse.size();

        // learn_cross_entropy may be called train cross
        // entropy in the world of machine learning,
        // When omitting the acronym, it is nice to be able to
        // distinguish it from test cross entropy(tce) by writing it as lce.

        if (sr.sfen_for_mse.size() && done)
        {
            cout << "INFO: "
                << "test_cross_entropy_eval = " << test_sum_cross_entropy_eval / sr.sfen_for_mse.size()
                << " , test_cross_entropy_win = " << test_sum_cross_entropy_win / sr.sfen_for_mse.size()
                << " , test_entropy_eval = " << test_sum_entropy_eval / sr.sfen_for_mse.size()
                << " , test_entropy_win = " << test_sum_entropy_win / sr.sfen_for_mse.size()
                << " , test_cross_entropy = " << test_sum_cross_entropy / sr.sfen_for_mse.size()
                << " , test_entropy = " << test_sum_entropy / sr.sfen_for_mse.size()
                << " , norm = " << sum_norm
                << " , move accuracy = " << (move_accord_count * 100.0 / sr.sfen_for_mse.size()) << "%"
                << endl;

            if (done != static_cast<uint64_t>(-1))
            {
                cout << "INFO: "
                    << "learn_cross_entropy_eval = " << learn_sum_cross_entropy_eval / done
                    << " , learn_cross_entropy_win = " << learn_sum_cross_entropy_win / done
                    << " , learn_entropy_eval = " << learn_sum_entropy_eval / done
                    << " , learn_entropy_win = " << learn_sum_entropy_win / done
                    << " , learn_cross_entropy = " << learn_sum_cross_entropy / done
                    << " , learn_entropy = " << learn_sum_entropy / done
                    << endl;
            }
        }
        else
        {
            cout << "Error! : sr.sfen_for_mse.size() = " << sr.sfen_for_mse.size() << " ,  done = " << done << endl;
        }

        // Clear 0 for next time.
        learn_sum_cross_entropy_eval = 0.0;
        learn_sum_cross_entropy_win = 0.0;
        learn_sum_cross_entropy = 0.0;
        learn_sum_entropy_eval = 0.0;
        learn_sum_entropy_win = 0.0;
        learn_sum_entropy = 0.0;
    }

    void LearnerThink::thread_worker(size_t thread_id)
    {
#if defined(_OPENMP)
        omp_set_num_threads((int)Options["Threads"]);
#endif

        auto th = Threads[thread_id];
        auto& pos = th->rootPos;

        while (true)
        {
            // display mse (this is sometimes done only for thread 0)
            // Immediately after being read from the file...

            // Lock the evaluation function so that it is not used during updating.
            shared_lock<shared_timed_mutex> read_lock(nn_mutex, defer_lock);
            if (sr.next_update_weights <= sr.total_done ||
                (thread_id != 0 && !read_lock.try_lock()))
            {
                if (thread_id != 0)
                {
                    // Wait except thread_id == 0.

                    if (stop_flag)
                        break;

                    // I want to parallelize rmse calculation etc., so if task() is loaded, process it.
                    task_dispatcher.on_idle(thread_id);
                    continue;
                }
                else
                {
                    // Only thread_id == 0 performs the following update process.

                    // The weight array is not updated for the first time.
                    if (sr.next_update_weights == 0)
                    {
                        sr.next_update_weights += mini_batch_size;
                        continue;
                    }

                    {
                        // update parameters

                        // Lock the evaluation function so that it is not used during updating.
                        lock_guard<shared_timed_mutex> write_lock(nn_mutex);
                        Eval::NNUE::update_parameters();
                    }

                    ++epoch;

                    // However, the elapsed time during update_weights() and calc_rmse() is ignored.
                    if (++sr.save_count * mini_batch_size >= eval_save_interval)
                    {
                        sr.save_count = 0;

                        // During this time, as the gradient calculation proceeds,
                        // the value becomes too large and I feel annoyed, so stop other threads.
                        const bool converged = save();
                        if (converged)
                        {
                            stop_flag = true;
                            sr.stop_flag = true;
                            break;
                        }
                    }

                    // Calculate rmse. This is done for samples of 10,000 phases.
                    // If you do with 40 cores, update_weights every 1 million phases
                    static uint64_t loss_output_count = 0;
                    if (++loss_output_count * mini_batch_size >= loss_output_interval)
                    {
                        loss_output_count = 0;

                        // Number of cases processed this time
                        uint64_t done = sr.total_done - sr.last_done;

                        // loss calculation
                        calc_loss(thread_id, done);

                        Eval::NNUE::check_health();

                        // Make a note of how far you have totaled.
                        sr.last_done = sr.total_done;
                    }

                    // Next time, I want you to do this series of
                    // processing again when you process only mini_batch_size.
                    sr.next_update_weights += mini_batch_size;

                    // Since I was waiting for the update of this
                    // sr.next_update_weights except the main thread,
                    // Once this value is updated, it will start moving again.
                }
            }

            PackedSfenValue ps;

        RETRY_READ:;

            if (!sr.read_to_thread_buffer(thread_id, ps))
            {
                // ran out of thread pool for my thread.
                // Because there are almost no phases left,
                // Terminate all other threads.

                stop_flag = true;
                break;
            }

            // The evaluation value exceeds the learning target value.
            // Ignore this aspect information.
            if (eval_limit < abs(ps.score))
                goto RETRY_READ;

            if (!use_draw_games_in_training && ps.game_result == 0)
                goto RETRY_READ;

            // Skip over the opening phase
            if (ps.gamePly < prng.rand(reduction_gameply))
                goto RETRY_READ;

            StateInfo si;
            if (pos.set_from_packed_sfen(ps.sfen, &si, th) != 0)
            {
                // I got a strange sfen. Should be debugged!
                // Since it is an illegal sfen, it may not be
                // displayed with pos.sfen(), but it is better than not.
                cout << "Error! : illigal packed sfen = " << pos.fen() << endl;
                goto RETRY_READ;
            }

            // I can read it, so try displaying it.
            //      cout << pos << value << endl;

            const auto rootColor = pos.side_to_move();

            int ply = 0;
            StateInfo state[MAX_PLY]; // PV of qsearch cannot be so long.

            if (!pos.pseudo_legal((Move)ps.move) || !pos.legal((Move)ps.move))
            {
                goto RETRY_READ;
            }

            pos.do_move((Move)ps.move, state[ply++]);

            // There is a possibility that all the pieces are blocked and stuck.
            // Also, the declaration win phase is excluded from
            // learning because you cannot go to leaf with PV moves.
            // (shouldn't write out such teacher aspect itself,
            // but may have written it out with an old generation routine)
            // Skip the position if there are no legal moves (=checkmated or stalemate).
            if (MoveList<LEGAL>(pos).size() == 0)
                goto RETRY_READ;

            // Evaluation value of shallow search (qsearch)
            const auto [_, pv] = Search::qsearch(pos);

            // Evaluation value of deep search
            const auto deep_value = (Value)ps.score;

            // I feel that the mini batch has a better gradient.
            // Go to the leaf node as it is, add only to the gradient array,
            // and later try AdaGrad at the time of rmse aggregation.


            // If the initial PV is different, it is better not to use it for learning.
            // If it is the result of searching a completely different place, it may become noise.
            // It may be better not to study where the difference in evaluation values ​​is too large.


            // A helper function that adds the gradient to the current phase.
            auto pos_add_grad = [&]() {
                // Use the value of evaluate in leaf as shallow_value.
                // Using the return value of qsearch() as shallow_value,
                // If PV is interrupted in the middle, the phase where
                // evaluate() is called to calculate the gradient,
                // and I don't think this is a very desirable property,
                // as the aspect that gives that gradient will be different.
                // I have turned off the substitution table, but since
                // the pv array has not been updated due to one stumbling block etc...

                const Value shallow_value =
                    (rootColor == pos.side_to_move())
                    ? Eval::evaluate(pos)
                    : -Eval::evaluate(pos);

                // Calculate loss for training data
                double learn_cross_entropy_eval, learn_cross_entropy_win, learn_cross_entropy;
                double learn_entropy_eval, learn_entropy_win, learn_entropy;
                calc_cross_entropy(
                    deep_value,
                    shallow_value,
                    ps,
                    learn_cross_entropy_eval,
                    learn_cross_entropy_win,
                    learn_cross_entropy,
                    learn_entropy_eval,
                    learn_entropy_win,
                    learn_entropy);

                learn_sum_cross_entropy_eval += learn_cross_entropy_eval;
                learn_sum_cross_entropy_win += learn_cross_entropy_win;
                learn_sum_cross_entropy += learn_cross_entropy;
                learn_sum_entropy_eval += learn_entropy_eval;
                learn_sum_entropy_win += learn_entropy_win;
                learn_sum_entropy += learn_entropy;

                Eval::NNUE::add_example(pos, rootColor, ps, 1.0);

                // Since the processing is completed, the counter of the processed number is incremented
                sr.total_done++;
            };

            bool illegal_move = false;
            for (auto m : pv)
            {
                // I shouldn't be an illegal player.
                // An illegal move sometimes comes here...
                if (!pos.pseudo_legal(m) || !pos.legal(m))
                {
                    //cout << pos << m << endl;
                    //assert(false);
                    illegal_move = true;
                    break;
                }

                pos.do_move(m, state[ply++]);
            }

            if (illegal_move)
            {
                goto RETRY_READ;
            }

            // Since we have reached the end phase of PV, add the slope here.
            pos_add_grad();
        }

    }

    // Write evaluation function file.
    bool LearnerThink::save(bool is_final)
    {
        // Each time you save, change the extension part of the file name like "0","1","2",..
        // (Because I want to compare the winning rate for each evaluation function parameter later)

        if (save_only_once)
        {
            // When EVAL_SAVE_ONLY_ONCE is defined,
            // Do not dig a subfolder because I want to save it only once.
            Eval::NNUE::save_eval("");
        }
        else if (is_final)
        {
            Eval::NNUE::save_eval("final");
            return true;
        }
        else
        {
            static int dir_number = 0;
            const std::string dir_name = std::to_string(dir_number++);
            Eval::NNUE::save_eval(dir_name);

            if (newbob_decay != 1.0 && latest_loss_count > 0) {
                static int trials = newbob_num_trials;
                const double latest_loss = latest_loss_sum / latest_loss_count;
                latest_loss_sum = 0.0;
                latest_loss_count = 0;
                cout << "loss: " << latest_loss;
                auto tot = sr.total_done.load();
                if (auto_lr_drop)
                {
                    cout << " < best (" << best_loss << "), accepted" << endl;
                    best_loss = latest_loss;
                    best_nn_directory = Path::combine((std::string)Options["EvalSaveDir"], dir_name);
                    trials = newbob_num_trials;

                    if (tot >= last_lr_drop + auto_lr_drop)
                    {
                        last_lr_drop = tot;
                        global_learning_rate *= newbob_decay;
                    }
                }
                else if (latest_loss < best_loss)
                {
                    cout << " < best (" << best_loss << "), accepted" << endl;
                    best_loss = latest_loss;
                    best_nn_directory = Path::combine((std::string)Options["EvalSaveDir"], dir_name);
                    trials = newbob_num_trials;
                }
                else
                {
                    cout << " >= best (" << best_loss << "), rejected" << endl;
                    best_nn_directory = Path::combine((std::string)Options["EvalSaveDir"], dir_name);

                    if (--trials > 0 && !is_final)
                    {
                        cout
                            << "reducing learning rate from " << global_learning_rate
                            << " to " << (global_learning_rate * newbob_decay)
                            << " (" << trials << " more trials)" << endl;

                        global_learning_rate *= newbob_decay;
                    }
                }

                if (trials == 0)
                {
                    cout << "converged" << endl;
                    return true;
                }
            }
        }
        return false;
    }

    // Shuffle_files(), shuffle_files_quick() subcontracting, writing part.
    // output_file_name: Name of the file to write
    // prng: random number generator
    // sfen_file_streams: fstream of each teacher phase file
    // sfen_count_in_file: The number of teacher positions present in each file.
    void shuffle_write(
        const string& output_file_name,
        PRNG& prng,
        vector<fstream>& sfen_file_streams,
        vector<uint64_t>& sfen_count_in_file)
    {
        uint64_t total_sfen_count = 0;
        for (auto c : sfen_count_in_file)
            total_sfen_count += c;

        // number of exported phases
        uint64_t write_sfen_count = 0;

        // Output the progress on the screen for each phase.
        const uint64_t buffer_size = 10000000;

        auto print_status = [&]()
        {
            // Output progress every 10M phase or when all writing is completed
            if (((write_sfen_count % buffer_size) == 0) ||
                (write_sfen_count == total_sfen_count))
            {
                cout << write_sfen_count << " / " << total_sfen_count << endl;
            }
        };

        cout << endl << "write : " << output_file_name << endl;

        fstream fs(output_file_name, ios::out | ios::binary);

        // total teacher positions
        uint64_t sfen_count_left = total_sfen_count;

        while (sfen_count_left != 0)
        {
            auto r = prng.rand(sfen_count_left);

            // Aspects stored in fs[0] file ... Aspects stored in fs[1] file ...
            //Think of it as a series like, and determine in which file r is pointing.
            // The contents of the file are shuffled, so you can take the next element from that file.
            // Each file has a_count[x] phases, so this process can be written as follows.

            uint64_t i = 0;
            while (sfen_count_in_file[i] <= r)
                r -= sfen_count_in_file[i++];

            // This confirms n. Before you forget it, reduce the remaining number.

            --sfen_count_in_file[i];
            --sfen_count_left;

            PackedSfenValue psv;
            // It's better to read and write all at once until the performance is not so good...
            if (sfen_file_streams[i].read((char*)&psv, sizeof(PackedSfenValue)))
            {
                fs.write((char*)&psv, sizeof(PackedSfenValue));
                ++write_sfen_count;
                print_status();
            }
        }

        print_status();
        fs.close();

        cout << "done!" << endl;
    }

    // Subcontracting the teacher shuffle "learn shuffle" command.
    // output_file_name: name of the output file where the shuffled teacher positions will be written
    void shuffle_files(const vector<string>& filenames, const string& output_file_name, uint64_t buffer_size, const std::string& seed)
    {
        // The destination folder is
        // tmp/ for temporary writing

        // Temporary file is written to tmp/ folder for each buffer_size phase.
        // For example, if buffer_size = 20M, you need a buffer of 20M*40bytes = 800MB.
        // In a PC with a small memory, it would be better to reduce this.
        // However, if the number of files increases too much,
        // it will not be possible to open at the same time due to OS restrictions.
        // There should have been a limit of 512 per process on Windows, so you can open here as 500,
        // The current setting is 500 files x 20M = 10G = 10 billion phases.

        PSVector buf(buffer_size);

        // ↑ buffer, a marker that indicates how much you have used
        uint64_t buf_write_marker = 0;

        // File name to write (incremental counter because it is a serial number)
        uint64_t write_file_count = 0;

        // random number to shuffle
        // Do not use std::random_device().  Because it always the same integers on MinGW.
        PRNG prng(seed);

        // generate the name of the temporary file
        auto make_filename = [](uint64_t i)
        {
            return "tmp/" + to_string(i) + ".bin";
        };

        // Exported files in tmp/ folder, number of teacher positions stored in each
        vector<uint64_t> a_count;

        auto write_buffer = [&](uint64_t size)
        {
            Algo::shuffle(buf, prng);

            // write to a file
            fstream fs;
            fs.open(make_filename(write_file_count++), ios::out | ios::binary);
            fs.write(reinterpret_cast<char*>(buf.data()), size * sizeof(PackedSfenValue));
            fs.close();
            a_count.push_back(size);

            buf_write_marker = 0;
            cout << ".";
        };

        std::filesystem::create_directory("tmp");

        // Shuffle and export as a 10M phase shredded file.
        for (auto filename : filenames)
        {
            fstream fs(filename, ios::in | ios::binary);
            cout << endl << "open file = " << filename;
            while (fs.read(reinterpret_cast<char*>(&buf[buf_write_marker]), sizeof(PackedSfenValue)))
                if (++buf_write_marker == buffer_size)
                    write_buffer(buffer_size);

            // Read in units of sizeof(PackedSfenValue),
            // Ignore the last remaining fraction. (Fails in fs.read, so exit while)
            // (The remaining fraction seems to be half-finished data
            // that was created because it was stopped halfway during teacher generation.)
        }

        if (buf_write_marker != 0)
            write_buffer(buf_write_marker);

        // Only shuffled files have been written write_file_count.
        // As a second pass, if you open all of them at the same time,
        // select one at random and load one phase at a time
        // Now you have shuffled.

        // Original file for shirt full + tmp file + file to write
        // requires 3 times the storage capacity of the original file.
        // 1 billion SSD is not enough for shuffling because it is 400GB for 10 billion phases.
        // If you want to delete (or delete by hand) the
        // original file at this point after writing to tmp,
        // The storage capacity is about twice that of the original file.
        // So, maybe we should have an option to delete the original file.

        // Files are opened at the same time. It is highly possible that this will exceed FOPEN_MAX.
        // In that case, rather than adjusting buffer_size to reduce the number of files.

        vector<fstream> afs;
        for (uint64_t i = 0; i < write_file_count; ++i)
            afs.emplace_back(fstream(make_filename(i), ios::in | ios::binary));

        // Throw to the subcontract function and end.
        shuffle_write(output_file_name, prng, afs, a_count);
    }

    // Subcontracting the teacher shuffle "learn shuffleq" command.
    // This is written in 1 pass.
    // output_file_name: name of the output file where the shuffled teacher positions will be written
    void shuffle_files_quick(const vector<string>& filenames, const string& output_file_name, const std::string& seed)
    {
        // random number to shuffle
        // Do not use std::random_device().  Because it always the same integers on MinGW.
        PRNG prng(seed);

        // number of files
        const size_t file_count = filenames.size();

        // Number of teacher positions stored in each file in filenames
        vector<uint64_t> sfen_count_in_file(file_count);

        // Count the number of teacher aspects in each file.
        vector<fstream> sfen_file_streams(file_count);

        for (size_t i = 0; i < file_count; ++i)
        {
            auto filename = filenames[i];
            auto& fs = sfen_file_streams[i];

            fs.open(filename, ios::in | ios::binary);
            const uint64_t file_size = get_file_size(fs);
            const uint64_t sfen_count = file_size / sizeof(PackedSfenValue);
            sfen_count_in_file[i] = sfen_count;

            // Output the number of sfen stored in each file.
            cout << filename << " = " << sfen_count << " sfens." << endl;
        }

        // Since we know the file size of each file,
        // open them all at once (already open),
        // Select one at a time and load one phase at a time
        // Now you have shuffled.

        // Throw to the subcontract function and end.
        shuffle_write(output_file_name, prng, sfen_file_streams, sfen_count_in_file);
    }

    // Subcontracting the teacher shuffle "learn shufflem" command.
    // Read the whole memory and write it out with the specified file name.
    void shuffle_files_on_memory(const vector<string>& filenames, const string output_file_name, const std::string& seed)
    {
        PSVector buf;

        for (auto filename : filenames)
        {
            std::cout << "read : " << filename << std::endl;
            read_file_to_memory(filename, [&buf](uint64_t size) {
                assert((size % sizeof(PackedSfenValue)) == 0);
                // Expand the buffer and read after the last end.
                uint64_t last = buf.size();
                buf.resize(last + size / sizeof(PackedSfenValue));
                return (void*)&buf[last];
                });
        }

        // shuffle from buf[0] to buf[size-1]
        // Do not use std::random_device().  Because it always the same integers on MinGW.
        PRNG prng(seed);
        uint64_t size = (uint64_t)buf.size();
        std::cout << "shuffle buf.size() = " << size << std::endl;

        Algo::shuffle(buf, prng);

        std::cout << "write : " << output_file_name << endl;

        // If the file to be written exceeds 2GB, it cannot be
        // written in one shot with fstream::write, so use wrapper.
        write_memory_to_file(
            output_file_name,
            (void*)&buf[0],
            sizeof(PackedSfenValue) * buf.size());

        std::cout << "..shuffle_on_memory done." << std::endl;
    }

    static void set_learning_search_limits()
    {
        // About Search::Limits
        // Be careful because this member variable is global and affects other threads.
        auto& limits = Search::Limits;

        limits.startTime = now();

        // Make the search equivalent to the "go infinite" command. (Because it is troublesome if time management is done)
        limits.infinite = true;

        // Since PV is an obstacle when displayed, erase it.
        limits.silent = true;

        // If you use this, it will be compared with the accumulated nodes of each thread. Therefore, do not use it.
        limits.nodes = 0;

        // depth is also processed by the one passed as an argument of Learner::search().
        limits.depth = 0;
    }

    // Learning from the generated game record
    void learn(Position&, istringstream& is)
    {
        const auto thread_num = (int)Options["Threads"];

        vector<string> filenames;

        // mini_batch_size 1M aspect by default. This can be increased.
        auto mini_batch_size = LEARN_MINI_BATCH_SIZE;

        // Number of loops (read the game record file this number of times)
        int loop = 1;

        // Game file storage folder (get game file with relative path from here)
        string base_dir;

        string target_dir;

        // --- Function that only shuffles the teacher aspect

        // normal shuffle
        bool shuffle_normal = false;
        uint64_t buffer_size = 20000000;
        // fast shuffling assuming each file is shuffled
        bool shuffle_quick = false;
        // A function to read the entire file in memory and shuffle it.
        // (Requires file size memory)
        bool shuffle_on_memory = false;
        // Conversion of packed sfen. In plain, it consists of sfen(string),
        // evaluation value (integer), move (eg 7g7f, string), result (loss-1, win 1, draw 0)
        bool use_convert_plain = false;
        // convert plain format teacher to Yaneura King's bin
        bool use_convert_bin = false;
        int ply_minimum = 0;
        int ply_maximum = 114514;
        bool interpolate_eval = 0;
        bool check_invalid_fen = false;
        bool check_illegal_move = false;
        // convert teacher in pgn-extract format to Yaneura King's bin
        bool use_convert_bin_from_pgn_extract = false;
        bool pgn_eval_side_to_move = false;
        bool convert_no_eval_fens_as_score_zero = false;
        // File name to write in those cases (default is "shuffled_sfen.bin")
        string output_file_name = "shuffled_sfen.bin";

        // If the absolute value of the evaluation value
        // in the deep search of the teacher phase exceeds this value,
        // that phase is discarded.
        int eval_limit = 32000;

        // Flag to save the evaluation function file only once near the end.
        bool save_only_once = false;

        // Shuffle about what you are pre-reading on the teacher aspect.
        // (Shuffle of about 10 million phases)
        // Turn on if you want to pass a pre-shuffled file.
        bool no_shuffle = false;

        global_learning_rate = 1.0;

        // elmo lambda
        ELMO_LAMBDA = 1.0;
        ELMO_LAMBDA2 = 1.0;
        ELMO_LAMBDA_LIMIT = 32000;

        // if (gamePly <rand(reduction_gameply)) continue;
        // An option to exclude the early stage from the learning target moderately like
        // If set to 1, rand(1)==0, so nothing is excluded.
        int reduction_gameply = 1;

        uint64_t nn_batch_size = 1000;
        double newbob_decay = 0.5;
        int newbob_num_trials = 4;
        uint64_t auto_lr_drop = 0;
        string nn_options;

        uint64_t eval_save_interval = LEARN_EVAL_SAVE_INTERVAL;
        uint64_t loss_output_interval = 1'000'000;

        string validation_set_file_name;
        string seed;

        // Assume the filenames are staggered.
        while (true)
        {
            string option;
            is >> option;

            if (option == "")
                break;

            // specify the number of phases of mini-batch
            if (option == "bat")
            {
                is >> mini_batch_size;
                mini_batch_size *= 10000; // Unit is ten thousand
            }

            // Specify the folder in which the game record is stored and make it the rooting target.
            else if (option == "targetdir") is >> target_dir;

            // Specify the number of loops
            else if (option == "loop")      is >> loop;

            // Game file storage folder (get game file with relative path from here)
            else if (option == "basedir")   is >> base_dir;

            // Mini batch size
            else if (option == "batchsize") is >> mini_batch_size;

            // learning rate
            else if (option == "lr")        is >> global_learning_rate;

            // Accept also the old option name.
            else if (option == "use_draw_in_training"
                  || option == "use_draw_games_in_training")
                is >> use_draw_games_in_training;

            // Accept also the old option name.
            else if (option == "use_draw_in_validation"
                  || option == "use_draw_games_in_validation")
                is >> use_draw_games_in_validation;

            // Accept also the old option name.
            else if (option == "use_hash_in_training"
                  || option == "skip_duplicated_positions_in_training")
                is >> skip_duplicated_positions_in_training;

            else if (option == "winning_probability_coefficient") is >> winning_probability_coefficient;

            // Using WDL with win rate model instead of sigmoid
            else if (option == "use_wdl") is >> use_wdl;


            // LAMBDA
            else if (option == "lambda")       is >> ELMO_LAMBDA;
            else if (option == "lambda2")      is >> ELMO_LAMBDA2;
            else if (option == "lambda_limit") is >> ELMO_LAMBDA_LIMIT;

            else if (option == "reduction_gameply") is >> reduction_gameply;

            // shuffle related
            else if (option == "shuffle")   shuffle_normal = true;
            else if (option == "buffer_size") is >> buffer_size;
            else if (option == "shuffleq")  shuffle_quick = true;
            else if (option == "shufflem")  shuffle_on_memory = true;
            else if (option == "output_file_name") is >> output_file_name;

            else if (option == "eval_limit") is >> eval_limit;
            else if (option == "save_only_once") save_only_once = true;
            else if (option == "no_shuffle") no_shuffle = true;

            else if (option == "nn_batch_size") is >> nn_batch_size;
            else if (option == "newbob_decay") is >> newbob_decay;
            else if (option == "newbob_num_trials") is >> newbob_num_trials;
            else if (option == "nn_options") is >> nn_options;
            else if (option == "auto_lr_drop") is >> auto_lr_drop;

            else if (option == "eval_save_interval") is >> eval_save_interval;
            else if (option == "loss_output_interval") is >> loss_output_interval;
            else if (option == "validation_set_file_name") is >> validation_set_file_name;

            // Rabbit convert related
            else if (option == "convert_plain") use_convert_plain = true;
            else if (option == "convert_bin") use_convert_bin = true;
            else if (option == "interpolate_eval") is >> interpolate_eval;
            else if (option == "check_invalid_fen") is >> check_invalid_fen;
            else if (option == "check_illegal_move") is >> check_illegal_move;
            else if (option == "convert_bin_from_pgn-extract") use_convert_bin_from_pgn_extract = true;
            else if (option == "pgn_eval_side_to_move") is >> pgn_eval_side_to_move;
            else if (option == "convert_no_eval_fens_as_score_zero") is >> convert_no_eval_fens_as_score_zero;
            else if (option == "src_score_min_value") is >> src_score_min_value;
            else if (option == "src_score_max_value") is >> src_score_max_value;
            else if (option == "dest_score_min_value") is >> dest_score_min_value;
            else if (option == "dest_score_max_value") is >> dest_score_max_value;
            else if (option == "seed") is >> seed;
            else if (option == "set_recommended_uci_options")
            {
                UCI::setoption("MultiPV", "1");
                UCI::setoption("Contempt", "0");
                UCI::setoption("Skill Level", "20");
                UCI::setoption("UCI_Chess960", "false");
                UCI::setoption("UCI_AnalyseMode", "false");
                UCI::setoption("UCI_LimitStrength", "false");
                UCI::setoption("PruneAtShallowDepth", "false");
                UCI::setoption("EnableTranspositionTable", "false");
            }
            // Otherwise, it's a filename.
            else
                filenames.push_back(option);
        }

        if (loss_output_interval == 0)
        {
            loss_output_interval = LEARN_RMSE_OUTPUT_INTERVAL * mini_batch_size;
        }

        cout << "learn command , ";

        // Issue a warning if OpenMP is disabled.
#if !defined(_OPENMP)
        cout << "Warning! OpenMP disabled." << endl;
#endif

        SfenReader sr(thread_num, seed);
        LearnerThink learn_think(sr, seed);

        // Display learning game file
        if (target_dir != "")
        {
            string kif_base_dir = Path::combine(base_dir, target_dir);

            namespace sys = std::filesystem;
            sys::path p(kif_base_dir); // Origin of enumeration
            std::for_each(sys::directory_iterator(p), sys::directory_iterator(),
                [&](const sys::path& path) {
                    if (sys::is_regular_file(path))
                        filenames.push_back(Path::combine(target_dir, path.filename().generic_string()));
                });
        }

        cout << "learn from ";
        for (auto s : filenames)
            cout << s << " , ";

        cout << endl;
        if (!validation_set_file_name.empty())
        {
            cout << "validation set  : " << validation_set_file_name << endl;
        }

        cout << "base dir        : " << base_dir << endl;
        cout << "target dir      : " << target_dir << endl;

        // shuffle mode
        if (shuffle_normal)
        {
            cout << "buffer_size     : " << buffer_size << endl;
            cout << "shuffle mode.." << endl;
            shuffle_files(filenames, output_file_name, buffer_size, seed);
            return;
        }

        if (shuffle_quick)
        {
            cout << "quick shuffle mode.." << endl;
            shuffle_files_quick(filenames, output_file_name, seed);
            return;
        }

        if (shuffle_on_memory)
        {
            cout << "shuffle on memory.." << endl;
            shuffle_files_on_memory(filenames, output_file_name, seed);
            return;
        }

        if (use_convert_plain)
        {
            Eval::NNUE::init();
            cout << "convert_plain.." << endl;
            convert_plain(filenames, output_file_name);
            return;
        }

        if (use_convert_bin)
        {
            Eval::NNUE::init();
            cout << "convert_bin.." << endl;
            convert_bin(
                filenames,
                output_file_name,
                ply_minimum,
                ply_maximum,
                interpolate_eval,
                src_score_min_value,
                src_score_max_value,
                dest_score_min_value,
                dest_score_max_value,
                check_invalid_fen,
                check_illegal_move);

            return;

        }

        if (use_convert_bin_from_pgn_extract)
        {
            Eval::NNUE::init();
            cout << "convert_bin_from_pgn-extract.." << endl;
            convert_bin_from_pgn_extract(
                filenames,
                output_file_name,
                pgn_eval_side_to_move,
                convert_no_eval_fens_as_score_zero);

            return;
        }

        cout << "loop              : " << loop << endl;
        cout << "eval_limit        : " << eval_limit << endl;
        cout << "save_only_once    : " << (save_only_once ? "true" : "false") << endl;
        cout << "no_shuffle        : " << (no_shuffle ? "true" : "false") << endl;

        // Insert the file name for the number of loops.
        for (int i = 0; i < loop; ++i)
        {
            // sfen reader, I'll read it in reverse
            // order so I'll reverse it here. I'm sorry.
            for (auto it = filenames.rbegin(); it != filenames.rend(); ++it)
            {
                sr.filenames.push_back(Path::combine(base_dir, *it));
            }
        }

        cout << "Loss Function     : " << LOSS_FUNCTION << endl;
        cout << "mini-batch size   : " << mini_batch_size << endl;

        cout << "nn_batch_size     : " << nn_batch_size << endl;
        cout << "nn_options        : " << nn_options << endl;

        cout << "learning rate     : " << global_learning_rate << endl;
        cout << "use_draw_games_in_training : " << use_draw_games_in_training << endl;
        cout << "use_draw_games_in_validation : " << use_draw_games_in_validation << endl;
        cout << "skip_duplicated_positions_in_training : " << skip_duplicated_positions_in_training << endl;

        if (newbob_decay != 1.0) {
            cout << "scheduling        : newbob with decay = " << newbob_decay
                << ", " << newbob_num_trials << " trials" << endl;
        }
        else {
            cout << "scheduling        : default" << endl;
        }

        // If reduction_gameply is set to 0, rand(0) will be divided by 0, so correct it to 1.
        reduction_gameply = max(reduction_gameply, 1);
        cout << "reduction_gameply : " << reduction_gameply << endl;

        cout << "LAMBDA            : " << ELMO_LAMBDA << endl;
        cout << "LAMBDA2           : " << ELMO_LAMBDA2 << endl;
        cout << "LAMBDA_LIMIT      : " << ELMO_LAMBDA_LIMIT << endl;
        cout << "eval_save_interval  : " << eval_save_interval << " sfens" << endl;
        cout << "loss_output_interval: " << loss_output_interval << " sfens" << endl;

        // -----------------------------------
        // various initialization
        // -----------------------------------

        cout << "init.." << endl;

        Threads.main()->ponder = false;

        set_learning_search_limits();

        cout << "init_training.." << endl;
        Eval::NNUE::initialize_training(seed);
        Eval::NNUE::set_batch_size(nn_batch_size);
        Eval::NNUE::set_options(nn_options);
        if (newbob_decay != 1.0 && !Options["SkipLoadingEval"]) {
            // Save the current net to [EvalSaveDir]\original.
            Eval::NNUE::save_eval("original");

            // Set the folder above to best_nn_directory so that the trainer can
            // resotre the network parameters from the original net file.
            learn_think.best_nn_directory =
                Path::combine(Options["EvalSaveDir"], "original");
        }

        cout << "init done." << endl;

        // Reflect other option settings.
        learn_think.eval_limit = eval_limit;
        learn_think.save_only_once = save_only_once;
        learn_think.sr.no_shuffle = no_shuffle;
        learn_think.reduction_gameply = reduction_gameply;

        learn_think.newbob_decay = newbob_decay;
        learn_think.newbob_num_trials = newbob_num_trials;
        learn_think.auto_lr_drop = auto_lr_drop;

        learn_think.eval_save_interval = eval_save_interval;
        learn_think.loss_output_interval = loss_output_interval;

        // Start a thread that loads the phase file in the background
        // (If this is not started, mse cannot be calculated.)
        learn_think.start_file_read_worker();

        learn_think.mini_batch_size = mini_batch_size;

        if (validation_set_file_name.empty())
        {
            // Get about 10,000 data for mse calculation.
            sr.read_for_mse();
        }
        else
        {
            sr.read_validation_set(validation_set_file_name, eval_limit);
        }

        cout << "Forcing Use NNUE pure.\n";
        UCI::setoption("Use NNUE", "pure");

        Eval::NNUE::verify_any_net_loaded();

        // Calculate rmse once at this point (timing of 0 sfen)
        // sr.calc_rmse();

        if (newbob_decay != 1.0) {
            learn_think.calc_loss(0, -1);
            learn_think.best_loss = learn_think.latest_loss_sum / learn_think.latest_loss_count;
            learn_think.latest_loss_sum = 0.0;
            learn_think.latest_loss_count = 0;
            cout << "initial loss: " << learn_think.best_loss << endl;
        }

        // -----------------------------------
        // start learning evaluation function parameters
        // -----------------------------------

        // Start learning.
        learn_think.go_think();

        Eval::NNUE::finalize_net();

        // Save once at the end.
        learn_think.save(true);
    }

} // namespace Learner
