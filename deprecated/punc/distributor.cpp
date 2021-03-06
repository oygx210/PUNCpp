#include "distributor.h"

namespace punc
{

std::vector<double> voronoi_volume_approx(std::shared_ptr<df::FunctionSpace> &V)
{
    auto num_dofs = V->dim();
    auto dof_indices = df::vertex_to_dof_map(*V);
    std::vector<double> volumes(num_dofs, 0.0);

    auto mesh = V->mesh();
    auto tdim = mesh->topology().dim();
    auto gdim = mesh->geometry().dim();
    int j = 0;
    mesh->init(0, tdim);
    for (df::MeshEntityIterator e(*mesh, 0); !e.end(); ++e)
    {
        auto cells = e->entities(tdim);
        auto num_cells = e->num_entities(tdim);
        for (std::size_t i = 0; i < num_cells; ++i)
        {
            df::Cell cell(*mesh, e->entities(tdim)[i]);
            volumes[dof_indices[j]] += cell.volume();
        }
        j++;
    }
    for (std::size_t i = 0; i < num_dofs; ++i)
    {
        volumes[i] = (gdim + 1.0) / volumes[i];
    }
    return volumes;
}

std::shared_ptr<df::Function> distribute(std::shared_ptr<df::FunctionSpace> &V,
                                         Population &pop,
                                         const std::vector<double> &dv_inv)
{
    auto mesh = V->mesh();
    auto tdim = mesh->topology().dim();
    auto rho = std::make_shared<df::Function>(V);
    auto rho_vec = rho->vector();
    std::size_t len_rho = rho_vec->size();
    std::vector<double> rho0(len_rho);
    rho_vec->get_local(rho0);

    auto element = V->element();
    auto s_dim = element->space_dimension();

    std::vector<double> basis_matrix(s_dim);
    std::vector<double> vertex_coordinates;

    for (df::MeshEntityIterator e(*mesh, tdim); !e.end(); ++e)
    {
        auto cell_id = e->index();
        df::Cell _cell(*mesh, cell_id);
        _cell.get_vertex_coordinates(vertex_coordinates);
        auto cell_orientation = _cell.orientation();
        auto dof_id = V->dofmap()->cell_dofs(cell_id);

        std::vector<double> basis(1);
        std::vector<double> accum(s_dim, 0.0);

        std::size_t num_particles = pop.cells[cell_id].particles.size();
        for (std::size_t p_id = 0; p_id < num_particles; ++p_id)
        {
            auto particle = pop.cells[cell_id].particles[p_id];

            for (std::size_t i = 0; i < s_dim; ++i)
            {
                element->evaluate_basis(i, basis.data(),
                                        particle.x.data(),
                                        vertex_coordinates.data(),
                                        cell_orientation);
                basis_matrix[i] = basis[0];
                accum[i] += particle.q * basis_matrix[i];
            }

        }
        for (std::size_t i = 0; i < s_dim; ++i)
        {
            rho0[dof_id[i]] += accum[i];
        }
    }
    for (std::size_t i = 0; i < len_rho; ++i)
    {
        rho0[i] *= dv_inv[i];
    }
    rho->vector()->set_local(rho0);
    return rho;
}

}
