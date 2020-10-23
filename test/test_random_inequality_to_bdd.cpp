#include "bdd_solver.h"
//#include "bdd/bdd_min_marginal_averaging_smoothed.h"
//#include "bdd/bdd_anisotropic_diffusion.h"
#include "convert_pb_to_bdd.h"
#include <vector>
#include <random>
#include <iostream>
#include <sstream>
#include "test.h"

// TODO: rename single to random

using namespace LPMP;

// coefficients, inequality type, right_hand_side
std::tuple<std::vector<int>, ILP_input::inequality_type, int> generate_random_inequality(const std::size_t nr_vars)
{
    std::uniform_int_distribution<> d(-10,10);
    std::mt19937 gen;

    std::vector<int> coefficients;
    for(std::size_t i=0; i<nr_vars; ++i)
        coefficients.push_back( d(gen) );

    ILP_input::inequality_type ineq = ILP_input::inequality_type::smaller_equal;
    // otherwise we cannot guarantee that every variable can take both values. Note: this can be reverted once we have bdd preprocessor filtering out fixed variables
    /*
    ILP_input::inequality_type ineq = [&]() {
        const int r = d(gen);
        if(r > 2)
            return ILP_input::inequality_type::smaller_equal;
        else if(r < -2)
            return ILP_input::inequality_type::greater_equal;
        else
            return ILP_input::inequality_type::equal;
    }();
    */

    // make right hand side so that every variable can take both 0 and 1
    int sum_negative = 0;
    for(auto c : coefficients)
        sum_negative += std::min(c,0);
    int max_positive = 0;
    for(auto c : coefficients)
        max_positive = std::max(c, max_positive);

    int rhs = std::max(sum_negative + max_positive, d(gen)); 

    return {coefficients, ineq, rhs}; 
}

std::vector<double> generate_random_costs(const std::size_t nr_vars)
{
    std::uniform_int_distribution<> d(-10,10);
    std::mt19937 gen;

    std::vector<double> coefficients;
    for(std::size_t i=0; i<nr_vars; ++i)
        coefficients.push_back( d(gen) ); 

    return coefficients;
}

template<typename LHS_ITERATOR, typename COST_ITERATOR, typename SOL_ITERATOR>
double min_cost_impl(LHS_ITERATOR lhs_begin, LHS_ITERATOR lhs_end, const ILP_input::inequality_type ineq, const int rhs, COST_ITERATOR cost_begin, COST_ITERATOR cost_end, SOL_ITERATOR sol_begin, const double partial_cost, double& best_current_sol)
{
    assert(std::distance(lhs_begin, lhs_end) == std::distance(cost_begin, cost_end));

    if(lhs_begin == lhs_end) {
        if(ineq == ILP_input::inequality_type::equal) {
            return rhs == 0 ? 0.0 : std::numeric_limits<double>::infinity();
        } else if(ineq == ILP_input::inequality_type::smaller_equal) {
            return rhs >= 0 ? 0.0 : std::numeric_limits<double>::infinity();
        } else if(ineq == ILP_input::inequality_type::greater_equal) {
            return rhs <= 0 ? 0.0 : std::numeric_limits<double>::infinity();
        } 
    }

    const double zero_cost = min_cost_impl(lhs_begin+1, lhs_end, ineq, rhs, cost_begin+1, cost_end, sol_begin+1, partial_cost, best_current_sol);
    const double one_cost = min_cost_impl(lhs_begin+1, lhs_end, ineq, rhs - *lhs_begin, cost_begin+1, cost_end, sol_begin+1, partial_cost + *cost_begin, best_current_sol) + *cost_begin;

    const double sub_tree_cost = std::min(zero_cost, one_cost);
    const double cur_cost = partial_cost + sub_tree_cost;
    if(cur_cost <= best_current_sol) {
        best_current_sol = cur_cost;
        *sol_begin = zero_cost < one_cost ? 0 : 1; 
    }

    return std::min(zero_cost, one_cost);
}

template<typename LHS_ITERATOR, typename COST_ITERATOR>
std::tuple<double, std::vector<char>> min_cost(LHS_ITERATOR lhs_begin, LHS_ITERATOR lhs_end, const ILP_input::inequality_type ineq, const int rhs, COST_ITERATOR cost_begin, COST_ITERATOR cost_end)
{
    std::vector<char> sol(std::distance(lhs_begin, lhs_end));

    double opt_val = std::numeric_limits<double>::infinity();
    const double opt_val_2 = min_cost_impl(lhs_begin, lhs_end, ineq, rhs, cost_begin, cost_end, sol.begin(), 0.0, opt_val);
    assert(opt_val == opt_val_2);

    return {opt_val, sol};
}

template<typename LHS_ITERATOR, typename COST_ITERATOR>
double exp_sum_impl(LHS_ITERATOR lhs_begin, LHS_ITERATOR lhs_end, const ILP_input::inequality_type ineq, const int rhs, COST_ITERATOR cost_begin, COST_ITERATOR cost_end, const double partial_sum)
{
    assert(std::distance(lhs_begin, lhs_end) == std::distance(cost_begin, cost_end));

    if(lhs_begin == lhs_end) {
        if(ineq == ILP_input::inequality_type::equal) {
            return rhs == 0 ? std::exp(partial_sum) : 0.0;
        } else if(ineq == ILP_input::inequality_type::smaller_equal) {
            return rhs >= 0 ? std::exp(partial_sum) : 0.0;
        } else if(ineq == ILP_input::inequality_type::greater_equal) {
            return rhs <= 0 ? std::exp(partial_sum) : 0.0;
        } 
    }

    const double zero_cost = exp_sum_impl(lhs_begin+1, lhs_end, ineq, rhs, cost_begin+1, cost_end, partial_sum);
    const double one_cost = exp_sum_impl(lhs_begin+1, lhs_end, ineq, rhs - *lhs_begin, cost_begin+1, cost_end, partial_sum - *cost_begin);

    return zero_cost + one_cost;
}

template<typename LHS_ITERATOR, typename COST_ITERATOR>
double log_exp(LHS_ITERATOR lhs_begin, LHS_ITERATOR lhs_end, const ILP_input::inequality_type ineq, const int rhs, COST_ITERATOR cost_begin, COST_ITERATOR cost_end)
{
    const double sum = exp_sum_impl(lhs_begin, lhs_end, ineq, rhs, cost_begin, cost_end, 0.0);
    return -std::log(sum);
} 

void test_random_inequality_min_sum()
{
    for(size_t nr_vars=3; nr_vars<=15; ++nr_vars)
    {
        const auto [coefficients, ineq, rhs] = generate_random_inequality(nr_vars);
        for(const auto c : coefficients) {
            std::cout << c << " ";
        }
        if(ineq == ILP_input::inequality_type::equal)
            std::cout << " = ";
        if(ineq == ILP_input::inequality_type::smaller_equal)
            std::cout << " <= ";
        if(ineq == ILP_input::inequality_type::greater_equal)
            std::cout << " >= ";
        std::cout << rhs << "\n";

        ILP_input ilp;
        ilp.begin_new_inequality();
        for(size_t i=0; i<coefficients.size(); ++i)
        {
            ilp.add_new_variable("x" + std::to_string(i));
            ilp.add_to_constraint(coefficients[i], i);
        }
        ilp.set_inequality_type(ineq);
        ilp.set_right_hand_side(rhs);

        const std::vector<double> costs = generate_random_costs(nr_vars);
        std::cout << "cost: ";
        for(const auto x : costs)
            std::cout << x << " ";
        std::cout << "\n"; 
        for(size_t i=0; i<costs.size(); ++i)
            ilp.add_to_objective(costs[i], i); 

        std::stringstream ss;
        ilp.write(ss);
        const std::string ilp_string = ss.str();

        bdd_solver decomp_mma({
            "--input_string", ilp_string,
            "-s", "decomposition_mma",
            "--nr_threads", "2",
            "--max_iter", "20",
            "--parallel_message_passing_weight", "1.0" 
                });
        decomp_mma.solve();

        bdd_solver mma({
            "--input_string", ilp_string,
            "-s", "mma",
            "--max_iter", "20"
            });
        mma.solve();

        test(std::abs(decomp_mma.lower_bound() - mma.lower_bound()) <= 1e-8);

        const auto [enumeration_lb, sol] = min_cost(coefficients.begin(), coefficients.end(), ineq, rhs, costs.begin(), costs.end());
        std::cout << "enumeration lb = " << enumeration_lb << ", backward lb = " << mma.lower_bound() << "\n";
        test(std::abs(mma.lower_bound() - enumeration_lb) <= 1e-8);
        std::cout << "cost of primal = " << ilp.evaluate(sol.begin(), sol.end()) << "\n";
        std::cout << "primal size = " << sol.size() << "\n";
        for(const auto x : sol)
            std::cout << int(x) << " ";
        std::cout << "\n";
        test(std::abs(enumeration_lb - ilp.evaluate(sol.begin(), sol.end())) <= 1e-8);

    } 
}

/*
void test_random_inequality_log_exp()
{
    BDD::bdd_mgr bdd_mgr;
    bdd_converter converter(bdd_mgr);

    for(std::size_t nr_vars = 3; nr_vars <= 15; ++nr_vars) {
        const auto [coefficients, ineq, rhs] = generate_random_inequality(nr_vars);
        for(const auto c : coefficients) {
            std::cout << c << " ";
        }
        if(ineq == ILP_input::inequality_type::equal)
            std::cout << " = ";
        if(ineq == ILP_input::inequality_type::smaller_equal)
            std::cout << " <= ";
        if(ineq == ILP_input::inequality_type::greater_equal)
            std::cout << " >= ";
        std::cout << rhs << "\n";

        auto bdd = converter.convert_to_bdd(coefficients.begin(), coefficients.end(), ineq, rhs);
        if(bdd.nr_nodes() < 2) 
            continue;
        bdd_min_marginal_averaging_smoothed bdds;
        std::vector<std::size_t> vars(nr_vars);
        std::iota (std::begin(vars), std::end(vars), 0);
        bdds.add_bdd(bdd, vars.begin(), vars.end(), bdd_mgr);
        //bdds.export_dot(std::cout);
        bdds.init(); 
        const std::vector<double> costs = generate_random_costs(nr_vars);
        std::cout << "cost: ";
        for(const auto x : costs)
            std::cout << x << " ";
        std::cout << "\n"; 
        bdds.set_costs(costs.begin(), costs.end());
        const double backward_lb = bdds.compute_smooth_lower_bound();
        bdds.forward_run();
        const double forward_lb = bdds.compute_smooth_lower_bound_forward();
        const double enumeration_lb = log_exp(coefficients.begin(), coefficients.end(), ineq, rhs, costs.begin(), costs.end());
        std::cout << "enumeration lb = " << enumeration_lb << ", backward lb = " << backward_lb << ", forward lb = " << forward_lb << "\n";
        test(std::abs(backward_lb - forward_lb) <= 1e-8);
        test(std::abs(backward_lb - enumeration_lb) <= 1e-8);
    } 
}
*/

int main(int argc, char** arv)
{
    //test_random_inequality_log_exp();
    test_random_inequality_min_sum();
    //test_random_inequality_min_sum<bdd_anisotropic_diffusion>();
}
