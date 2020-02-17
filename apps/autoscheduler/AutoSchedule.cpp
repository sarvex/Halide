/*
  This file is the core of the autoscheduler. Most of the code here is
  about navigating the search space and computing the
  featurization. This also contains the top-level interface into the
  autoscheduler.

  The most interesting classes to look at are:

  LoopNest               Represents one node in our tree representation of loop nests.
  State                  A state in the beam search. Holds a root loop nest.

  Interesting functions below are:

  generate_schedule            The top-level entrypoint, which computes and applies a schedule to a Halide pipeline
  optimal_schedule             Runs the passes of the coarse-to-fine beam search
  optimal_schedule_pass        Runs a single pass of beam search
  LoopNest::compute_features   Recursively walks over a loop nest tree, computing our featurization using Halide's analysis tools.
  LoopNest::apply              Actually apply a computed schedule to a Halide pipeline
  State::generate_children     Generates successor states to a state in the beam search

  Environment variables used (directly or indirectly):

  HL_BEAM_SIZE
  Beam size to use in the beam search. Defaults to 32. Use 1 to get a greedy search instead.

  HL_CYOS
  "Choose-your-own-schedule". If set to 1, lets you navigate the search tree by hand in the terminal. Whee! This is for debugging the autoscheduler.

  HL_FEATURE_FILE -> output
  *** DEPRECATED *** use the 'featurization' output from Generator instead
  Write out a training featurization for the selected schedule into this file.
  Needs to be converted to a sample file with the runtime using featurization_to_sample before it can be used to train.

  HL_MACHINE_PARAMS
  An architecture description string. Used by Halide master to configure the cost model. We only use the first term. Set it to the number of cores to target.

  HL_PERMIT_FAILED_UNROLL
  Set to 1 to tell Halide not to freak out if we try to unroll a loop that doesn't have a constant extent. Should generally not be necessary, but sometimes the autoscheduler's model for what will and will not turn into a constant during lowering is inaccurate, because Halide isn't perfect at constant-folding.

  HL_SCHEDULE_FILE
  *** DEPRECATED *** use the 'schedule' output from Generator instead
  Write out a human-and-machine readable block of scheduling source code for the selected schedule into this file.

  HL_RANDOM_DROPOUT
  percent chance of accepting each state in the beam. Normalized by the number of decisions made, so 5 would be there's a 5 percent chance of never rejecting any states.

  HL_SEED
  Random seed used by the random dropout.

  HL_WEIGHTS_DIR
  When training or schedule, read weights from this directory or file
  (if path ends in `.weights` it is written as a single file, otherwise a directory of files)

  HL_NO_SUBTILING
  If set to 1, limits the search space to that of Mullapudi et al.

  HL_DEBUG_AUTOSCHEDULE
  If set, is used for the debug log level for auto-schedule generation (overriding the
  value of HL_DEBUG_CODEGEN, if any).

  TODO: expose these settings by adding some means to pass args to
  generator plugins instead of environment vars.
*/
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "ASLog.h"
#include "AutoSchedule.h"
#include "CostModel.h"
#include "DefaultCostModel.h"
#include "Errors.h"
#include "Featurization.h"
#include "FunctionDAG.h"
#include "Halide.h"
#include "LoopNest.h"
#include "NetworkSize.h"
#include "PerfectHashMap.h"
#include "State.h"

#ifdef _WIN32
#include <io.h>
#define _isatty isatty;
#endif

namespace Halide {
namespace Internal {
namespace Autoscheduler {

using std::string;
using std::vector;
using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;

struct RNG {
    std::mt19937 gen;
    std::uniform_real_distribution<double> dis;

    RNG(uint32_t seed)
        : gen{seed}
        , dis{0.0, 100.0}
    {}

    double operator()() {
        return dis(gen);
    }
};

struct ProgressBar {
    void set(double progress) {
        if (!draw_progress_bar) return;
        counter++;
        const int bits = 11;
        if (counter & ((1 << bits) - 1)) return;
        const int pos = (int)(progress * 78);
        aslog(0) << '[';
        for (int j = 0; j < 78; j++) {
            if (j < pos) {
                aslog(0) << '.';
            } else if (j - 1 < pos) {
                aslog(0) << "/-\\|"[(counter >> bits) % 4];
            } else {
                aslog(0) << ' ';
            }
        }
        aslog(0) << ']';
        for (int j = 0; j < 80; j++) {
            aslog(0) << '\b';
        }
    }

    void clear() {
        if (counter) {
            for (int j = 0; j < 80; j++) {
                aslog(0) << ' ';
            }
            for (int j = 0; j < 80; j++) {
                aslog(0) << '\b';
            }
        }
    }

private:
    uint32_t counter = 0;
    const bool draw_progress_bar = isatty(2);
};

// Get the HL_RANDOM_DROPOUT environment variable. Purpose of this is described above.
double get_dropout_threshold() {
    string random_dropout_str = get_env_variable("HL_RANDOM_DROPOUT");
    if (!random_dropout_str.empty()) {
        return atof(random_dropout_str.c_str());
    } else {
        return 100;
    }
}

// Decide whether or not to drop a beam search state. Used for
// randomly exploring the search tree for autotuning and to generate
// training data.
bool random_dropout(std::mt19937 &rng, size_t num_decisions) {
    static double random_dropout_threshold = std::max(0.0, get_dropout_threshold());
    if (random_dropout_threshold >= 100) return false;

    // The random dropout threshold is the chance that we operate
    // entirely greedily and never discard anything.
    double t = random_dropout_threshold;
    t /= 100;
    t = std::pow(t, 1.0f / num_decisions);
    t *= 100;

    double r = rng() % 100;
    bool drop_it = r >= t;
    return drop_it;
}

// Configure a cost model to process a specific pipeline.
void configure_pipeline_features(const FunctionDAG &dag,
                                 const MachineParams &params,
                                 CostModel *cost_model) {
    cost_model->reset();
    const int pipeline_feat_size = head1_w * head1_h;
    // We ignore the first seven pipeline features in the cost
    // model. It's just a mask of which types are in use.
    static_assert(sizeof(PipelineFeatures) - 7 * sizeof(int) ==
                      sizeof(int) * pipeline_feat_size,
                  "Incorrect size for pipeline features");
    int num_stages = 0;
    for (const auto &n : dag.nodes) {
        if (!n.is_input) num_stages += (int)n.stages.size();
    }
    Runtime::Buffer<float> pipeline_features(head1_w, head1_h, num_stages);
    int stage = 0;
    for (const auto &n : dag.nodes) {
        if (n.is_input) continue;
        for (auto it = n.stages.rbegin(); it != n.stages.rend(); it++) {
            const auto &s = *it;
            const int *pipeline_feats = (const int *)(&(s.features)) + 7;
            // skip the first 7 features
            for (int i = 0; i < pipeline_feat_size; i++) {
                int x = i / 7;
                int y = i % 7;
                pipeline_features(x, y, stage) = pipeline_feats[i];
            }
            stage += 1;
        }
    }
    internal_assert(stage == num_stages);
    cost_model->set_pipeline_features(pipeline_features, params.parallelism);
}

// A single pass of coarse-to-fine beam search.
IntrusivePtr<State> optimal_schedule_pass(FunctionDAG &dag,
                                          vector<Function> outputs,
                                          const MachineParams &params,
                                          const Target &target,
                                          CostModel *cost_model,
                                          std::mt19937 &rng,
                                          int beam_size,
                                          int pass_idx,
                                          int num_passes,
                                          ProgressBar &tick,
                                          std::unordered_set<uint64_t> &permitted_hashes,
                                          Statistics& stats,
                                          const NodeMap<bool>& inlined_nodes,
                                          const NodeMap<std::vector<IntrusivePtr<const LoopNest>>>& compute_root_nodes,
                                          NodeMap<std::map<int, std::vector<IntrusivePtr<const LoopNest>>>>& memoized_compute_root_blocks) {

    if (cost_model) {
        configure_pipeline_features(dag, params, cost_model);
    }

    StateQueue q, pending;

    // The initial state, with no decisions made
    {
        IntrusivePtr<State> initial{new State};
        initial->root = new LoopNest;
        q.emplace(std::move(initial));
    }

    int expanded = 0;

    std::function<void(IntrusivePtr<State> &&)> enqueue_new_children =
        [&](IntrusivePtr<State> &&s) {
            // aslog(0) << "\n** Generated child: ";
            // s->dump();
            // s->calculate_cost(dag, params, nullptr, true);

            // Each child should have one more decision made than its parent state.
            internal_assert(s->num_decisions_made == s->parent->num_decisions_made + 1);

            int progress = s->num_decisions_made * beam_size + expanded;
            size_t max_progress = dag.nodes.size() * beam_size * 2;

            // Update the progress bar
            tick.set(double(progress) / max_progress);
            s->penalized = false;

            ++stats.num_states_added;

            // Add the state to the list of states to evaluate
            q.emplace(std::move(s));
        };

    string cyos_str = get_env_variable("HL_CYOS");

    // This loop is beam search over the sequence of decisions to make.
    for (int i = 0;; i++) {
        std::unordered_map<uint64_t, int> hashes;
        q.swap(pending);

        if (pending.empty()) {
            if ((false) && beam_size < 1000) {  // Intentional dead code. Extra parens to pacify clang-tidy.
                // Total mortality. Double the beam size and
                // restart. Disabled for now because total mortality
                // may indicate a bug.
                return optimal_schedule_pass(dag,
                                             outputs,
                                             params,
                                             target,
                                             cost_model,
                                             rng,
                                             beam_size * 2,
                                             pass_idx,
                                             num_passes,
                                             tick,
                                             permitted_hashes,
                                             stats,
                                             inlined_nodes,
                                             compute_root_nodes,
                                             memoized_compute_root_blocks);
            } else {
                internal_error << "Ran out of legal states with beam size " << beam_size << "\n";
            }
        }

        if ((int)pending.size() > beam_size * 10000) {
            aslog(0) << "Warning: Huge number of states generated (" << pending.size() << ").\n";
        }

        expanded = 0;
        while (expanded < beam_size && !pending.empty()) {

            IntrusivePtr<State> state{pending.pop()};

            if (beam_size > 1 && num_passes > 1 && pass_idx >= 0) {
                // We are doing coarse-to-fine beam search using the
                // hashing strategy mentioned in the paper.
                //
                // We will lazily apply cost penalties to the queue
                // according to structural uniqueness.
                if (!state->penalized) {
                    uint64_t h1 = state->structural_hash(pass_idx + 1);
                    uint64_t h0 = state->structural_hash(pass_idx - 1);
                    // We penalize the cost of a state proportionately
                    // to how many states we've already seen with that
                    // hash.
                    int penalty = ++hashes[h1];
                    if (pass_idx > 0 && !permitted_hashes.count(h0)) {
                        // It's possible to get yourself into a state
                        // where the only things in the beam that match
                        // the hash were quick-rejected due to details not
                        // captured in the hash, so we apply a huge
                        // penalty, but leave the impermissible state in
                        // the beam.
                        penalty += 10;
                    }
                    if (penalty > 1) {
                        state->penalized = true;
                        state->cost *= penalty;
                        for (auto& c : state->cost_per_stage) {
                            c *= penalty;
                        }
                        // After penalizing this state, if it's no
                        // longer the best, defer it. We set the
                        // 'penalized' flag so that we know not to
                        // penalize and defer it again.
                        if (!pending.empty() && state->cost > pending.top()->cost) {
                            pending.emplace(std::move(state));
                            continue;
                        }
                    }
                }
            }

            // Random dropout
            if (pending.size() > 1 && random_dropout(rng, dag.nodes.size() * 2)) {
                continue;
            }

            if (state->num_decisions_made == 2 * (int)dag.nodes.size()) {
                // We've reached the end of the pass. The first state
                // must be the best, because we're pulling off a
                // priority queue.
                auto best = state;

                // Bless the reasonable stuff in the beam as
                // permissible states to visit again. We define
                // reasonable as having a cost no more than 20% higher
                // than the cost of the best thing. Only do this if
                // there are more coarse-to-fine passes yet to come.
                if (pass_idx >= 0 && pass_idx + 1 < num_passes) {
                    int blessed = 0;
                    while (state->cost <= 1.2 * best->cost && blessed < beam_size) {
                        const State *s = state.get();
                        while (s) {
                            uint64_t h1 = s->structural_hash(pass_idx);
                            permitted_hashes.insert(h1);
                            s = s->parent.get();
                        }
                        if (pending.empty()) break;
                        state = pending.pop();
                        blessed++;
                    }
                }

                return best;
            }

            auto t1 = std::chrono::high_resolution_clock::now();
            state->generate_children(dag, params, target, cost_model, enqueue_new_children, stats, pass_idx == -1, inlined_nodes, compute_root_nodes, memoized_compute_root_blocks);
            stats.generate_children_time += std::chrono::high_resolution_clock::now() - t1;
            expanded++;
        }

        // Drop the other states unconsidered.
        pending.clear();

        if (cost_model) {
            // Now evaluate all the costs and re-sort them in the priority queue
            auto t1 = std::chrono::high_resolution_clock::now();
            cost_model->evaluate_costs();
            stats.cost_model_evaluation_time += std::chrono::high_resolution_clock::now() - t1;
            q.resort();
        }

        for (size_t j = 0; j < q.size(); j++) {
            //float cost_per_stage_sum = 0;
            //for (const auto& c : q[j]->cost_per_stage) {
                //cost_per_stage_sum += c;
            //}

            //if (std::abs(cost_per_stage_sum - q[j]->cost) > 0.1) {
                //std::cerr << "Sum of per stage costs: " << cost_per_stage_sum << "; cost: " << q[j]->cost << "\n";
                //for (const auto& c : q[j]->cost_per_stage) {
                    //std::cerr << c << ", ";
                //}
                //q[j]->root->dump("", nullptr);
                //std::cerr << "\n";
                //internal_assert(false);
            //}

            if (std::isinf(q[j]->cost)) {
                debug(0) << "Infinite cost on intermediate state: " << q[j]->cost << "\n";
                q[j]->dump();
            }
        }

        if (cyos_str == "1") {
            // The user has set HL_CYOS, and wants to navigate the
            // search space manually.  Discard everything in the queue
            // except for the user-chosen option.
            aslog(0) << "\n--------------------\n";
            aslog(0) << "Select a schedule:\n";
            for (int choice_label = (int)q.size() - 1; choice_label >= 0; choice_label--) {
                auto state = q[choice_label];
                aslog(0) << "\n[" << choice_label << "]:\n";
                state->dump();
                //state->calculate_cost(dag, params, target, cost_model, stats, true);
            }
            cost_model->evaluate_costs();

            // Select next partial schedule to expand.
            int selection = -1;
            while (selection < 0 || selection >= (int)q.size()) {
                aslog(0) << "\nEnter selection: ";
                std::cin >> selection;
            }

            auto selected = q[selection];
            selected->dump();
            q.clear();
            q.emplace(std::move(selected));
        }
    }
}

struct ClearInlinedMutator {
    void operator()(LoopNest* new_loop_nest) const {
        new_loop_nest->inlined = {};
    }
};

void freeze_lowest_cost_stages(const FunctionDAG& dag, const IntrusivePtr<State> best, NodeMap<bool>& inlined_nodes, NodeMap<std::vector<IntrusivePtr<const LoopNest>>>& compute_root_nodes) {

    std::vector<std::pair<int, double>> node_ids_and_costs;
    NodeMap<double> node_costs;
    size_t num_stages = 0;
    size_t num_nodes = 0;
    for (const auto& n : dag.nodes) {
        if (n.is_input) {
            continue;
        }
        num_stages += n.stages.size();
        ++num_nodes;
    }

    for (size_t i = 0; i < num_stages; ++i) {
        if (dag.stage_id_to_node_map.at(i)->is_input) {
            continue;
        }

        if (!node_costs.contains(dag.stage_id_to_node_map.at(i))) {
            node_costs.get_or_create(dag.stage_id_to_node_map.at(i)) = 0;
        }

        node_costs.get(dag.stage_id_to_node_map.at(i)) += best->cost_per_stage[i];
    }

    for (auto it = node_costs.begin(); it != node_costs.end(); it++) {
        node_ids_and_costs.push_back({it.key()->id, it.value()});
    }

    for (const auto& n : node_ids_and_costs) {
        internal_assert(n.first >= 0);
    }

    std::sort(node_ids_and_costs.begin(), node_ids_and_costs.end(), [](const std::pair<int, double>& a, const std::pair<int, double>& b) {
        return a.second < b.second;
    });

    size_t num_to_freeze = num_nodes - std::log2(num_nodes);
    NodeMap<bool> nodes_to_freeze;
    for (size_t i = 0; i < num_to_freeze; ++i) {
        auto id = node_ids_and_costs[i].first;
        std::cerr << "Freezing " << dag.nodes[id].func.name() << " with cost = " << node_ids_and_costs[i].second << "\n";
        nodes_to_freeze.insert(&dag.nodes[id], true);
    }

    best->root->collect_nodes_that_should_be_inlined(nodes_to_freeze, inlined_nodes);

    ClearInlinedMutator mutator{};

    for (const auto& c : best->root->children) {
        if (nodes_to_freeze.contains(c->node)) {
            auto new_loop_nest = deep_copy_loop_nest(c, mutator);
            compute_root_nodes.get_or_create(c->node).push_back(new_loop_nest);
            std::cerr << "Freezing as compute_root: " << c->node->func.name() << "\n";
        }
    }
}

// Performance coarse-to-fine beam search and return the best state found.
IntrusivePtr<State> optimal_schedule(FunctionDAG &dag,
                                     vector<Function> outputs,
                                     const MachineParams &params,
                                     const Target &target,
                                     CostModel *cost_model,
                                     std::mt19937 &rng,
                                     int beam_size,
                                     Statistics& stats) {

    IntrusivePtr<State> best;

    std::unordered_set<uint64_t> permitted_hashes;

    // If the beam size is one, it's pointless doing multiple passes.
    int num_passes = (beam_size == 1) ? 1 : 5;

    string cyos_str = get_env_variable("HL_CYOS");
    if (cyos_str == "1") {
        // If the user is manually navigating the search space, don't
        // ask them to do more than one pass.
        num_passes = 1;
    }

    string num_passes_str = get_env_variable("HL_NUM_PASSES");
    if (!num_passes_str.empty()) {
        // The user has requested a non-standard number of passes.
        num_passes = std::atoi(num_passes_str.c_str());
    }

    NodeMap<std::map<int, std::vector<IntrusivePtr<const LoopNest>>>> memoized_compute_root_blocks;
    memoized_compute_root_blocks.make_large(dag.nodes.size());

    bool use_pre_pass = get_env_variable("HL_FREEZE_INLINE_COMPUTE_ROOT") == "1";
    int pass_idx = use_pre_pass ? -1 : 0;

    if (use_pre_pass && num_passes > 1) {
        --num_passes;
    }

    NodeMap<bool> inlined_nodes;
    NodeMap<std::vector<IntrusivePtr<const LoopNest>>> compute_root_nodes;

    for (; pass_idx < num_passes; pass_idx++) {
        ProgressBar tick;

        auto pass = optimal_schedule_pass(dag, outputs, params, target, cost_model,
            rng, beam_size, pass_idx, num_passes, tick, permitted_hashes, stats, inlined_nodes, compute_root_nodes, memoized_compute_root_blocks);

        tick.clear();

        if (aslog::aslog_level() == 0) {
            aslog(0) << "Pass " << pass_idx + 1 << " of " << num_passes << ", cost: " << pass->cost << "\n";
        } else {
            aslog(0) << "Pass " << pass_idx + 1 << " result: ";
            pass->dump();
        }

        if (pass_idx == -1) {
            freeze_lowest_cost_stages(dag, pass, inlined_nodes, compute_root_nodes);
        }

        if (pass_idx >= 0 && (pass_idx == 0 || pass->cost < best->cost)) {
            // Track which pass produced the lowest-cost state. It's
            // not necessarily the final one.
            best = pass;
        }
    }

    aslog(0) << "Best cost: " << best->cost << "\n";

    return best;
}

// The main entrypoint to generate a schedule for a pipeline.
void generate_schedule(const std::vector<Function> &outputs,
                       const Target &target,
                       const MachineParams &params,
                       AutoSchedulerResults *auto_scheduler_results) {
    auto start = std::chrono::high_resolution_clock::now();
    aslog(0) << "generate_schedule for target=" << target.to_string() << "\n";

    // Start a timer
    HALIDE_TIC;

    // Get the seed for random dropout
    string seed_str = get_env_variable("HL_SEED");
    // Or use the time, if not set.
    int seed = (int)time(NULL);
    if (!seed_str.empty()) {
        seed = atoi(seed_str.c_str());
    }
    aslog(1) << "Dropout seed = " << seed << '\n';
    std::mt19937 rng((uint32_t)seed);

    // Get the beam size
    string beam_size_str = get_env_variable("HL_BEAM_SIZE");
    // Defaults to 32
    size_t beam_size = 32;
    if (!beam_size_str.empty()) {
        beam_size = atoi(beam_size_str.c_str());
    }

    string weights_in_path = get_env_variable("HL_WEIGHTS_DIR");
    string weights_out_path;  // deliberately empty

    string randomize_weights_str = get_env_variable("HL_RANDOMIZE_WEIGHTS");
    bool randomize_weights = randomize_weights_str == "1";

    // Analyse the Halide algorithm and construct our abstract representation of it
    FunctionDAG dag(outputs, params, target);
    if (aslog::aslog_level() > 0) {
        dag.dump();
    }

    // Construct a cost model to use to evaluate states. Currently we
    // just have the one, but it's an abstract interface, so others
    // can be slotted in for experimentation.
    std::unique_ptr<CostModel> cost_model = make_default_cost_model(weights_in_path, weights_out_path, randomize_weights);
    internal_assert(cost_model != nullptr);

    IntrusivePtr<State> optimal;

    Statistics stats;

    // Run beam search
    optimal = optimal_schedule(dag, outputs, params, target, cost_model.get(), rng, beam_size, stats);

    HALIDE_TOC;

    // Dump the schedule found
    aslog(1) << "** Optimal schedule:\n";

    // Just to get the debugging prints to fire
    optimal->calculate_cost(dag, params, target, cost_model.get(), stats, aslog::aslog_level() > 0);

    // Apply the schedules to the pipeline
    optimal->apply_schedule(dag, params, target);

    // Print out the schedule
    if (aslog::aslog_level() > 0) {
        optimal->dump();
    }

    string schedule_file = get_env_variable("HL_SCHEDULE_FILE");
    if (!schedule_file.empty()) {
        user_warning << "HL_SCHEDULE_FILE is deprecated; use the schedule output from Generator instead\n";
        aslog(1) << "Writing schedule to " << schedule_file << "...\n";
        std::ofstream f(schedule_file);
        f << "// --- BEGIN machine-generated schedule\n"
          << optimal->schedule_source
          << "// --- END machine-generated schedule\n";
        f.close();
        internal_assert(!f.fail()) << "Failed to write " << schedule_file;
    }

    // Save the featurization, so that we can use this schedule as
    // training data (once we've benchmarked it).
    string feature_file = get_env_variable("HL_FEATURE_FILE");
    if (!feature_file.empty()) {
        user_warning << "HL_FEATURE_FILE is deprecated; use the featurization output from Generator instead\n";
        std::ofstream binfile(feature_file, std::ios::binary | std::ios_base::trunc);
        optimal->save_featurization(dag, params, target, binfile);
        binfile.close();
        internal_assert(!binfile.fail()) << "Failed to write " << feature_file;
    }

    if (auto_scheduler_results) {
        auto_scheduler_results->scheduler_name = "Adams2019";
        auto_scheduler_results->schedule_source = optimal->schedule_source;
        {
            std::ostringstream out;
            optimal->save_featurization(dag, params, target, out);
            auto_scheduler_results->featurization.resize(out.str().size());
            memcpy(auto_scheduler_results->featurization.data(), out.str().data(), out.str().size());
        }
    }

    aslog(1) << "Number of states added: " << stats.num_states_added << '\n';
    aslog(1) << "Number of featurizations computed: " << stats.num_featurizations << '\n';
    aslog(1) << "Number of memoization hits: " << stats.num_memoization_hits << '\n';
    aslog(1) << "Number of memoization misses: " << stats.num_memoization_misses << '\n';
    aslog(1) << "Number of block memoization hits: " << stats.num_block_memoization_hits << '\n';
    aslog(1) << "Number of block memoization misses: " << stats.num_block_memoization_misses << '\n';
    aslog(1) << "Total featurization time (ms): " << stats.total_featurization_time() << "\n";
    aslog(1) << "Average featurization time (ms): " << stats.average_featurization_time() << "\n";
    aslog(1) << "Total enqueue time (ms): " << stats.total_enqueue_time() << "\n";
    aslog(1) << "Total calculate cost time (ms): " << stats.total_calculate_cost_time() << "\n";
    aslog(1) << "Total feature write time (ms): " << stats.total_feature_write_time() << "\n";
    aslog(1) << "Total generate children time (ms): " << stats.total_generate_children_time() << "\n";
    aslog(1) << "Total compute in tiles time (ms): " << stats.total_compute_in_tiles_time() << "\n";
    aslog(1) << "Total filter thread tiles time (ms): " << stats.total_filter_thread_tiles_time() << "\n";
    aslog(1) << "Total filter parallel tiles time (ms): " << stats.total_filter_parallel_tiles_time() << "\n";

    aslog(1) << "Number of schedules evaluated by cost model: " << stats.num_schedules_enqueued << '\n';
    aslog(1) << "Total cost model evaluation time (ms): " << stats.total_cost_model_evaluation_time() << "\n";
    aslog(1) << "Average cost model evaluation time (ms): " << stats.average_cost_model_evaluation_time() << "\n";
    std::chrono::duration<double> total_time = std::chrono::high_resolution_clock::now() - start;
    aslog(1) << "Time taken for autoscheduler (s): " << std::chrono::duration_cast<std::chrono::milliseconds>(total_time).count() / 1000.0 << '\n';
}

// Halide uses a plugin architecture for registering custom
// autoschedulers. We register our autoscheduler using a static
// constructor.
struct RegisterAutoscheduler {
    RegisterAutoscheduler() {
        aslog(1) << "Registering autoscheduler 'Adams2019'...\n";
        Pipeline::add_autoscheduler("Adams2019", *this);
    }

    void operator()(Pipeline p, const Target &target, const MachineParams &params, AutoSchedulerResults *results) {
        std::vector<Function> outputs;
        for (Func f : p.outputs()) {
            outputs.push_back(f.function());
        }
        Autoscheduler::generate_schedule(outputs, target, params, results);
    }
} register_auto_scheduler;

// An alternative entrypoint for other uses
void find_and_apply_schedule(FunctionDAG &dag,
                             const std::vector<Function> &outputs,
                             const MachineParams &params,
                             const Target &target,
                             CostModel* cost_model,
                             int beam_size,
                             StageMap<ScheduleFeatures> *schedule_features) {

    std::mt19937 rng(12345);
    Statistics stats;
    IntrusivePtr<State> optimal = optimal_schedule(dag, outputs, params, target, cost_model, rng, beam_size, stats);

    // Apply the schedules
    optimal->apply_schedule(dag, params, target);

    if (schedule_features) {
        optimal->compute_featurization(dag, params, target, schedule_features, stats);
    }
}

}  // namespace Autoscheduler

// Intrusive shared ptr helpers.
template<>
RefCount &ref_count<Autoscheduler::LoopNest>(const Autoscheduler::LoopNest *t) noexcept {
    return t->ref_count;
}

template<>
void destroy<Autoscheduler::LoopNest>(const Autoscheduler::LoopNest *t) {
    delete t;
}

template<>
RefCount &ref_count<Autoscheduler::State>(const Autoscheduler::State *t) noexcept {
    return t->ref_count;
}

template<>
void destroy<Autoscheduler::State>(const Autoscheduler::State *t) {
    delete t;
}

}  // namespace Internal
}  // namespace Halide
