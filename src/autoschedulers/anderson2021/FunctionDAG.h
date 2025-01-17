/** This file defines the class FunctionDAG, which is our
 * representation of a Halide pipeline, and contains methods to using
 * Halide's bounds tools to query properties of it. */

#ifndef FUNCTION_DAG_H
#define FUNCTION_DAG_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Errors.h"
#include "Featurization.h"
#include "Halide.h"
#include "PerfectHashMap.h"

namespace Halide {
namespace Internal {
namespace Autoscheduler {

using std::map;
using std::pair;
using std::string;
using std::unique_ptr;
using std::vector;

// First we have various utility classes.

// An optional rational type used when analyzing memory dependencies.
struct OptionalRational {
    int32_t numerator = 0, denominator = 0;

    bool exists() const {
        return denominator != 0;
    }

    OptionalRational() = default;
    OptionalRational(int64_t n, int64_t d)
        : numerator(n), denominator(d) {
    }

    void operator+=(const OptionalRational &other) {
        if ((denominator & other.denominator) == 0) {
            numerator = denominator = 0;
            return;
        }
        if (denominator == other.denominator) {
            numerator += other.numerator;
            return;
        }

        int64_t l = lcm(denominator, other.denominator);
        numerator *= l / denominator;
        denominator = l;
        numerator += other.numerator * (l / other.denominator);
        int64_t g = gcd(numerator, denominator);
        numerator /= g;
        denominator /= g;
    }

    OptionalRational operator*(int64_t factor) const {
        if ((*this) == 0) {
            return *this;
        }
        int64_t num = numerator * factor;
        return OptionalRational{num, denominator};
    }

    OptionalRational operator*(const OptionalRational &other) const {
        if ((*this) == 0) {
            return *this;
        }
        if (other == 0) {
            return other;
        }
        int64_t num = numerator * other.numerator;
        int64_t den = denominator * other.denominator;
        return OptionalRational{num, den};
    }

    // Because this type is optional (exists may be false), we don't
    // have a total ordering. These methods all return false when the
    // operators are not comparable, so a < b is not the same as !(a
    // >= b).
    bool operator<(int x) const {
        if (denominator == 0) {
            return false;
        } else if (denominator > 0) {
            return numerator < x * denominator;
        } else {
            return numerator > x * denominator;
        }
    }

    bool operator<=(int x) const {
        if (denominator == 0) {
            return false;
        } else if (denominator > 0) {
            return numerator <= x * denominator;
        } else {
            return numerator >= x * denominator;
        }
    }

    bool operator>(int x) const {
        if (!exists()) {
            return false;
        }
        return !((*this) <= x);
    }

    bool operator>=(int x) const {
        if (!exists()) {
            return false;
        }
        return !((*this) < x);
    }

    bool operator==(int x) const {
        return exists() && (numerator == (x * denominator));
    }

    bool operator==(const OptionalRational &other) const {
        return (exists() == other.exists()) && (numerator * other.denominator == denominator * other.numerator);
    }
};

// A LoadJacobian records the derivative of the coordinate accessed in
// some producer w.r.t the loops of the consumer.
class LoadJacobian {
    std::vector<OptionalRational> coeffs;
    int64_t c;
    size_t rows, cols;

public:
    LoadJacobian(size_t producer_storage_dims, size_t consumer_loop_dims, int64_t count)
        : c(count), rows(producer_storage_dims), cols(consumer_loop_dims) {
        coeffs.resize(rows * cols);
    }

    bool all_coeffs_exist() const {
        for (const auto &coeff : coeffs) {
            if (!coeff.exists()) {
                return false;
            }
        }
        return true;
    }

    bool empty() const {
        return rows == 0;
    }

    size_t producer_storage_dims() const {
        return rows;
    }

    size_t consumer_loop_dims() const {
        return cols;
    }

    bool is_constant() const {
        for (const auto &c : coeffs) {
            if (!c.exists() || !(c == 0)) {
                return false;
            }
        }

        return true;
    }

    OptionalRational operator()(int producer_storage_dim, int consumer_loop_dim) const {
        if (producer_storage_dims() == 0 || consumer_loop_dims() == 0) {
            // The producer or consumer is scalar, so all strides are zero.
            return {0, 1};
        }
        return coeffs[producer_storage_dim * cols + consumer_loop_dim];
    }

    OptionalRational &operator()(int producer_storage_dim, int consumer_loop_dim) {
        return coeffs[producer_storage_dim * cols + consumer_loop_dim];
    }

    // To avoid redundantly re-recording copies of the same
    // load Jacobian, we keep a count of how many times a
    // load with this Jacobian occurs.
    int64_t count() const {
        return c;
    }

    // Try to merge another LoadJacobian into this one, increasing the
    // count if the coefficients match.
    bool merge(const LoadJacobian &other) {
        if (other.rows != rows || other.cols != cols) {
            return false;
        }
        for (size_t i = 0; i < rows * cols; i++) {
            if (!(other.coeffs[i] == coeffs[i])) {
                return false;
            }
        }
        c += other.count();
        return true;
    }

    // Scale the matrix coefficients by the given factors
    LoadJacobian operator*(const std::vector<int64_t> &factors) const {
        LoadJacobian result(rows, cols, c);
        for (size_t i = 0; i < producer_storage_dims(); i++) {
            for (size_t j = 0; j < consumer_loop_dims(); j++) {
                result(i, j) = (*this)(i, j) * factors[j];
            }
        }
        return result;
    }

    // Multiply Jacobians, used to look at memory dependencies through
    // inlined functions.
    LoadJacobian operator*(const LoadJacobian &other) const {
        LoadJacobian result(producer_storage_dims(), other.consumer_loop_dims(), count() * other.count());
        for (size_t i = 0; i < producer_storage_dims(); i++) {
            for (size_t j = 0; j < other.consumer_loop_dims(); j++) {
                result(i, j) = OptionalRational{0, 1};
                for (size_t k = 0; k < consumer_loop_dims(); k++) {
                    result(i, j) += (*this)(i, k) * other(k, j);
                }
            }
        }
        return result;
    }

    void dump(const char *prefix) const;
};

// Classes to represent a concrete set of bounds for a Func. A Span is
// single-dimensional, and a Bound is a multi-dimensional box. For
// each dimension we track the estimated size, and also whether or not
// the size is known to be constant at compile-time. For each Func we
// track three different types of bounds:

// 1) The region required by consumers of the Func, which determines
// 2) The region actually computed, which in turn determines
// 3) The min and max of all loops in the loop next.

// 3 in turn determines the region required of the inputs to a Func,
// which determines their region computed, and hence their loop nest,
// and so on back up the Function DAG from outputs back to inputs.

class Span {
    int64_t min_, max_;
    bool constant_extent_;

public:
    int64_t min() const {
        return min_;
    }
    int64_t max() const {
        return max_;
    }
    int64_t extent() const {
        return max_ - min_ + 1;
    }
    bool constant_extent() const {
        return constant_extent_;
    }

    void union_with(const Span &other) {
        min_ = std::min(min_, other.min());
        max_ = std::max(max_, other.max());
        constant_extent_ = constant_extent_ && other.constant_extent();
    }

    void set_extent(int64_t e) {
        max_ = min_ + e - 1;
    }

    void translate(int64_t x) {
        min_ += x;
        max_ += x;
    }

    Span(int64_t a, int64_t b, bool c)
        : min_(a), max_(b), constant_extent_(c) {
    }
    Span() = default;
    Span(const Span &other) = default;
    static Span empty_span() {
        return Span(INT64_MAX, INT64_MIN, true);
    }
};

// Bounds objects are created and destroyed very frequently while
// exploring scheduling options, so we have a custom allocator and
// memory pool. Much like IR nodes, we treat them as immutable once
// created and wrapped in a Bound object so that they can be shared
// safely across scheduling alternatives.
struct BoundContents {
    mutable RefCount ref_count;

    class Layout;
    const Layout *layout = nullptr;

    Span *data() const {
        // This struct is a header
        return (Span *)(const_cast<BoundContents *>(this) + 1);
    }

    Span &region_required(int i) {
        return data()[i];
    }

    Span &region_computed(int i) {
        return data()[i + layout->computed_offset];
    }

    Span &loops(int i, int j) {
        return data()[j + layout->loop_offset[i]];
    }

    const Span &region_required(int i) const {
        return data()[i];
    }

    const Span &region_computed(int i) const {
        return data()[i + layout->computed_offset];
    }

    const Span &loops(int i, int j) const {
        return data()[j + layout->loop_offset[i]];
    }

    BoundContents *make_copy() const {
        auto *b = layout->make();
        size_t bytes = sizeof(data()[0]) * layout->total_size;
        memcpy(b->data(), data(), bytes);
        return b;
    }

    void validate() const;

    // We're frequently going to need to make these concrete bounds
    // arrays.  It makes things more efficient if we figure out the
    // memory layout of those data structures once ahead of time, and
    // make each individual instance just use that. Note that this is
    // not thread-safe.
    class Layout {
        // A memory pool of free BoundContent objects with this layout
        mutable std::vector<BoundContents *> pool;

        // All the blocks of memory allocated
        mutable std::vector<void *> blocks;

        mutable size_t num_live = 0;

        void allocate_some_more() const;

    public:
        // number of Span to allocate
        int total_size;

        // region_computed comes next at the following index
        int computed_offset;

        // the loop for each stage starts at the following index
        std::vector<int> loop_offset;

        Layout() = default;
        ~Layout();

        Layout(const Layout &) = delete;
        void operator=(const Layout &) = delete;
        Layout(Layout &&) = delete;
        void operator=(Layout &&) = delete;

        // Make a BoundContents object with this layout
        BoundContents *make() const;

        // Release a BoundContents object with this layout back to the pool
        void release(const BoundContents *b) const;
    };
};

using Bound = IntrusivePtr<const BoundContents>;

// A representation of the function DAG. The nodes and edges are both
// in reverse realization order, so if you want to walk backwards up
// the DAG, just iterate the nodes or edges in-order.
struct FunctionDAG {

    // An edge is a producer-consumer relationship
    struct Edge;

    struct SymbolicInterval {
        Halide::Var min;
        Halide::Var max;
    };

    // A Node represents a single Func
    struct Node {
        // A pointer back to the owning DAG
        FunctionDAG *dag;

        // The Halide Func this represents
        Function func;

        // The number of bytes per point stored.
        double bytes_per_point;

        // The min/max variables used to denote a symbolic region of
        // this Func. Used in the cost above, and in the Edges below.
        vector<SymbolicInterval> region_required;

        // A concrete region required from a bounds estimate. Only
        // defined for outputs.
        vector<Span> estimated_region_required;

        // The region computed of a Func, in terms of the region
        // required. For simple Funcs this is identical to the
        // region_required. However, in some Funcs computing one
        // output requires computing other outputs too. You can't
        // really ask for a single output pixel from something blurred
        // with an IIR without computing the others, for example.
        struct RegionComputedInfo {
            // The min and max in their full symbolic glory. We use
            // these in the general case.
            Interval in;

            // Analysis used to accelerate common cases
            bool equals_required = false, equals_union_of_required_with_constants = false;
            int64_t c_min = 0, c_max = 0;
        };
        vector<RegionComputedInfo> region_computed;
        bool region_computed_all_common_cases = false;

        // Expand a region required into a region computed, using the
        // symbolic intervals above.
        void required_to_computed(const Span *required, Span *computed) const;

        // Metadata about one symbolic loop in a Func's default loop nest.
        struct Loop {
            string var;
            bool pure, rvar;
            Expr min, max;

            // Which pure dimension does this loop correspond to? Invalid if it's an rvar
            int pure_dim;

            // Precomputed metadata to accelerate common cases:

            // If true, the loop bounds are just the region computed in the given dimension
            bool equals_region_computed = false;
            int region_computed_dim = 0;

            // If true, the loop bounds are a constant with the given min and max
            bool bounds_are_constant = false;
            int64_t c_min = 0, c_max = 0;

            // A persistent fragment of source for getting this Var
            // from its owner Func. Used for printing source code
            // equivalent to a computed schedule.
            string accessor;
        };

        // Get the loop nest shape as a function of the region computed
        void loop_nest_for_region(int stage_idx, const Span *computed, Span *loop) const;

        // One stage of a Func
        struct Stage {
            // The owning Node
            Node *node;

            // Which stage of the Func is this. 0 = pure.
            int index;

            // The loop nest that computes this stage, from innermost out.
            vector<Loop> loop;
            bool loop_nest_all_common_cases = false;

            // The vectorization width that will be used for
            // compute. Corresponds to the natural width for the
            // narrowest type used.
            int vector_size;

            // The featurization of the compute done
            PipelineFeatures features;

            // The actual Halide front-end stage object
            Halide::Stage stage;

            // The name for scheduling (e.g. "foo.update(3)")
            string name;

            string sanitized_name;

            // Ids for perfect hashing on stages.
            int id, max_id;

            std::unique_ptr<LoadJacobian> store_jacobian;

            vector<Edge *> incoming_edges;

            vector<bool> dependencies;
            bool downstream_of(const Node &n) const {
                return dependencies[n.id];
            };

            Stage(Halide::Stage s)
                : stage(std::move(s)) {
            }

            int get_loop_index_from_var(const std::string &var) const {
                int i = 0;
                for (const auto &l : loop) {
                    if (l.var == var) {
                        return i;
                    }

                    ++i;
                }

                return -1;
            }
        };
        vector<Stage> stages;

        vector<Edge *> outgoing_edges;

        // Max vector size across the stages
        int vector_size;

        // A unique ID for this node, allocated consecutively starting
        // at zero for each pipeline.
        int id, max_id;

        // Just func->dimensions(), but we ask for it so many times
        // that's it's worth avoiding the function call into
        // libHalide.
        int dimensions;

        // Is a single pointwise call to another Func
        bool is_wrapper;

        // We represent the input buffers as node, though we do not attempt to schedule them.
        bool is_input;

        // Is one of the pipeline outputs
        bool is_output;

        // Only uses pointwise calls
        bool is_pointwise;

        // Only uses pointwise calls + clamping on all indices
        bool is_boundary_condition;

        std::unique_ptr<BoundContents::Layout> bounds_memory_layout;

        BoundContents *make_bound() const {
            return bounds_memory_layout->make();
        }
    };

    // A representation of a producer-consumer relationship
    struct Edge {
        struct BoundInfo {
            // The symbolic expression for the bound in this dimension
            Expr expr;

            // Fields below are the results of additional analysis
            // used to evaluate this bound more quickly.
            int64_t coeff, constant;
            int64_t consumer_dim;
            bool affine, uses_max;

            BoundInfo(const Expr &e, const Node::Stage &consumer);
        };

        // Memory footprint on producer required by consumer.
        vector<pair<BoundInfo, BoundInfo>> bounds;

        FunctionDAG::Node *producer;
        FunctionDAG::Node::Stage *consumer;

        // The number of calls the consumer makes to the producer, per
        // point in the loop nest of the consumer.
        int calls;

        bool all_bounds_affine;

        vector<LoadJacobian> load_jacobians;

        bool all_load_jacobian_coeffs_exist() const;

        void add_load_jacobian(LoadJacobian j1);

        // Given a loop nest of the consumer stage, expand a region
        // required of the producer to be large enough to include all
        // points required.
        void expand_footprint(const Span *consumer_loop, Span *producer_required) const;
    };

    vector<Node> nodes;
    vector<Edge> edges;

    int num_non_input_nodes{0};

    // We're going to be querying this DAG a lot while searching for
    // an optimal schedule, so we'll also create a variety of
    // auxiliary data structures.
    map<int, const Node *> stage_id_to_node_map;

    // Create the function DAG, and do all the dependency and cost
    // analysis. This is done once up-front before the tree search.
    FunctionDAG(const vector<Function> &outputs, const Target &target);

    void dump() const;
    std::ostream &dump(std::ostream &os) const;

    // This class uses a lot of internal pointers, so we'll hide the copy constructor.
    FunctionDAG(const FunctionDAG &other) = delete;
    void operator=(const FunctionDAG &other) = delete;

private:
    // Compute the featurization for the entire DAG
    void featurize();

    template<typename OS>
    void dump_internal(OS &os) const;
};

template<typename T>
using NodeMap = PerfectHashMap<FunctionDAG::Node, T>;

class ExprBranching : public VariadicVisitor<ExprBranching, int, int> {
    using Super = VariadicVisitor<ExprBranching, int, int>;

private:
    const NodeMap<int64_t> &inlined;

public:
    int visit(const Reinterpret *op);
    int visit(const IntImm *op);
    int visit(const UIntImm *op);
    int visit(const FloatImm *op);
    int visit(const StringImm *op);
    int visit(const Broadcast *op);
    int visit(const Cast *op);
    int visit(const Variable *op);
    int visit(const Add *op);
    int visit(const Sub *op);
    int visit(const Mul *op);
    int visit(const Div *op);
    int visit(const Mod *op);
    int visit(const Min *op);
    int visit(const Max *op);
    int visit(const EQ *op);
    int visit(const NE *op);
    int visit(const LT *op);
    int visit(const LE *op);
    int visit(const GT *op);
    int visit(const GE *op);
    int visit(const And *op);
    int visit(const Or *op);
    int visit(const Not *op);
    int visit(const Select *op);
    int visit(const Ramp *op);
    int visit(const Load *op);
    int visit(const Call *op);
    int visit(const Shuffle *op);
    int visit(const Let *op);
    int visit(const VectorReduce *op);
    int visit_binary(const Expr &a, const Expr &b);
    int visit_nary(const std::vector<Expr> &exprs);

    ExprBranching(const NodeMap<int64_t> &inlined)
        : inlined{inlined} {
    }

    int compute(const Function &f);
};

void sanitize_names(std::string &str);

}  // namespace Autoscheduler
}  // namespace Internal
}  // namespace Halide

#endif  // FUNCTION_DAG_H
