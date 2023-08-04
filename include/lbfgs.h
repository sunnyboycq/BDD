#pragma once

#include <vector>
#include "bdd_collection/bdd_collection.h"
#include "time_measure_util.h"
#include "bdd_logging.h"
#include <deque>
#ifdef WITH_CUDA
// #include "cuda_utils.h"
#include <thrust/for_each.h>
#include <thrust/inner_product.h>
#include <thrust/host_vector.h>
#include <thrust/device_vector.h>
#endif

namespace LPMP {

#ifdef WITH_CUDA
using namespace thrust::placeholders;
#endif

// LBFGS requires the following functions to be implemented in the SOLVER base class:
// VECTOR bdd_solutions_vec()
// void make_dual_feasible(VECTOR)
// VECTOR net_solver_costs()
// size_t nr_layers()
// void gradient_step(VECTOR)

template<class SOLVER, typename VECTOR, typename REAL>
class lbfgs : public SOLVER
{
    public:
        lbfgs(const BDD::bdd_collection& bdd_col, const int _history_size, 
        const double _init_step_size = 1e-6, const double _req_rel_lb_increase = 1e-6, 
        const double _step_size_decrease_factor = 0.8, const double _step_size_increase_factor = 1.1);

        void iteration();

        template <typename ITERATOR>
        void update_costs(
            ITERATOR cost_delta_0_begin, ITERATOR cost_delta_0_end,
            ITERATOR cost_delta_1_begin, ITERATOR cost_delta_1_end);
#ifdef WITH_CUDA
        template<typename REAL_arg>
        void update_costs(const thrust::device_vector<REAL_arg> &cost_0, const thrust::device_vector<REAL_arg> &cost_1);
#endif

    private:
        void store_iterate();
        VECTOR compute_update_direction();
        void search_step_size_and_apply(const VECTOR &update);
        void flush_lbfgs_states();
        bool lbfgs_update_possible() const;
        void next_itr_without_storage();

        struct history
        {
            VECTOR s, y; // difference of x, grad_x f(x) resp.
            REAL rho_inv;
        };
        std::deque<history> history;

        VECTOR prev_x, prev_grad_f;
        const int m;
        double step_size;
        //double init_lb_increase;
        //bool init_lb_valid = false;
        const double required_relative_lb_increase, step_size_decrease_factor, step_size_increase_factor;
        int num_unsuccessful_lbfgs_updates_ = 0;
        double initial_rho_inv = 0.0;

        std::deque<REAL> lb_history;

        bool prev_states_stored = false;
        bool initial_rho_inv_valid = false;

        double mma_lb_increase_per_time = 0.0;
        double lbfgs_lb_increase_per_time = 0.0;
        size_t mma_iterations = 0;
        size_t lbfgs_iterations = 0;

        enum class solver_type {mma, lbfgs};
        solver_type choose_solver() const;

        void mma_iteration();
        void lbfgs_iteration();
};

template <class SOLVER, typename VECTOR, typename REAL>
lbfgs<SOLVER, VECTOR, REAL>::lbfgs(const BDD::bdd_collection &bdd_col, const int _history_size,
                                   const double _init_step_size, const double _req_rel_lb_increase,
                                   const double _step_size_decrease_factor, const double _step_size_increase_factor)
    : SOLVER(bdd_col),
      m(_history_size),
      step_size(_init_step_size), required_relative_lb_increase(_req_rel_lb_increase),
      step_size_decrease_factor(_step_size_decrease_factor), step_size_increase_factor(_step_size_increase_factor)
{
        bdd_log << "[lbfgs] Initialized LBFGS with"
                << "\n[lbfgs]\thistory size: " << m
                << "\n[lbfgs]\tinitial step size " << step_size
                << "\n[lbfgs]\trequired relative lb increase " << required_relative_lb_increase
                << "\n[lbfgs]\tstep size decrease factor " << step_size_decrease_factor
                << "\n[lbfgs]\tstep size increase factor " << step_size_increase_factor
                << "\n[lbfgs]\thistory size" << m << "\n";

        assert(step_size > 0.0);
        assert(step_size_decrease_factor > 0.0 && step_size_decrease_factor < 1.0);
        assert(step_size_increase_factor > 1.0);
        assert(required_relative_lb_increase > 0.0);
        assert(m > 1);

        prev_x = VECTOR(this->nr_layers());
        prev_grad_f = VECTOR(this->nr_layers());
    }

    template <class SOLVER, typename VECTOR, typename REAL>
    void lbfgs<SOLVER, VECTOR, REAL>::store_iterate()
    {
        // TODO: Provide following two functions in all solvers.
        // should return thrust::device_vector for GPU and thrust::host_vector/std::vector for CPU solvers.
        VECTOR cur_x = this->net_solver_costs();
        VECTOR cur_grad_f = this->bdds_solution_vec();
        
        assert(cur_x.size() == prev_x.size());
        assert(cur_grad_f.size() == prev_x.size());
        if (!prev_states_stored)
        {
            prev_x = cur_x;
            prev_grad_f = cur_grad_f;
            prev_states_stored = true;
        }
        else
        {
            VECTOR cur_s(cur_x.size()); // compute x_k - x_{k-1}
#ifdef WITH_CUDA
            thrust::transform(cur_x.begin(), cur_x.end(), prev_x.begin(), cur_s.begin(), thrust::minus<REAL>());
#else
            std::transform(cur_x.begin(), cur_x.end(), prev_x.begin(), cur_s.begin(), std::minus<REAL>());
#endif
            // compute grad_f_k - grad_f_{k-1}, but since we have maximization problem and lbfgs updates are derived for minimization so multiply gradients by -1.
            VECTOR cur_y(cur_grad_f.size());
#ifdef WITH_CUDA
            thrust::transform(prev_grad_f.begin(), prev_grad_f.end(), cur_grad_f.begin(), cur_y.begin(), thrust::minus<REAL>());
            REAL rho_inv = thrust::inner_product(cur_s.begin(), cur_s.end(), cur_y.begin(), (REAL) 0.0);
#else
            std::transform(prev_grad_f.begin(), prev_grad_f.end(), cur_grad_f.begin(), cur_y.begin(), std::minus<REAL>());
            REAL rho_inv = std::inner_product(cur_s.begin(), cur_s.end(), cur_y.begin(), (REAL) 0.0);
#endif

            if (!initial_rho_inv_valid)
            {
                initial_rho_inv = rho_inv;
                initial_rho_inv_valid = true;
            }
            //if (rho_inv / initial_rho_inv > 1e-8) // otherwise, skip the iterate as curvature condition is not strongly satisfied.
            if (rho_inv > 1e-8) // otherwise, skip the iterate as curvature condition is not strongly satisfied.
            {
                history.push_back({});
                history.back().s = cur_s;
                history.back().y = cur_y;
                history.back().rho_inv = rho_inv;

                if(history.size() > m)
                {
                    history.pop_front();
                    assert(history.size() == m);
                }
            } // when skipping estimate of hessian will become out-of-date. However removing these updates as below gives worse results than not removing.
            else
            {
                //num_stored = std::max(num_stored - 1, 0);
            }
            // TODO: std::move?
            prev_x = cur_x;
            prev_grad_f = cur_grad_f;
        }
    }

    template<class SOLVER, typename VECTOR, typename REAL>
    void lbfgs<SOLVER, VECTOR, REAL>::iteration()
    {
        if (lb_history.empty())
            lb_history.push_back(this->lower_bound());

        // Check if enough history accumulated
        if (this->choose_solver() == solver_type::lbfgs)
        {
           lbfgs_iteration(); 
           //mma_iteration();
        }
        else
           mma_iteration();

        // Update LBFGS states:
        this->store_iterate();
        lb_history.push_back(this->lower_bound());
    }

    template<class SOLVER, typename VECTOR, typename REAL>
    void lbfgs<SOLVER, VECTOR, REAL>::search_step_size_and_apply(const VECTOR& update)
    {
        const REAL lb_pre = this->lower_bound();
    
        auto calculate_rel_change = [&]() {
            assert(lb_history.size() >= m);
            const double cur_lb_increase = this->lower_bound() - lb_pre;
            const double past_lb_increase = *(lb_history.rbegin()+m-2) - *(lb_history.rbegin()+m-1);
            //assert(cur_lb_increase >= 0.0);
            assert(past_lb_increase >= 0.0);
            const double ratio = cur_lb_increase / (1e-9 + past_lb_increase);
            bdd_log << "[lbfgs] cur lb increase = " << cur_lb_increase << ", past lb increase = " << past_lb_increase << ", cur/past lb increase = " << ratio << "\n";
            return ratio;
            //return (this->lower_bound() - lb_pre) / (1e-9 + this->init_lb_increase);
        };

        double prev_step_size = 0.0;
        auto apply_update = [&](const REAL new_step_size) 
        {
            double net_step_size = new_step_size - prev_step_size;
            if (net_step_size != 0.0)
                this->gradient_step(update.begin(), update.end(), net_step_size); // TODO: implement for each solver.
            prev_step_size = new_step_size;
        };

        size_t num_updates = 0;
        REAL curr_rel_change = 0.0;
        REAL best_step_size = 0.0;
        REAL best_rel_improvement = 0.0;
        do
        {
            apply_update(this->step_size);
            curr_rel_change = calculate_rel_change();
            bdd_log << "[lbfgs] perform update step with step size " << this->step_size << ", curr_rel_change: "<<curr_rel_change<<"\n";
            if (best_rel_improvement < curr_rel_change)
            {
                best_rel_improvement = curr_rel_change;
                best_step_size = this->step_size;
            }

            if (curr_rel_change <= 0.0)
                this->step_size *= this->step_size_decrease_factor;
            else if (curr_rel_change < required_relative_lb_increase)
                this->step_size *= this->step_size_increase_factor;

            if (num_updates > 5)
            {
                if (best_rel_improvement > required_relative_lb_increase / 10.0) //TODO: Have a separate parameter?
                    apply_update(best_step_size);
                else
                {
                    bdd_log<<"[lbfgs] step size selection unsuccessful.\n";
                    apply_update(0.0);
                    this->num_unsuccessful_lbfgs_updates_ += 1;
                }
                return;
            }
            num_updates++;
        } while(curr_rel_change < required_relative_lb_increase);

        if (num_updates == 1 && this->num_unsuccessful_lbfgs_updates_ == 0)
            this->step_size *= this->step_size_increase_factor;
        this->num_unsuccessful_lbfgs_updates_ = 0;
    }

    template<typename REAL>
    struct update_q
    {
        const REAL alpha;
        const REAL* y;
        REAL* q;
#ifdef WITH_CUDA
        __host__ __device__
#endif
            void
            operator()(const int idx)
        {
            q[idx] -= alpha * y[idx];
        }
    };

    template<typename REAL>
    struct update_r
    {
        const REAL alpha;
        const REAL beta;
        const REAL* s;
        REAL* r;
#ifdef WITH_CUDA
        __host__ __device__
#endif
            void
            operator()(const int idx)
        {
            r[idx] += s[idx] * (alpha - beta);
        }
    };

    template<class SOLVER, typename VECTOR, typename REAL>
    VECTOR lbfgs<SOLVER, VECTOR, REAL>::compute_update_direction()
    {
        assert(this->lbfgs_update_possible());
        //VECTOR direction(this->nr_layers());
        VECTOR direction = this->bdds_solution_vec();

        //const int n = s_history[0].size();
        assert(history.size() > 0);
        const size_t n = history.back().s.size();

        std::vector<REAL> alpha_history;
        //for (int count = 0; count < num_stored; count++)
        for (int i = history.size()-1; i >= 0; i--)
        {
#ifdef WITH_CUDA
            const REAL alpha = thrust::inner_product(history[i].s.begin(), history[i].s.end(), direction.begin(), (REAL)0.0) / (history[i].rho_inv);
#else
            const REAL alpha = std::inner_product(history[i].s.begin(), history[i].s.end(), direction.begin(), (REAL)0.0) / (history[i].rho_inv);
#endif

            alpha_history.push_back(alpha);

#ifdef WITH_CUDA
            update_q<REAL> update_q_func({alpha, thrust::raw_pointer_cast(history[i].y.data()), thrust::raw_pointer_cast(direction.data())});
            thrust::for_each(thrust::make_counting_iterator<int>(0), thrust::make_counting_iterator<int>(0) + n, update_q_func);
#else
            update_q<REAL> update_q_func({alpha, history[i].y.data(), direction.data()});
            for(size_t j = 0; j < n; ++j)
                update_q_func(j);
#endif
        }

        std::reverse(alpha_history.begin(), alpha_history.end());

#ifdef WITH_CUDA
        REAL last_y_norm = thrust::inner_product(history.back().y.begin(), history.back().y.end(), history.back().y.begin(), (REAL)0.0);
#else
        REAL last_y_norm = std::inner_product(history.back().y.begin(), history.back().y.end(), history.back().y.begin(), (REAL)0.0);
#endif
        REAL initial_H_diag_multiplier = history.back().rho_inv / (1e-8 + last_y_norm);
        // Skip line 5 in Alg.1 and fuse with line 7 for first loop itr.
        //for (int count = 0; count < history.size(); count++)
        for (int i = 0; i < history.size(); i++)
        {
            assert(history[i].y.size() == n);

            REAL current_rho = 1.0 / (history[i].rho_inv);
            if (i == 0)
                current_rho *= initial_H_diag_multiplier;
#ifdef WITH_CUDA
            const REAL beta = current_rho * thrust::inner_product(history[i].y.begin(), history[i].y.end(), direction.begin(), (REAL)0.0);
            update_r<REAL> update_r_func({alpha_history[i], beta, thrust::raw_pointer_cast(history[i].s.data()), thrust::raw_pointer_cast(direction.data())});
            thrust::for_each(thrust::make_counting_iterator<int>(0), thrust::make_counting_iterator<int>(0) + n, update_r_func);
#else
            const REAL beta = current_rho * std::inner_product(history[i].y.begin(), history[i].y.end(), direction.begin(), (REAL)0.0);
            update_r<REAL> update_r_func({alpha_history[i], beta, history[i].s.data(), direction.data()});
            for (size_t j = 0; j < n; ++j)
                update_r_func(j);
#endif
        }
        return direction;
    }

    template<class SOLVER, typename VECTOR, typename REAL>
    void lbfgs<SOLVER, VECTOR, REAL>::flush_lbfgs_states()
    {
        num_unsuccessful_lbfgs_updates_ = 0;
        history.clear();
        prev_states_stored = false;
        initial_rho_inv = 0.0;
        initial_rho_inv_valid = false;
        //init_lb_valid = false;
    }

    template<class SOLVER, typename VECTOR, typename REAL>
    void lbfgs<SOLVER, VECTOR, REAL>::next_itr_without_storage()
    {
        history.pop_front();
    }

    template<class SOLVER, typename VECTOR, typename REAL>
    bool lbfgs<SOLVER, VECTOR, REAL>::lbfgs_update_possible() const
    {
        if (history.size() < m || num_unsuccessful_lbfgs_updates_ > 5) // || !init_lb_valid)
            return false;
        return true;
    }

    template<class SOLVER, typename VECTOR, typename REAL>
    template<typename ITERATOR>
    void lbfgs<SOLVER, VECTOR, REAL>::update_costs(
        ITERATOR cost_delta_0_begin, ITERATOR cost_delta_0_end,
        ITERATOR cost_delta_1_begin, ITERATOR cost_delta_1_end)
    {
        flush_lbfgs_states();
        static_cast<SOLVER*>(this)->update_costs(cost_delta_0_begin, cost_delta_0_end, cost_delta_1_begin, cost_delta_1_end);
    }

#ifdef WITH_CUDA
    template<class SOLVER, typename VECTOR, typename REAL>
    template<typename REAL_arg>
    void lbfgs<SOLVER, VECTOR, REAL>::update_costs(const thrust::device_vector<REAL_arg>& cost_0, const thrust::device_vector<REAL_arg>& cost_1)
    {
        flush_lbfgs_states();
        static_cast<SOLVER*>(this)->update_costs(cost_0, cost_1);

    }
#endif

    template<class SOLVER, typename VECTOR, typename REAL>
    void lbfgs<SOLVER, VECTOR, REAL>::mma_iteration()
    {
        const double lb_before = this->lower_bound();
        const auto pre = std::chrono::steady_clock::now();
        static_cast<SOLVER *>(this)->iteration();
        const auto post = std::chrono::steady_clock::now();
        const double lb_after = this->lower_bound();
        mma_lb_increase_per_time = (lb_after - lb_before) / std::chrono::duration<double>(post - pre).count();
        bdd_log << "[lbfgs] mma lb increase over time = " << mma_lb_increase_per_time << "\n";
        mma_iterations++;
    }

    template<class SOLVER, typename VECTOR, typename REAL>
    void lbfgs<SOLVER, VECTOR, REAL>::lbfgs_iteration()
    {
        const double lb_before = this->lower_bound();
        const auto pre = std::chrono::steady_clock::now();

        // Compute LBFGS update direction. This can be infeasible.
        VECTOR grad_lbfgs = this->compute_update_direction();

        // Make the update direction dual feasible by making it sum to zero for all primal variables.
        this->make_dual_feasible(grad_lbfgs.begin(), grad_lbfgs.end()); // TODO: Implement for all solvers

        // Apply the update by choosing appropriate step size:
        this->search_step_size_and_apply(grad_lbfgs);
        static_cast<SOLVER*>(this)->iteration();

        const auto post = std::chrono::steady_clock::now();
        const double lb_after = this->lower_bound();
        assert(lb_after >= lb_before - 1e-6);
        lbfgs_lb_increase_per_time = (lb_after - lb_before) / std::chrono::duration<double>(post - pre).count();
        std::cout << "[lbfgs] lbfgs pre lb = " << lb_before << ", after lb = " << lb_after << "\n";
        bdd_log << "[lbfgs] lbfgs lb increase over time = " << lbfgs_lb_increase_per_time << "\n";
        lbfgs_iterations++;
    }

    template<class SOLVER, typename VECTOR, typename REAL>
    typename lbfgs<SOLVER, VECTOR, REAL>::solver_type lbfgs<SOLVER, VECTOR, REAL>::choose_solver() const
    {
        if (!lbfgs_update_possible())
        {
            bdd_log << "[lbfgs] Do mma iterations for collecting states\n";
            return solver_type::mma;
        }

        if (double(lbfgs_iterations) / double(mma_iterations + 1e-9) > 50.0)
        {
            bdd_log << "[lbfgs] Do mma iterations to estimate mma improvement\n";
            return solver_type::mma;
        }

        if(double(mma_iterations)/double(lbfgs_iterations+1e-9) > 50.0)
        {
            bdd_log << "[lbfgs] Do lbfgs iterations to estimate lbfgs improvement\n";
            return solver_type::lbfgs;
        }

        if(mma_lb_increase_per_time > 2.0 * lbfgs_lb_increase_per_time)
        {
            bdd_log << "[lbfgs] mma lb increase per time = " << mma_lb_increase_per_time << " > lbfgs lb increase per time = " << lbfgs_lb_increase_per_time << ", choose mma\n";
            return solver_type::mma;
        }
        else
        {
            bdd_log << "[lbfgs] mma lb increase per time = " << mma_lb_increase_per_time << " < lbfgs lb increase per time = " << lbfgs_lb_increase_per_time << ", choose lbfgs\n";
            return solver_type::lbfgs;
        }
    }


}
