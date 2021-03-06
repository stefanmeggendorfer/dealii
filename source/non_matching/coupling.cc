// ---------------------------------------------------------------------
//
// Copyright (C) 2018 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE.md at
// the top level directory of deal.II.
//
// ---------------------------------------------------------------------

#include <deal.II/base/exceptions.h>
#include <deal.II/base/point.h>
#include <deal.II/base/tensor.h>

#include <deal.II/distributed/shared_tria.h>
#include <deal.II/distributed/tria.h>

#include <deal.II/fe/fe_values.h>

#include <deal.II/grid/grid_tools.h>
#include <deal.II/grid/grid_tools_cache.h>

#include <deal.II/lac/block_sparse_matrix.h>
#include <deal.II/lac/block_sparsity_pattern.h>
#include <deal.II/lac/petsc_block_sparse_matrix.h>
#include <deal.II/lac/petsc_sparse_matrix.h>
#include <deal.II/lac/sparse_matrix.h>
#include <deal.II/lac/sparsity_pattern.h>
#include <deal.II/lac/trilinos_block_sparse_matrix.h>
#include <deal.II/lac/trilinos_sparse_matrix.h>
#include <deal.II/lac/trilinos_sparsity_pattern.h>

#include <deal.II/non_matching/coupling.h>

DEAL_II_NAMESPACE_OPEN
namespace NonMatching
{
  template <int dim0,
            int dim1,
            int spacedim,
            typename Sparsity,
            typename number>
  void
  create_coupling_sparsity_pattern(
    const DoFHandler<dim0, spacedim> &space_dh,
    const DoFHandler<dim1, spacedim> &immersed_dh,
    const Quadrature<dim1> &          quad,
    Sparsity &                        sparsity,
    const AffineConstraints<number> & constraints,
    const ComponentMask &             space_comps,
    const ComponentMask &             immersed_comps,
    const Mapping<dim0, spacedim> &   space_mapping,
    const Mapping<dim1, spacedim> &   immersed_mapping)
  {
    GridTools::Cache<dim0, spacedim> cache(space_dh.get_triangulation(),
                                           space_mapping);
    create_coupling_sparsity_pattern(cache,
                                     space_dh,
                                     immersed_dh,
                                     quad,
                                     sparsity,
                                     constraints,
                                     space_comps,
                                     immersed_comps,
                                     immersed_mapping);
  }



  template <int dim0,
            int dim1,
            int spacedim,
            typename Sparsity,
            typename number>
  void
  create_coupling_sparsity_pattern(
    const GridTools::Cache<dim0, spacedim> &cache,
    const DoFHandler<dim0, spacedim> &      space_dh,
    const DoFHandler<dim1, spacedim> &      immersed_dh,
    const Quadrature<dim1> &                quad,
    Sparsity &                              sparsity,
    const AffineConstraints<number> &       constraints,
    const ComponentMask &                   space_comps,
    const ComponentMask &                   immersed_comps,
    const Mapping<dim1, spacedim> &         immersed_mapping)
  {
    AssertDimension(sparsity.n_rows(), space_dh.n_dofs());
    AssertDimension(sparsity.n_cols(), immersed_dh.n_dofs());
    static_assert(dim1 <= dim0, "This function can only work if dim1 <= dim0");
    Assert((dynamic_cast<
              const parallel::distributed::Triangulation<dim1, spacedim> *>(
              &immersed_dh.get_triangulation()) == nullptr),
           ExcNotImplemented());

    const auto &space_fe    = space_dh.get_fe();
    const auto &immersed_fe = immersed_dh.get_fe();

    // Dof indices
    std::vector<types::global_dof_index> dofs(immersed_fe.dofs_per_cell);
    std::vector<types::global_dof_index> odofs(space_fe.dofs_per_cell);

    // Take care of components
    const ComponentMask space_c =
      (space_comps.size() == 0 ? ComponentMask(space_fe.n_components(), true) :
                                 space_comps);

    const ComponentMask immersed_c =
      (immersed_comps.size() == 0 ?
         ComponentMask(immersed_fe.n_components(), true) :
         immersed_comps);

    AssertDimension(space_c.size(), space_fe.n_components());
    AssertDimension(immersed_c.size(), immersed_fe.n_components());

    // Global to local indices
    std::vector<unsigned int> space_gtl(space_fe.n_components(),
                                        numbers::invalid_unsigned_int);
    std::vector<unsigned int> immersed_gtl(immersed_fe.n_components(),
                                           numbers::invalid_unsigned_int);

    for (unsigned int i = 0, j = 0; i < space_gtl.size(); ++i)
      if (space_c[i])
        space_gtl[i] = j++;

    for (unsigned int i = 0, j = 0; i < immersed_gtl.size(); ++i)
      if (immersed_c[i])
        immersed_gtl[i] = j++;

    const unsigned int n_q_points = quad.size();
    const unsigned int n_active_c =
      immersed_dh.get_triangulation().n_active_cells();
    std::vector<Point<spacedim>> all_points(n_active_c * n_q_points);
    {
      FEValues<dim1, spacedim> fe_v(immersed_mapping,
                                    immersed_fe,
                                    quad,
                                    update_quadrature_points);
      unsigned int             c = 0;
      for (const auto &cell : immersed_dh.active_cell_iterators())
        {
          // Reinitialize the cell and the fe_values
          fe_v.reinit(cell);
          const std::vector<Point<spacedim>> &x_points =
            fe_v.get_quadrature_points();

          // Copy the points to the vector
          std::copy(x_points.begin(),
                    x_points.end(),
                    all_points.begin() + c * n_q_points);
          ++c;
        }
    }
    // [TODO]: when the add_entries_local_to_global below will implement
    // the version with the dof_mask, this should be uncommented.
    //
    // // Construct a dof_mask, used to distribute entries to the sparsity
    // able< 2, bool > dof_mask(space_fe.dofs_per_cell,
    //                          immersed_fe.dofs_per_cell);
    // of_mask.fill(false);
    // or (unsigned int i=0; i<space_fe.dofs_per_cell; ++i)
    //  {
    //    const auto comp_i = space_fe.system_to_component_index(i).first;
    //    if (space_gtl[comp_i] != numbers::invalid_unsigned_int)
    //      for (unsigned int j=0; j<immersed_fe.dofs_per_cell; ++j)
    //        {
    //          const auto comp_j =
    //          immersed_fe.system_to_component_index(j).first; if
    //          (immersed_gtl[comp_j] == space_gtl[comp_i])
    //            dof_mask(i,j) = true;
    //        }
    //  }


    // Get a list of outer cells, qpoints and maps.
    const auto  cpm = GridTools::compute_point_locations(cache, all_points);
    const auto &all_cells = std::get<0>(cpm);
    const auto &maps      = std::get<2>(cpm);

    std::vector<
      std::set<typename Triangulation<dim0, spacedim>::active_cell_iterator>>
      cell_sets(n_active_c);

    for (unsigned int i = 0; i < maps.size(); ++i)
      {
        // Quadrature points should be reasonably clustered:
        // the following index keeps track of the last id
        // where the current cell was inserted
        unsigned int last_id = std::numeric_limits<unsigned int>::max();
        for (const unsigned int idx : maps[i])
          {
            // Find in which cell the point lies
            unsigned int cell_id = idx / n_q_points;
            if (last_id != cell_id)
              {
                cell_sets[cell_id].insert(all_cells[i]);
                last_id = cell_id;
              }
          }
      }

    // Now we run on each cell of the immersed
    // and build the sparsity
    unsigned int i = 0;
    for (const auto &cell : immersed_dh.active_cell_iterators())
      {
        // Reinitialize the cell
        cell->get_dof_indices(dofs);

        // List of outer cells
        const auto &cells = cell_sets[i];

        for (const auto &cell_c : cells)
          {
            // Get the ones in the current outer cell
            typename DoFHandler<dim0, spacedim>::cell_iterator ocell(*cell_c,
                                                                     &space_dh);
            // Make sure we act only on locally_owned cells
            if (ocell->is_locally_owned())
              {
                ocell->get_dof_indices(odofs);
                // [TODO]: When the following function will be implemented
                // for the case of non-trivial dof_mask, we should
                // uncomment the missing part.
                constraints.add_entries_local_to_global(
                  odofs, dofs, sparsity); //, true, dof_mask);
              }
          }
        ++i;
      }
  }



  template <int dim0, int dim1, int spacedim, typename Matrix>
  void
  create_coupling_mass_matrix(
    const DoFHandler<dim0, spacedim> &                    space_dh,
    const DoFHandler<dim1, spacedim> &                    immersed_dh,
    const Quadrature<dim1> &                              quad,
    Matrix &                                              matrix,
    const AffineConstraints<typename Matrix::value_type> &constraints,
    const ComponentMask &                                 space_comps,
    const ComponentMask &                                 immersed_comps,
    const Mapping<dim0, spacedim> &                       space_mapping,
    const Mapping<dim1, spacedim> &                       immersed_mapping)
  {
    GridTools::Cache<dim0, spacedim> cache(space_dh.get_triangulation(),
                                           space_mapping);
    create_coupling_mass_matrix(cache,
                                space_dh,
                                immersed_dh,
                                quad,
                                matrix,
                                constraints,
                                space_comps,
                                immersed_comps,
                                immersed_mapping);
  }



  template <int dim0, int dim1, int spacedim, typename Matrix>
  void
  create_coupling_mass_matrix(
    const GridTools::Cache<dim0, spacedim> &              cache,
    const DoFHandler<dim0, spacedim> &                    space_dh,
    const DoFHandler<dim1, spacedim> &                    immersed_dh,
    const Quadrature<dim1> &                              quad,
    Matrix &                                              matrix,
    const AffineConstraints<typename Matrix::value_type> &constraints,
    const ComponentMask &                                 space_comps,
    const ComponentMask &                                 immersed_comps,
    const Mapping<dim1, spacedim> &                       immersed_mapping)
  {
    AssertDimension(matrix.m(), space_dh.n_dofs());
    AssertDimension(matrix.n(), immersed_dh.n_dofs());
    static_assert(dim1 <= dim0, "This function can only work if dim1 <= dim0");
    Assert((dynamic_cast<
              const parallel::distributed::Triangulation<dim1, spacedim> *>(
              &immersed_dh.get_triangulation()) == nullptr),
           ExcNotImplemented());

    const auto &space_fe    = space_dh.get_fe();
    const auto &immersed_fe = immersed_dh.get_fe();

    // Dof indices
    std::vector<types::global_dof_index> dofs(immersed_fe.dofs_per_cell);
    std::vector<types::global_dof_index> odofs(space_fe.dofs_per_cell);

    // Take care of components
    const ComponentMask space_c =
      (space_comps.size() == 0 ? ComponentMask(space_fe.n_components(), true) :
                                 space_comps);

    const ComponentMask immersed_c =
      (immersed_comps.size() == 0 ?
         ComponentMask(immersed_fe.n_components(), true) :
         immersed_comps);

    AssertDimension(space_c.size(), space_fe.n_components());
    AssertDimension(immersed_c.size(), immersed_fe.n_components());

    std::vector<unsigned int> space_gtl(space_fe.n_components(),
                                        numbers::invalid_unsigned_int);
    std::vector<unsigned int> immersed_gtl(immersed_fe.n_components(),
                                           numbers::invalid_unsigned_int);

    for (unsigned int i = 0, j = 0; i < space_gtl.size(); ++i)
      if (space_c[i])
        space_gtl[i] = j++;

    for (unsigned int i = 0, j = 0; i < immersed_gtl.size(); ++i)
      if (immersed_c[i])
        immersed_gtl[i] = j++;

    FullMatrix<typename Matrix::value_type> cell_matrix(
      space_dh.get_fe().dofs_per_cell, immersed_dh.get_fe().dofs_per_cell);

    FEValues<dim1, spacedim> fe_v(immersed_mapping,
                                  immersed_dh.get_fe(),
                                  quad,
                                  update_JxW_values | update_quadrature_points |
                                    update_values);

    const unsigned int n_q_points = quad.size();
    const unsigned int n_active_c =
      immersed_dh.get_triangulation().n_active_cells();
    std::vector<Point<spacedim>> all_points(n_active_c * n_q_points);

    // Collecting all points
    {
      unsigned int c = 0;
      for (const auto &cell : immersed_dh.active_cell_iterators())
        {
          // Reinitialize the cell and the fe_values
          fe_v.reinit(cell);
          const std::vector<Point<spacedim>> &x_points =
            fe_v.get_quadrature_points();

          // Copy the points to the vector
          std::copy(x_points.begin(),
                    x_points.end(),
                    all_points.begin() + c * n_q_points);
          ++c;
        }
    }

    // Get a list of outer cells, qpoints and maps.
    const auto  cpm = GridTools::compute_point_locations(cache, all_points);
    const auto &all_cells   = std::get<0>(cpm);
    const auto &all_qpoints = std::get<1>(cpm);
    const auto &all_maps    = std::get<2>(cpm);

    std::vector<
      std::vector<typename Triangulation<dim0, spacedim>::active_cell_iterator>>
                                                       cell_container(n_active_c);
    std::vector<std::vector<std::vector<Point<dim0>>>> qpoints_container(
      n_active_c);
    std::vector<std::vector<std::vector<unsigned int>>> maps_container(
      n_active_c);

    // Cycle over all cells of underling mesh found
    // call it omesh, elaborating the output
    for (unsigned int o = 0; o < all_cells.size(); ++o)
      {
        for (unsigned int j = 0; j < all_maps[o].size(); ++j)
          {
            // Find the index of the "owner" cell and qpoint
            // with regard to the immersed mesh
            const unsigned int cell_id = all_maps[o][j] / n_q_points;
            const unsigned int n_pt    = all_maps[o][j] % n_q_points;

            // If there are no cells, we just add our data
            if (cell_container[cell_id].empty())
              {
                cell_container[cell_id].emplace_back(all_cells[o]);
                qpoints_container[cell_id].emplace_back(
                  std::vector<Point<dim0>>{all_qpoints[o][j]});
                maps_container[cell_id].emplace_back(
                  std::vector<unsigned int>{n_pt});
              }
            // If there are already cells, we begin by looking
            // at the last inserted cell, which is more likely:
            else if (cell_container[cell_id].back() == all_cells[o])
              {
                qpoints_container[cell_id].back().emplace_back(
                  all_qpoints[o][j]);
                maps_container[cell_id].back().emplace_back(n_pt);
              }
            else
              {
                // We don't need to check the last element
                const auto cell_p = std::find(cell_container[cell_id].begin(),
                                              cell_container[cell_id].end() - 1,
                                              all_cells[o]);

                if (cell_p == cell_container[cell_id].end() - 1)
                  {
                    cell_container[cell_id].emplace_back(all_cells[o]);
                    qpoints_container[cell_id].emplace_back(
                      std::vector<Point<dim0>>{all_qpoints[o][j]});
                    maps_container[cell_id].emplace_back(
                      std::vector<unsigned int>{n_pt});
                  }
                else
                  {
                    const unsigned int pos =
                      cell_p - cell_container[cell_id].begin();
                    qpoints_container[cell_id][pos].emplace_back(
                      all_qpoints[o][j]);
                    maps_container[cell_id][pos].emplace_back(n_pt);
                  }
              }
          }
      }

    typename DoFHandler<dim1, spacedim>::active_cell_iterator
      cell = immersed_dh.begin_active(),
      endc = immersed_dh.end();

    for (unsigned int j = 0; cell != endc; ++cell, ++j)
      {
        // Reinitialize the cell and the fe_values
        fe_v.reinit(cell);
        cell->get_dof_indices(dofs);

        // Get a list of outer cells, qpoints and maps.
        const auto &cells   = cell_container[j];
        const auto &qpoints = qpoints_container[j];
        const auto &maps    = maps_container[j];

        for (unsigned int c = 0; c < cells.size(); ++c)
          {
            // Get the ones in the current outer cell
            typename DoFHandler<dim0, spacedim>::active_cell_iterator ocell(
              *cells[c], &space_dh);
            // Make sure we act only on locally_owned cells
            if (ocell->is_locally_owned())
              {
                const std::vector<Point<dim0>> & qps = qpoints[c];
                const std::vector<unsigned int> &ids = maps[c];

                FEValues<dim0, spacedim> o_fe_v(cache.get_mapping(),
                                                space_dh.get_fe(),
                                                qps,
                                                update_values);
                o_fe_v.reinit(ocell);
                ocell->get_dof_indices(odofs);

                // Reset the matrices.
                cell_matrix = typename Matrix::value_type();

                for (unsigned int i = 0; i < space_dh.get_fe().dofs_per_cell;
                     ++i)
                  {
                    const auto comp_i =
                      space_dh.get_fe().system_to_component_index(i).first;
                    if (space_gtl[comp_i] != numbers::invalid_unsigned_int)
                      for (unsigned int j = 0;
                           j < immersed_dh.get_fe().dofs_per_cell;
                           ++j)
                        {
                          const auto comp_j = immersed_dh.get_fe()
                                                .system_to_component_index(j)
                                                .first;
                          if (space_gtl[comp_i] == immersed_gtl[comp_j])
                            for (unsigned int oq = 0;
                                 oq < o_fe_v.n_quadrature_points;
                                 ++oq)
                              {
                                // Get the corresponding q point
                                const unsigned int q = ids[oq];

                                cell_matrix(i, j) +=
                                  (fe_v.shape_value(j, q) *
                                   o_fe_v.shape_value(i, oq) * fe_v.JxW(q));
                              }
                        }
                  }

                // Now assemble the matrices
                constraints.distribute_local_to_global(cell_matrix,
                                                       odofs,
                                                       dofs,
                                                       matrix);
              }
          }
      }
  }

#include "coupling.inst"
} // namespace NonMatching


DEAL_II_NAMESPACE_CLOSE
