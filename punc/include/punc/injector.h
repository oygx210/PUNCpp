// Copyright (C) 2018, Diako Darian and Sigvald Marholm
//
// This file is part of PUNC++.
//
// PUNC++ is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// PUNC++ is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// PUNC++. If not, see <http://www.gnu.org/licenses/>.

/**
 * @file		injector.h
 * @brief		Particle injector
 *
 * Velocity distribution functions and functions for injecting particles.
 */

#ifndef INJECTOR_H
#define INJECTOR_H

#include "population.h"
#include <random>

namespace punc
{

namespace df = dolfin;

/**
 * @brief Generates random numbers to seed std::mt19937_64, i.e., the Mersenne 
 * Twister pseudo-random generator of 64-bit numbers
 */
class RandomSeed
{
public:
    /**
     * @brief generates a sequence of random numbers using std::random_device
     */
    template <typename T>
    void generate(T begin, T end)
    {
        for (; begin != end; ++begin)
        {
            *begin = device();
        }
    }
    /**
     * @brief Used to initialize std::mt19937_64
     */
    static RandomSeed &get_instance()
    {
        static thread_local RandomSeed seed;
        return seed;
    }

private:
    std::random_device device; ///< The random device to seed std::mt19937_64
};

/**
 * @brief Standard rejection sampler
 * @param vs[in, out] - array of generated random samples
 * @param N[in] - number of random numbers to be generated
 * @param pdf[in] - a probability distribution function to sample from
 * @param pdf_max[in] - maximum value of pdf
 * @param dim[in] - the dimension of the space pdf is defined on
 * @param domain[in] - the domian in which the random numbers are constrained to
 * @param rand[in] - the uniform distribution function
 * @param rng[in] - The Mersenne Twister pseudo-random generator of 64-bit numbers
 * 
 * Standard rejection sampler uses a simpler proposal distribution, which in 
 * this case is a uniform distribution function, U, to generate random samples. 
 * A sample is then accepted with probability pdf(v)/ U(v), or discarded 
 * otherwise. This process is repeated until a sample is accepted.
 */
void rejection_sampler(double *vs, std::size_t N,
                       std::shared_ptr<Pdf> pdf,
                       double pdf_max, int dim,
                       const std::vector<double> &domain,
                       std::uniform_real_distribution<double> &rand,
                       std::mt19937_64 &rng);

/**
 * @brief Standard rejection sampler
 * @param vs[in, out] - array of generated random samples
 * @param N[in] - number of random numbers to be generated
 * @param n_vec[in] - normal vector to an exterior facet
 * @param pdf[in] - a probability distribution function to sample from
 * @param pdf_max[in] - maximum value of pdf
 * @param dim[in] - the dimension of the space pdf is defined on
 * @param domain[in] - the domian in which the random numbers are constrained to
 * @param rand[in] - the uniform distribution function
 * @param rng[in] - The Mersenne Twister pseudo-random generator of 64-bit numbers
 * 
 * Standard rejection sampler uses a simpler proposal distribution, which in 
 * this case is a uniform distribution function, U, to generate random samples. 
 * A sample is then accepted with probability pdf(v)/ U(v), or discarded 
 * otherwise. This process is repeated until a sample is accepted.
 */
void rejection_sampler(double *vs, std::size_t N,
                       const std::vector<double> &n_vec,
                       std::shared_ptr<Pdf> pdf,
                       double pdf_max, int dim,
                       const std::vector<double> &domain,
                       std::uniform_real_distribution<double> &rand,
                       std::mt19937_64 &rng);

/**
 * @brief Generates uniformly distributed random particle positions on a given exterior facet
 * @param xs[in, out] - a vector of sampled random positions on a given exterior facet
 * @param N[in] - number of random particle positions to be generated
 * @param vertices[in] - a vector containing all the vertices of the facet
 * @param rand[in] - the uniform distribution function
 * @param rng[in] - The Mersenne Twister pseudo-random generator of 64-bit numbers
 * 
 * In 1D, a facet is a single point. Hence, there is no point in generating 
 * uniformly distributed random particle positions on a point. Therefore, this 
 * function is only valid for 2D and 3D. In 2D, the facet is a straight line, 
 * and in 3D the facet is triangle. Random positions are generated within the 
 * facet depending on the geometrical dimension. 
 */
void random_facet_points(double *xs, std::size_t N,
                            const std::vector<double> &vertices,
                            std::uniform_real_distribution<double> &rand,
                            std::mt19937_64 &rng);

/**
 * @brief Creates flux needed for injecting particles through exterior boundary facets
 * @param  species[in] - A vector containing all the plasma species
 * @param  facets[in] - A vector containing all the exterior facets
 * 
 * For each species and for each exterior boundary facet, calculates the number 
 * of particles to be injected through the facet. In addition, for each facet
 * finds the maximum value of the flux probability distribution function by 
 * using Monte Carlo integration. This value is needed in the rejection sampler.
 */
void create_flux(std::vector<Species> &species, std::vector<ExteriorFacet> &facets);

/**
 * @brief Injects particles through the exterior boundary facets
 * @param pop[in, out] - the plasma particle population
 * @param species[in] - a vector containing all the plasma species
 * @param facets[in] - a vector containing all the exterior facets
 * @param dt[in] - duration of a time-step 
 * 
 * Generates random particle velocities and positions for each species from each
 * exterior boundary facet, and adds the newly created particles to plasma 
 * population. 
 */
template <typename PopulationType>
void inject_particles(PopulationType &pop, std::vector<Species> &species,
                      std::vector<ExteriorFacet> &facets, double dt)
{
    std::mt19937_64 rng{RandomSeed::get_instance()};
    std::uniform_real_distribution<double> rand(0.0, 1.0);

    auto dim = pop.g_dim;
    auto num_species = species.size();
    auto num_facets = facets.size();

    for (std::size_t i = 0; i < num_species; ++i)
    {
        auto num = species[i].vdf->num_particles;

        for (auto &num_i : num)
        {
            num_i *= species[i].n * dt;
        }
        std::vector<std::size_t> num_particles(num.begin(), num.end());
        std::size_t tot_num = std::accumulate(num_particles.begin(), num_particles.end(), 0);
        tot_num += num.size();
        std::vector<double> xs(tot_num * dim), vs(tot_num * dim);
        double *xs_ptr = xs.data();
        double *vs_ptr = vs.data();
        std::size_t tot_N = 0;
        for (std::size_t j = 0; j < num_facets; ++j)
        {
            auto normal_i = facets[j].normal;
            auto N_float = num[j];
            auto N = static_cast<std::size_t>(N_float);
            if (rand(rng) < (N_float - N))
            {
                N += 1;
            }

            auto pdf_max = species[i].vdf->pdf_max[j];

            random_facet_points(xs_ptr, N, facets[j].vertices, rand, rng);
            rejection_sampler(vs_ptr, N, normal_i, species[i].vdf, pdf_max, species[i].vdf->dim,
                              species[i].vdf->domain, rand, rng);

            if (j < num_facets - 1)
            {
                vs_ptr += N * dim;
                xs_ptr += N * dim;
            }
            tot_N += N;
        }
        xs_ptr = &xs[0];
        vs_ptr = &vs[0];

        std::size_t num_inside = 0;
        for (std::size_t k = 0; k < tot_N; ++k)
        {
            auto r = rand(rng);
            for (std::size_t l = 0; l < dim; ++l)
            {
                xs_ptr[l] = xs[k * dim + l] + r * dt * vs[k * dim + l];
                vs_ptr[l] = vs[k * dim + l];
            }
            if (pop.locate(&xs[num_inside * dim]) >= 0)
            {
                if (k < tot_N - 1)
                {
                    xs_ptr += dim;
                    vs_ptr += dim;
                }
                num_inside++;
            }
        }
        xs.resize(num_inside * dim);
        vs.resize(num_inside * dim);
        pop.add_particles(xs, vs, species[i].q, species[i].m);
    }
}

/**
 * @brief Generates particle velocities and positions for each species, and populates the simultion domain
 * @param pop[in, out] - the plasma particle population
 * @param species[in] - a vector containing all the plasma species
 * 
 * Generates random particle velocities based on the prespecified velocity
 * distribution function for each species, and populates the simulation domain
 * by generating uniformly distributed random positions in the entire domain.
 * The newly created particles are then added to the plasma population.
 */
template <typename PopulationType>
void load_particles(PopulationType &pop, std::vector<Species> &species)
{
    std::mt19937_64 rng{RandomSeed::get_instance()};
    std::uniform_real_distribution<double> rand(0.0, 1.0);

    auto num_species = species.size();
    std::vector<double> xs, vs;
    for (std::size_t i = 0; i < num_species; ++i)
    {
        auto s = species[i];
        auto dim = s.vdf->dim;
        auto N = s.num;
        std::vector<double> xs(N * dim, 0.0), vs(N * dim, 0.0);

        rejection_sampler(xs.data(), N, s.pdf, s.pdf->max(), dim, s.pdf->domain, rand, rng);
        if (s.vdf->has_icdf)
        {
            for (auto j = 0; j < dim; ++j)
            {
                for (auto k = 0; k < s.num; ++k)
                {
                    vs[k * dim + j] = rand(rng);
                }
            }
            s.vdf->icdf(vs.data(), N);
        }
        else
        {
            rejection_sampler(vs.data(), N, s.vdf, s.vdf->max(), dim, s.vdf->domain, rand, rng);
        }

        pop.add_particles(xs, vs, s.q, s.m);
    }
}

} // namespace punc

#endif // INJECTOR_H
