// ------------------------------------------------------------------------
//
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2015 - 2025 by the deal.II authors
//
// This file is part of the deal.II library.
//
// Part of the source code is dual licensed under Apache-2.0 WITH
// LLVM-exception OR LGPL-2.1-or-later. Detailed license information
// governing the source code and code contributions can be found in
// LICENSE.md and CONTRIBUTING.md at the top level directory of deal.II.
//
// ------------------------------------------------------------------------

#ifndef dealii_mapping_fe_field_templates_h
#define dealii_mapping_fe_field_templates_h

#include <deal.II/base/array_view.h>
#include <deal.II/base/memory_consumption.h>
#include <deal.II/base/polynomial.h>
#include <deal.II/base/qprojector.h>
#include <deal.II/base/quadrature.h>
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/tensor_product_polynomials.h>
#include <deal.II/base/utilities.h>

#include <deal.II/dofs/dof_accessor.h>

#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/fe_system.h>
#include <deal.II/fe/fe_tools.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/mapping.h>
#include <deal.II/fe/mapping_fe_field.h>
#include <deal.II/fe/mapping_internal.h>

#include <deal.II/grid/tria_iterator.h>

#include <deal.II/lac/block_vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/la_parallel_block_vector.h>
#include <deal.II/lac/la_parallel_vector.h>
#include <deal.II/lac/petsc_block_vector.h>
#include <deal.II/lac/petsc_vector.h>
#include <deal.II/lac/trilinos_epetra_vector.h>
#include <deal.II/lac/trilinos_parallel_block_vector.h>
#include <deal.II/lac/trilinos_tpetra_block_vector.h>
#include <deal.II/lac/trilinos_tpetra_vector.h>
#include <deal.II/lac/trilinos_vector.h>
#include <deal.II/lac/vector.h>

#include <deal.II/numerics/vector_tools.h>

#include <fstream>
#include <memory>
#include <numeric>



DEAL_II_NAMESPACE_OPEN

namespace internal
{
  template <int dim, int spacedim>
  ComponentDoFs<dim, spacedim>::ComponentDoFs(
    const FiniteElement<dim, spacedim> &fe,
    const ComponentMask                &mask)
    : all_components_primitive(true)
  {
    AssertDimension(mask.size(), fe.n_components());
    AssertDimension(mask.n_selected_components(fe.n_components()), spacedim);

    offsets.fill(0u);
    unsigned int d = 0;
    for (unsigned int component_no = 0; component_no < mask.size();
         ++component_no)
      if (mask[component_no])
        {
          const auto base_no = fe.component_to_base_index(component_no).first;
          all_components_primitive &= fe.base_element(base_no).is_primitive();
          unsigned int n_component_dofs = 0;
          for (unsigned int dof = 0; dof < fe.n_dofs_per_cell(); ++dof)
            if (fe.get_nonzero_components(dof)[component_no])
              {
                component_dofs.push_back(dof);
                ++n_component_dofs;
              }
          ++d;
          offsets[d] = offsets[d - 1] + n_component_dofs;
        }
    AssertDimension(d, spacedim);
  }



  template <int dim, int spacedim>
  bool
  ComponentDoFs<dim, spacedim>::all_components_are_primitive() const
  {
    return all_components_primitive;
  }



  template <int dim, int spacedim>
  ArrayView<const unsigned int>
  ComponentDoFs<dim, spacedim>::operator[](const unsigned int component) const
  {
    AssertIndexRange(component, spacedim);
    Assert(component_dofs.cbegin() + offsets[component + 1] <=
             component_dofs.end(),
           ExcInternalError());
    return make_array_view(component_dofs.cbegin() + offsets[component],
                           component_dofs.cbegin() + offsets[component + 1]);
  }
} // namespace internal


template <int dim, int spacedim, typename VectorType>
MappingFEField<dim, spacedim, VectorType>::InternalData::InternalData(
  const FiniteElement<dim, spacedim> &fe,
  const ComponentMask                &mask)
  : fe(&fe)
  , unit_tangentials()
  , n_shape_functions(fe.n_dofs_per_cell())
  , mask(mask)
  , local_dof_indices(fe.n_dofs_per_cell())
  , local_dof_values(fe.n_dofs_per_cell())
{}



template <int dim, int spacedim, typename VectorType>
void
MappingFEField<dim, spacedim, VectorType>::InternalData::reinit(
  const UpdateFlags      update_flags,
  const Quadrature<dim> &quadrature)
{
  // store the flags in the internal data object so we can access them
  // in fill_fe_*_values(). use the transitive hull of the required
  // flags
  this->update_each = update_flags;

  const unsigned int             n_q_points = quadrature.size();
  const std::vector<Point<dim>> &points     = quadrature.get_points();

  // see if we need the (transformation) shape function values
  // and/or gradients and resize the necessary arrays
  if (update_flags & update_quadrature_points)
    {
      shape_values.resize(n_shape_functions * n_q_points);
      for (unsigned int point = 0; point < n_q_points; ++point)
        for (unsigned int i = 0; i < n_shape_functions; ++i)
          shape(point, i) = fe->shape_value(i, points[point]);
    }

  if (update_flags &
      (update_covariant_transformation | update_contravariant_transformation |
       update_JxW_values | update_boundary_forms | update_normal_vectors |
       update_jacobians | update_jacobian_grads | update_inverse_jacobians))
    {
      shape_derivatives.resize(n_shape_functions * n_q_points);
      for (unsigned int point = 0; point < n_q_points; ++point)
        for (unsigned int i = 0; i < n_shape_functions; ++i)
          derivative(point, i) = fe->shape_grad(i, points[point]);
    }

  if (update_flags & update_covariant_transformation)
    covariant.resize(n_q_points);

  if (update_flags & update_contravariant_transformation)
    contravariant.resize(n_q_points);

  if (update_flags & update_volume_elements)
    volume_elements.resize(n_q_points);

  if (update_flags &
      (update_jacobian_grads | update_jacobian_pushed_forward_grads))
    {
      shape_second_derivatives.resize(n_shape_functions * n_q_points);
      for (unsigned int point = 0; point < n_q_points; ++point)
        for (unsigned int i = 0; i < n_shape_functions; ++i)
          second_derivative(point, i) = fe->shape_grad_grad(i, points[point]);
    }

  if (update_flags & (update_jacobian_2nd_derivatives |
                      update_jacobian_pushed_forward_2nd_derivatives))
    {
      shape_third_derivatives.resize(n_shape_functions * n_q_points);
      for (unsigned int point = 0; point < n_q_points; ++point)
        for (unsigned int i = 0; i < n_shape_functions; ++i)
          third_derivative(point, i) =
            fe->shape_3rd_derivative(i, points[point]);
    }

  if (update_flags & (update_jacobian_3rd_derivatives |
                      update_jacobian_pushed_forward_3rd_derivatives))
    {
      shape_fourth_derivatives.resize(n_shape_functions * n_q_points);
      for (unsigned int point = 0; point < n_q_points; ++point)
        for (unsigned int i = 0; i < n_shape_functions; ++i)
          fourth_derivative(point, i) =
            fe->shape_4th_derivative(i, points[point]);
    }

  // This (for face values and simplices) can be different for different
  // calls, so always copy
  quadrature_weights = quadrature.get_weights();
}



template <int dim, int spacedim, typename VectorType>
std::size_t
MappingFEField<dim, spacedim, VectorType>::InternalData::memory_consumption()
  const
{
  DEAL_II_NOT_IMPLEMENTED();
  return 0;
}



template <int dim, int spacedim, typename VectorType>
double &
MappingFEField<dim, spacedim, VectorType>::InternalData::shape(
  const unsigned int qpoint,
  const unsigned int shape_nr)
{
  AssertIndexRange(qpoint * n_shape_functions + shape_nr, shape_values.size());
  return shape_values[qpoint * n_shape_functions + shape_nr];
}


template <int dim, int spacedim, typename VectorType>
const Tensor<1, dim> &
MappingFEField<dim, spacedim, VectorType>::InternalData::derivative(
  const unsigned int qpoint,
  const unsigned int shape_nr) const
{
  AssertIndexRange(qpoint * n_shape_functions + shape_nr,
                   shape_derivatives.size());
  return shape_derivatives[qpoint * n_shape_functions + shape_nr];
}



template <int dim, int spacedim, typename VectorType>
Tensor<1, dim> &
MappingFEField<dim, spacedim, VectorType>::InternalData::derivative(
  const unsigned int qpoint,
  const unsigned int shape_nr)
{
  AssertIndexRange(qpoint * n_shape_functions + shape_nr,
                   shape_derivatives.size());
  return shape_derivatives[qpoint * n_shape_functions + shape_nr];
}


template <int dim, int spacedim, typename VectorType>
const Tensor<2, dim> &
MappingFEField<dim, spacedim, VectorType>::InternalData::second_derivative(
  const unsigned int qpoint,
  const unsigned int shape_nr) const
{
  AssertIndexRange(qpoint * n_shape_functions + shape_nr,
                   shape_second_derivatives.size());
  return shape_second_derivatives[qpoint * n_shape_functions + shape_nr];
}



template <int dim, int spacedim, typename VectorType>
Tensor<2, dim> &
MappingFEField<dim, spacedim, VectorType>::InternalData::second_derivative(
  const unsigned int qpoint,
  const unsigned int shape_nr)
{
  AssertIndexRange(qpoint * n_shape_functions + shape_nr,
                   shape_second_derivatives.size());
  return shape_second_derivatives[qpoint * n_shape_functions + shape_nr];
}


template <int dim, int spacedim, typename VectorType>
const Tensor<3, dim> &
MappingFEField<dim, spacedim, VectorType>::InternalData::third_derivative(
  const unsigned int qpoint,
  const unsigned int shape_nr) const
{
  AssertIndexRange(qpoint * n_shape_functions + shape_nr,
                   shape_third_derivatives.size());
  return shape_third_derivatives[qpoint * n_shape_functions + shape_nr];
}



template <int dim, int spacedim, typename VectorType>
Tensor<3, dim> &
MappingFEField<dim, spacedim, VectorType>::InternalData::third_derivative(
  const unsigned int qpoint,
  const unsigned int shape_nr)
{
  AssertIndexRange(qpoint * n_shape_functions + shape_nr,
                   shape_third_derivatives.size());
  return shape_third_derivatives[qpoint * n_shape_functions + shape_nr];
}


template <int dim, int spacedim, typename VectorType>
const Tensor<4, dim> &
MappingFEField<dim, spacedim, VectorType>::InternalData::fourth_derivative(
  const unsigned int qpoint,
  const unsigned int shape_nr) const
{
  AssertIndexRange(qpoint * n_shape_functions + shape_nr,
                   shape_fourth_derivatives.size());
  return shape_fourth_derivatives[qpoint * n_shape_functions + shape_nr];
}



template <int dim, int spacedim, typename VectorType>
Tensor<4, dim> &
MappingFEField<dim, spacedim, VectorType>::InternalData::fourth_derivative(
  const unsigned int qpoint,
  const unsigned int shape_nr)
{
  AssertIndexRange(qpoint * n_shape_functions + shape_nr,
                   shape_fourth_derivatives.size());
  return shape_fourth_derivatives[qpoint * n_shape_functions + shape_nr];
}



template <int dim, int spacedim, typename VectorType>
MappingFEField<dim, spacedim, VectorType>::MappingFEField(
  const DoFHandler<dim, spacedim> &euler_dof_handler,
  const VectorType                &euler_vector,
  const ComponentMask             &mask)
  : reference_cell(euler_dof_handler.get_fe().reference_cell())
  , uses_level_dofs(false)
  , euler_vector({&euler_vector})
  , euler_dof_handler(&euler_dof_handler)
  , fe_mask(mask.size() != 0u ?
              mask :
              ComponentMask(
                euler_dof_handler.get_fe().get_nonzero_components(0).size(),
                true))
  , component_dofs(euler_dof_handler.get_fe(), fe_mask)
  , fe_values(this->euler_dof_handler->get_fe(),
              reference_cell.template get_nodal_type_quadrature<dim>(),
              update_values)
{
  AssertDimension(euler_dof_handler.n_dofs(), euler_vector.size());
}



template <int dim, int spacedim, typename VectorType>
MappingFEField<dim, spacedim, VectorType>::MappingFEField(
  const DoFHandler<dim, spacedim> &euler_dof_handler,
  const std::vector<VectorType>   &euler_vector,
  const ComponentMask             &mask)
  : reference_cell(euler_dof_handler.get_fe().reference_cell())
  , uses_level_dofs(true)
  , euler_dof_handler(&euler_dof_handler)
  , fe_mask(mask.size() != 0u ?
              mask :
              ComponentMask(
                euler_dof_handler.get_fe().get_nonzero_components(0).size(),
                true))
  , component_dofs(euler_dof_handler.get_fe(), fe_mask)
  , fe_values(this->euler_dof_handler->get_fe(),
              reference_cell.template get_nodal_type_quadrature<dim>(),
              update_values)
{
  Assert(euler_dof_handler.has_level_dofs(),
         ExcMessage("The underlying DoFHandler object did not call "
                    "distribute_mg_dofs(). In this case, the construction via "
                    "level vectors does not make sense."));
  AssertDimension(euler_vector.size(),
                  euler_dof_handler.get_triangulation().n_global_levels());
  this->euler_vector.clear();
  this->euler_vector.resize(euler_vector.size());
  for (unsigned int i = 0; i < euler_vector.size(); ++i)
    {
      AssertDimension(euler_dof_handler.n_dofs(i), euler_vector[i].size());
      this->euler_vector[i] = &euler_vector[i];
    }
}



template <int dim, int spacedim, typename VectorType>
MappingFEField<dim, spacedim, VectorType>::MappingFEField(
  const DoFHandler<dim, spacedim> &euler_dof_handler,
  const MGLevelObject<VectorType> &euler_vector,
  const ComponentMask             &mask)
  : reference_cell(euler_dof_handler.get_fe().reference_cell())
  , uses_level_dofs(true)
  , euler_dof_handler(&euler_dof_handler)
  , fe_mask(mask.size() != 0u ?
              mask :
              ComponentMask(
                euler_dof_handler.get_fe().get_nonzero_components(0).size(),
                true))
  , component_dofs(euler_dof_handler.get_fe(), fe_mask)
  , fe_values(this->euler_dof_handler->get_fe(),
              reference_cell.template get_nodal_type_quadrature<dim>(),
              update_values)
{
  Assert(euler_dof_handler.has_level_dofs(),
         ExcMessage("The underlying DoFHandler object did not call "
                    "distribute_mg_dofs(). In this case, the construction via "
                    "level vectors does not make sense."));
  AssertDimension(euler_vector.max_level() + 1,
                  euler_dof_handler.get_triangulation().n_global_levels());
  this->euler_vector.clear();
  this->euler_vector.resize(
    euler_dof_handler.get_triangulation().n_global_levels());
  for (unsigned int i = euler_vector.min_level(); i <= euler_vector.max_level();
       ++i)
    {
      AssertDimension(euler_dof_handler.n_dofs(i), euler_vector[i].size());
      this->euler_vector[i] = &euler_vector[i];
    }
}



template <int dim, int spacedim, typename VectorType>
MappingFEField<dim, spacedim, VectorType>::MappingFEField(
  const MappingFEField<dim, spacedim, VectorType> &mapping)
  : reference_cell(mapping.reference_cell)
  , uses_level_dofs(mapping.uses_level_dofs)
  , euler_vector(mapping.euler_vector)
  , euler_dof_handler(mapping.euler_dof_handler)
  , fe_mask(mapping.fe_mask)
  , component_dofs(euler_dof_handler->get_fe(), fe_mask)
  , fe_values(mapping.euler_dof_handler->get_fe(),
              reference_cell.template get_nodal_type_quadrature<dim>(),
              update_values)
{}



template <int dim, int spacedim, typename VectorType>
inline const double &
MappingFEField<dim, spacedim, VectorType>::InternalData::shape(
  const unsigned int qpoint,
  const unsigned int shape_nr) const
{
  AssertIndexRange(qpoint * n_shape_functions + shape_nr, shape_values.size());
  return shape_values[qpoint * n_shape_functions + shape_nr];
}



template <int dim, int spacedim, typename VectorType>
bool
MappingFEField<dim, spacedim, VectorType>::preserves_vertex_locations() const
{
  return false;
}



template <int dim, int spacedim, typename VectorType>
bool
MappingFEField<dim, spacedim, VectorType>::is_compatible_with(
  const ReferenceCell &reference_cell) const
{
  Assert(dim == reference_cell.get_dimension(),
         ExcMessage("The dimension of your mapping (" +
                    Utilities::to_string(dim) +
                    ") and the reference cell cell_type (" +
                    Utilities::to_string(reference_cell.get_dimension()) +
                    " ) do not agree."));

  return this->reference_cell == reference_cell;
}



template <int dim, int spacedim, typename VectorType>
boost::container::small_vector<Point<spacedim>,
#ifndef _MSC_VER
                               ReferenceCells::max_n_vertices<dim>()
#else
                               GeometryInfo<dim>::vertices_per_cell
#endif
                               >
MappingFEField<dim, spacedim, VectorType>::get_vertices(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell) const
{
  // we transform our tria iterator into a dof iterator so we can access
  // data not associated with triangulations
  const typename DoFHandler<dim, spacedim>::cell_iterator dof_cell(
    *cell, euler_dof_handler);

  Assert(uses_level_dofs || dof_cell->is_active() == true, ExcInactiveCell());
  AssertDimension(cell->n_vertices(), fe_values.n_quadrature_points);
  if (uses_level_dofs)
    {
      AssertIndexRange(cell->level(), euler_vector.size());
      AssertDimension(euler_vector[cell->level()]->size(),
                      euler_dof_handler->n_dofs(cell->level()));
    }
  else
    AssertDimension(euler_vector[0]->size(), euler_dof_handler->n_dofs());

  {
    std::lock_guard<std::mutex> lock(fe_values_mutex);
    fe_values.reinit(dof_cell);
  }
  const unsigned int dofs_per_cell =
    euler_dof_handler->get_fe().n_dofs_per_cell();
  std::vector<types::global_dof_index> dof_indices(dofs_per_cell);
  if (uses_level_dofs)
    dof_cell->get_mg_dof_indices(dof_indices);
  else
    dof_cell->get_dof_indices(dof_indices);

  const VectorType &vector =
    uses_level_dofs ? *euler_vector[cell->level()] : *euler_vector[0];

  boost::container::small_vector<Point<spacedim>,
#ifndef _MSC_VER
                                 ReferenceCells::max_n_vertices<dim>()
#else
                                 GeometryInfo<dim>::vertices_per_cell
#endif
                                 >
    vertices(cell->n_vertices());

  for (unsigned int d = 0; d < spacedim; ++d)
    for (const auto &i : component_dofs[d])
      {
        const auto value =
          internal::ElementAccess<VectorType>::get(vector, dof_indices[i]);
        if (component_dofs.all_components_are_primitive())
          for (const unsigned int v : cell->vertex_indices())
            vertices[v][d] += fe_values.shape_value(i, v) * value;
        else
          for (const unsigned int v : cell->vertex_indices())
            vertices[v][d] += fe_values.shape_value_component(i, v, d) * value;
      }

  return vertices;
}



template <int dim, int spacedim, typename VectorType>
UpdateFlags
MappingFEField<dim, spacedim, VectorType>::requires_update_flags(
  const UpdateFlags in) const
{
  // add flags if the respective quantities are necessary to compute
  // what we need. note that some flags appear in both conditions and
  // in subsequent set operations. this leads to some circular
  // logic. the only way to treat this is to iterate. since there are
  // 5 if-clauses in the loop, it will take at most 4 iterations to
  // converge. do them:
  UpdateFlags out = in;
  for (unsigned int i = 0; i < 5; ++i)
    {
      // The following is a little incorrect:
      // If not applied on a face,
      // update_boundary_forms does not
      // make sense. On the other hand,
      // it is necessary on a
      // face. Currently,
      // update_boundary_forms is simply
      // ignored for the interior of a
      // cell.
      if (out & (update_JxW_values | update_normal_vectors))
        out |= update_boundary_forms;

      if (out &
          (update_covariant_transformation | update_jacobian_grads |
           update_jacobians | update_boundary_forms | update_normal_vectors))
        out |= update_contravariant_transformation;

      if (out &
          (update_inverse_jacobians | update_jacobian_pushed_forward_grads |
           update_jacobian_pushed_forward_2nd_derivatives |
           update_jacobian_pushed_forward_3rd_derivatives))
        out |= update_covariant_transformation;

      // The contravariant transformation is used in the Piola
      // transformation, which requires the determinant of the Jacobi
      // matrix of the transformation.  Because we have no way of
      // knowing here whether the finite element wants to use the
      // contravariant or the Piola transforms, we add the volume elements
      // to the list of flags to be updated for each cell.
      if (out & update_contravariant_transformation)
        out |= update_volume_elements;

      if (out & update_normal_vectors)
        out |= update_volume_elements;
    }

  return out;
}


template <int dim, int spacedim, typename VectorType>
void
MappingFEField<dim, spacedim, VectorType>::compute_face_data(
  const unsigned int n_original_q_points,
  InternalData      &data) const
{
  // Set to the size of a single quadrature object for faces, as the size set
  // in in reinit() is for all points
  if (data.update_each & update_covariant_transformation)
    data.covariant.resize(n_original_q_points);

  if (data.update_each & update_contravariant_transformation)
    data.contravariant.resize(n_original_q_points);

  if (data.update_each & update_volume_elements)
    data.volume_elements.resize(n_original_q_points);

  if (dim > 1)
    {
      if (data.update_each & update_boundary_forms)
        {
          data.aux.resize(
            dim - 1, std::vector<Tensor<1, spacedim>>(n_original_q_points));


          // TODO: only a single reference cell type possible...
          const auto n_faces = reference_cell.n_faces();

          // Compute tangentials to the unit cell.
          for (unsigned int i = 0; i < n_faces; ++i)
            {
              data.unit_tangentials[i].resize(n_original_q_points);
              std::fill(data.unit_tangentials[i].begin(),
                        data.unit_tangentials[i].end(),
                        reference_cell.template face_tangent_vector<dim>(i, 0));
              if (dim > 2)
                {
                  data.unit_tangentials[n_faces + i].resize(
                    n_original_q_points);
                  std::fill(
                    data.unit_tangentials[n_faces + i].begin(),
                    data.unit_tangentials[n_faces + i].end(),
                    reference_cell.template face_tangent_vector<dim>(i, 1));
                }
            }
        }
    }
}



template <int dim, int spacedim, typename VectorType>
typename std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase>
MappingFEField<dim, spacedim, VectorType>::get_data(
  const UpdateFlags      update_flags,
  const Quadrature<dim> &quadrature) const
{
  std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase> data_ptr =
    std::make_unique<InternalData>(euler_dof_handler->get_fe(), fe_mask);
  data_ptr->reinit(requires_update_flags(update_flags), quadrature);

  return data_ptr;
}



template <int dim, int spacedim, typename VectorType>
std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase>
MappingFEField<dim, spacedim, VectorType>::get_face_data(
  const UpdateFlags               update_flags,
  const hp::QCollection<dim - 1> &quadrature) const
{
  AssertDimension(quadrature.size(), 1);

  std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase> data_ptr =
    std::make_unique<InternalData>(euler_dof_handler->get_fe(), fe_mask);
  auto &data = dynamic_cast<InternalData &>(*data_ptr);

  const Quadrature<dim> q(
    QProjector<dim>::project_to_all_faces(reference_cell, quadrature[0]));
  data.reinit(requires_update_flags(update_flags), q);
  this->compute_face_data(quadrature[0].size(), data);

  return data_ptr;
}


template <int dim, int spacedim, typename VectorType>
std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase>
MappingFEField<dim, spacedim, VectorType>::get_subface_data(
  const UpdateFlags          update_flags,
  const Quadrature<dim - 1> &quadrature) const
{
  std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase> data_ptr =
    std::make_unique<InternalData>(euler_dof_handler->get_fe(), fe_mask);
  auto &data = dynamic_cast<InternalData &>(*data_ptr);

  const Quadrature<dim> q(
    QProjector<dim>::project_to_all_subfaces(reference_cell, quadrature));
  data.reinit(requires_update_flags(update_flags), q);
  this->compute_face_data(quadrature.size(), data);

  return data_ptr;
}



namespace internal
{
  namespace MappingFEFieldImplementation
  {
    /**
     * Compute the locations of quadrature points on the object described by
     * the first argument (and the cell for which the mapping support points
     * have already been set), but only if the update_flags of the @p data
     * argument indicate so.
     */
    template <int dim, int spacedim, typename VectorType>
    void
    maybe_compute_q_points(
      const typename QProjector<dim>::DataSetDescriptor data_set,
      const typename MappingFEField<dim, spacedim, VectorType>::InternalData
                                                   &data,
      const internal::ComponentDoFs<dim, spacedim> &component_dofs,
      std::vector<Point<spacedim>>                 &quadrature_points)
    {
      const UpdateFlags update_flags = data.update_each;

      if (update_flags & update_quadrature_points)
        {
          for (unsigned int point = 0; point < quadrature_points.size();
               ++point)
            {
              Point<spacedim> result;
              const double   *shape = &data.shape(point + data_set, 0);

              for (unsigned int d = 0; d < spacedim; ++d)
                for (const auto &i : component_dofs[d])
                  result[d] += data.local_dof_values[i] * shape[i];

              quadrature_points[point] = result;
            }
        }
    }

    /**
     * Update the co- and contravariant matrices as well as their determinant,
     * for the cell described stored in the data object, but only if the
     * update_flags of the @p data argument indicate so.
     *
     * Skip the computation if possible as indicated by the first argument.
     */
    template <int dim, int spacedim, typename VectorType>
    void
    maybe_update_Jacobians(
      const typename QProjector<dim>::DataSetDescriptor data_set,
      const typename MappingFEField<dim, spacedim, VectorType>::InternalData
                                                   &data,
      const internal::ComponentDoFs<dim, spacedim> &component_dofs)
    {
      const UpdateFlags update_flags = data.update_each;

      // then Jacobians
      if (update_flags & update_contravariant_transformation)
        {
          const unsigned int n_q_points = data.contravariant.size();

          Assert(data.n_shape_functions > 0, ExcInternalError());

          for (unsigned int point = 0; point < n_q_points; ++point)
            {
              const Tensor<1, dim> *data_derv =
                &data.derivative(point + data_set, 0);

              Tensor<1, dim> result[spacedim];

              for (unsigned int d = 0; d < spacedim; ++d)
                for (const auto &i : component_dofs[d])
                  result[d] += data.local_dof_values[i] * data_derv[i];

              // write result into contravariant data
              for (unsigned int i = 0; i < spacedim; ++i)
                data.contravariant[point][i] = result[i];
            }
        }

      if (update_flags & update_covariant_transformation)
        {
          AssertDimension(data.covariant.size(), data.contravariant.size());
          for (unsigned int point = 0; point < data.contravariant.size();
               ++point)
            data.covariant[point] =
              (data.contravariant[point]).covariant_form();
        }

      if (update_flags & update_volume_elements)
        {
          AssertDimension(data.contravariant.size(),
                          data.volume_elements.size());
          for (unsigned int point = 0; point < data.contravariant.size();
               ++point)
            data.volume_elements[point] =
              data.contravariant[point].determinant();
        }
    }

    /**
     * Update the Hessian of the transformation from unit to real cell, the
     * Jacobian gradients.
     *
     * Skip the computation if possible as indicated by the first argument.
     */
    template <int dim, int spacedim, typename VectorType>
    void
    maybe_update_jacobian_grads(
      const typename QProjector<dim>::DataSetDescriptor data_set,
      const typename MappingFEField<dim, spacedim, VectorType>::InternalData
                                                    &data,
      const internal::ComponentDoFs<dim, spacedim>  &component_dofs,
      std::vector<DerivativeForm<2, dim, spacedim>> &jacobian_grads)
    {
      const UpdateFlags update_flags = data.update_each;
      if (update_flags & update_jacobian_grads)
        {
          const unsigned int n_q_points = jacobian_grads.size();

          for (unsigned int point = 0; point < n_q_points; ++point)
            {
              const Tensor<2, dim> *second =
                &data.second_derivative(point + data_set, 0);

              DerivativeForm<2, dim, spacedim> result;

              for (unsigned int d = 0; d < spacedim; ++d)
                for (const auto &i : component_dofs[d])
                  for (unsigned int j = 0; j < dim; ++j)
                    for (unsigned int l = 0; l < dim; ++l)
                      result[d][j][l] +=
                        (second[i][j][l] * data.local_dof_values[i]);

              // never touch any data for j=dim in case dim<spacedim, so
              // it will always be zero as it was initialized
              for (unsigned int i = 0; i < spacedim; ++i)
                for (unsigned int j = 0; j < dim; ++j)
                  for (unsigned int l = 0; l < dim; ++l)
                    jacobian_grads[point][i][j][l] = result[i][j][l];
            }
        }
    }

    /**
     * Update the Hessian of the transformation from unit to real cell, the
     * Jacobian gradients, pushed forward to the real cell coordinates.
     *
     * Skip the computation if possible as indicated by the first argument.
     */
    template <int dim, int spacedim, typename VectorType>
    void
    maybe_update_jacobian_pushed_forward_grads(
      const typename QProjector<dim>::DataSetDescriptor data_set,
      const typename MappingFEField<dim, spacedim, VectorType>::InternalData
                                                   &data,
      const internal::ComponentDoFs<dim, spacedim> &component_dofs,
      std::vector<Tensor<3, spacedim>> &jacobian_pushed_forward_grads)
    {
      const UpdateFlags update_flags = data.update_each;
      if (update_flags & update_jacobian_pushed_forward_grads)
        {
          const unsigned int n_q_points = jacobian_pushed_forward_grads.size();

          double tmp[spacedim][spacedim][spacedim];
          for (unsigned int point = 0; point < n_q_points; ++point)
            {
              const Tensor<2, dim> *second =
                &data.second_derivative(point + data_set, 0);

              DerivativeForm<2, dim, spacedim> result;

              for (unsigned int d = 0; d < spacedim; ++d)
                for (const auto &i : component_dofs[d])
                  for (unsigned int j = 0; j < dim; ++j)
                    for (unsigned int l = 0; l < dim; ++l)
                      result[d][j][l] +=
                        (second[i][j][l] * data.local_dof_values[i]);

              // first push forward the j-components
              for (unsigned int i = 0; i < spacedim; ++i)
                for (unsigned int j = 0; j < spacedim; ++j)
                  for (unsigned int l = 0; l < dim; ++l)
                    {
                      tmp[i][j][l] =
                        result[i][0][l] * data.covariant[point][j][0];
                      for (unsigned int jr = 1; jr < dim; ++jr)
                        {
                          tmp[i][j][l] +=
                            result[i][jr][l] * data.covariant[point][j][jr];
                        }
                    }

              // now, pushing forward the l-components
              for (unsigned int i = 0; i < spacedim; ++i)
                for (unsigned int j = 0; j < spacedim; ++j)
                  for (unsigned int l = 0; l < spacedim; ++l)
                    {
                      jacobian_pushed_forward_grads[point][i][j][l] =
                        tmp[i][j][0] * data.covariant[point][l][0];
                      for (unsigned int lr = 1; lr < dim; ++lr)
                        {
                          jacobian_pushed_forward_grads[point][i][j][l] +=
                            tmp[i][j][lr] * data.covariant[point][l][lr];
                        }
                    }
            }
        }
    }

    /**
     * Update the third derivative of the transformation from unit to real
     * cell, the Jacobian hessians.
     *
     * Skip the computation if possible as indicated by the first argument.
     */
    template <int dim, int spacedim, typename VectorType>
    void
    maybe_update_jacobian_2nd_derivatives(
      const typename QProjector<dim>::DataSetDescriptor data_set,
      const typename MappingFEField<dim, spacedim, VectorType>::InternalData
                                                    &data,
      const internal::ComponentDoFs<dim, spacedim>  &component_dofs,
      std::vector<DerivativeForm<3, dim, spacedim>> &jacobian_2nd_derivatives)
    {
      const UpdateFlags update_flags = data.update_each;
      if (update_flags & update_jacobian_2nd_derivatives)
        {
          const unsigned int n_q_points = jacobian_2nd_derivatives.size();

          for (unsigned int point = 0; point < n_q_points; ++point)
            {
              const Tensor<3, dim> *third =
                &data.third_derivative(point + data_set, 0);

              DerivativeForm<3, dim, spacedim> result;

              for (unsigned int d = 0; d < spacedim; ++d)
                for (const auto &i : component_dofs[d])
                  for (unsigned int j = 0; j < dim; ++j)
                    for (unsigned int l = 0; l < dim; ++l)
                      for (unsigned int m = 0; m < dim; ++m)
                        result[d][j][l][m] +=
                          (third[i][j][l][m] * data.local_dof_values[i]);

              // never touch any data for j=dim in case dim<spacedim, so
              // it will always be zero as it was initialized
              for (unsigned int i = 0; i < spacedim; ++i)
                for (unsigned int j = 0; j < dim; ++j)
                  for (unsigned int l = 0; l < dim; ++l)
                    for (unsigned int m = 0; m < dim; ++m)
                      jacobian_2nd_derivatives[point][i][j][l][m] =
                        result[i][j][l][m];
            }
        }
    }

    /**
     * Update the third derivative of the transformation from unit to real
     * cell, the Jacobian hessians, pushed forward to the real cell
     * coordinates.
     *
     * Skip the computation if possible as indicated by the first argument.
     */
    template <int dim, int spacedim, typename VectorType>
    void
    maybe_update_jacobian_pushed_forward_2nd_derivatives(
      const typename QProjector<dim>::DataSetDescriptor data_set,
      const typename MappingFEField<dim, spacedim, VectorType>::InternalData
                                                   &data,
      const internal::ComponentDoFs<dim, spacedim> &component_dofs,
      std::vector<Tensor<4, spacedim>> &jacobian_pushed_forward_2nd_derivatives)
    {
      const UpdateFlags update_flags = data.update_each;
      if (update_flags & update_jacobian_pushed_forward_2nd_derivatives)
        {
          const unsigned int n_q_points =
            jacobian_pushed_forward_2nd_derivatives.size();

          double tmp[spacedim][spacedim][spacedim][spacedim];
          for (unsigned int point = 0; point < n_q_points; ++point)
            {
              const Tensor<3, dim> *third =
                &data.third_derivative(point + data_set, 0);

              DerivativeForm<3, dim, spacedim> result;

              for (unsigned int d = 0; d < spacedim; ++d)
                for (const auto &i : component_dofs[d])
                  for (unsigned int j = 0; j < dim; ++j)
                    for (unsigned int l = 0; l < dim; ++l)
                      for (unsigned int m = 0; m < dim; ++m)
                        result[d][j][l][m] +=
                          (third[i][j][l][m] * data.local_dof_values[i]);

              // push forward the j-coordinate
              for (unsigned int i = 0; i < spacedim; ++i)
                for (unsigned int j = 0; j < spacedim; ++j)
                  for (unsigned int l = 0; l < dim; ++l)
                    for (unsigned int m = 0; m < dim; ++m)
                      {
                        jacobian_pushed_forward_2nd_derivatives
                          [point][i][j][l][m] =
                            result[i][0][l][m] * data.covariant[point][j][0];
                        for (unsigned int jr = 1; jr < dim; ++jr)
                          jacobian_pushed_forward_2nd_derivatives[point][i][j]
                                                                 [l][m] +=
                            result[i][jr][l][m] * data.covariant[point][j][jr];
                      }

              // push forward the l-coordinate
              for (unsigned int i = 0; i < spacedim; ++i)
                for (unsigned int j = 0; j < spacedim; ++j)
                  for (unsigned int l = 0; l < spacedim; ++l)
                    for (unsigned int m = 0; m < dim; ++m)
                      {
                        tmp[i][j][l][m] =
                          jacobian_pushed_forward_2nd_derivatives[point][i][j]
                                                                 [0][m] *
                          data.covariant[point][l][0];
                        for (unsigned int lr = 1; lr < dim; ++lr)
                          tmp[i][j][l][m] +=
                            jacobian_pushed_forward_2nd_derivatives[point][i][j]
                                                                   [lr][m] *
                            data.covariant[point][l][lr];
                      }

              // push forward the m-coordinate
              for (unsigned int i = 0; i < spacedim; ++i)
                for (unsigned int j = 0; j < spacedim; ++j)
                  for (unsigned int l = 0; l < spacedim; ++l)
                    for (unsigned int m = 0; m < spacedim; ++m)
                      {
                        jacobian_pushed_forward_2nd_derivatives
                          [point][i][j][l][m] =
                            tmp[i][j][l][0] * data.covariant[point][m][0];
                        for (unsigned int mr = 1; mr < dim; ++mr)
                          jacobian_pushed_forward_2nd_derivatives[point][i][j]
                                                                 [l][m] +=
                            tmp[i][j][l][mr] * data.covariant[point][m][mr];
                      }
            }
        }
    }

    /**
     * Update the fourth derivative of the transformation from unit to real
     * cell, the Jacobian hessian gradients.
     *
     * Skip the computation if possible as indicated by the first argument.
     */
    template <int dim, int spacedim, typename VectorType>
    void
    maybe_update_jacobian_3rd_derivatives(
      const typename QProjector<dim>::DataSetDescriptor data_set,
      const typename MappingFEField<dim, spacedim, VectorType>::InternalData
                                                    &data,
      const internal::ComponentDoFs<dim, spacedim>  &component_dofs,
      std::vector<DerivativeForm<4, dim, spacedim>> &jacobian_3rd_derivatives)
    {
      const UpdateFlags update_flags = data.update_each;
      if (update_flags & update_jacobian_3rd_derivatives)
        {
          const unsigned int n_q_points = jacobian_3rd_derivatives.size();

          for (unsigned int point = 0; point < n_q_points; ++point)
            {
              const Tensor<4, dim> *fourth =
                &data.fourth_derivative(point + data_set, 0);

              DerivativeForm<4, dim, spacedim> result;

              for (unsigned int d = 0; d < spacedim; ++d)
                for (const auto &i : component_dofs[d])
                  for (unsigned int j = 0; j < dim; ++j)
                    for (unsigned int l = 0; l < dim; ++l)
                      for (unsigned int m = 0; m < dim; ++m)
                        for (unsigned int n = 0; n < dim; ++n)
                          result[d][j][l][m][n] +=
                            (fourth[i][j][l][m][n] * data.local_dof_values[i]);

              // never touch any data for j,l,m,n=dim in case
              // dim<spacedim, so it will always be zero as it was
              // initialized
              for (unsigned int i = 0; i < spacedim; ++i)
                for (unsigned int j = 0; j < dim; ++j)
                  for (unsigned int l = 0; l < dim; ++l)
                    for (unsigned int m = 0; m < dim; ++m)
                      for (unsigned int n = 0; n < dim; ++n)
                        jacobian_3rd_derivatives[point][i][j][l][m][n] =
                          result[i][j][l][m][n];
            }
        }
    }

    /**
     * Update the fourth derivative of the transformation from unit to real
     * cell, the Jacobian hessian gradients, pushed forward to the real cell
     * coordinates.
     *
     * Skip the computation if possible as indicated by the first argument.
     */
    template <int dim, int spacedim, typename VectorType>
    void
    maybe_update_jacobian_pushed_forward_3rd_derivatives(
      const typename QProjector<dim>::DataSetDescriptor data_set,
      const typename MappingFEField<dim, spacedim, VectorType>::InternalData
                                                   &data,
      const internal::ComponentDoFs<dim, spacedim> &component_dofs,
      std::vector<Tensor<5, spacedim>> &jacobian_pushed_forward_3rd_derivatives)
    {
      const UpdateFlags update_flags = data.update_each;
      if (update_flags & update_jacobian_pushed_forward_3rd_derivatives)
        {
          const unsigned int n_q_points =
            jacobian_pushed_forward_3rd_derivatives.size();

          double tmp[spacedim][spacedim][spacedim][spacedim][spacedim];
          for (unsigned int point = 0; point < n_q_points; ++point)
            {
              const Tensor<4, dim> *fourth =
                &data.fourth_derivative(point + data_set, 0);

              DerivativeForm<4, dim, spacedim> result;

              for (unsigned int d = 0; d < spacedim; ++d)
                for (const auto &i : component_dofs[d])
                  for (unsigned int j = 0; j < dim; ++j)
                    for (unsigned int l = 0; l < dim; ++l)
                      for (unsigned int m = 0; m < dim; ++m)
                        for (unsigned int n = 0; n < dim; ++n)
                          result[d][j][l][m][n] +=
                            (fourth[i][j][l][m][n] * data.local_dof_values[i]);

              // push-forward the j-coordinate
              for (unsigned int i = 0; i < spacedim; ++i)
                for (unsigned int j = 0; j < spacedim; ++j)
                  for (unsigned int l = 0; l < dim; ++l)
                    for (unsigned int m = 0; m < dim; ++m)
                      for (unsigned int n = 0; n < dim; ++n)
                        {
                          tmp[i][j][l][m][n] =
                            result[i][0][l][m][n] * data.covariant[point][j][0];
                          for (unsigned int jr = 1; jr < dim; ++jr)
                            tmp[i][j][l][m][n] += result[i][jr][l][m][n] *
                                                  data.covariant[point][j][jr];
                        }

              // push-forward the l-coordinate
              for (unsigned int i = 0; i < spacedim; ++i)
                for (unsigned int j = 0; j < spacedim; ++j)
                  for (unsigned int l = 0; l < spacedim; ++l)
                    for (unsigned int m = 0; m < dim; ++m)
                      for (unsigned int n = 0; n < dim; ++n)
                        {
                          jacobian_pushed_forward_3rd_derivatives
                            [point][i][j][l][m][n] =
                              tmp[i][j][0][m][n] * data.covariant[point][l][0];
                          for (unsigned int lr = 1; lr < dim; ++lr)
                            jacobian_pushed_forward_3rd_derivatives[point][i][j]
                                                                   [l][m][n] +=
                              tmp[i][j][lr][m][n] *
                              data.covariant[point][l][lr];
                        }

              // push-forward the m-coordinate
              for (unsigned int i = 0; i < spacedim; ++i)
                for (unsigned int j = 0; j < spacedim; ++j)
                  for (unsigned int l = 0; l < spacedim; ++l)
                    for (unsigned int m = 0; m < spacedim; ++m)
                      for (unsigned int n = 0; n < dim; ++n)
                        {
                          tmp[i][j][l][m][n] =
                            jacobian_pushed_forward_3rd_derivatives[point][i][j]
                                                                   [l][0][n] *
                            data.covariant[point][m][0];
                          for (unsigned int mr = 1; mr < dim; ++mr)
                            tmp[i][j][l][m][n] +=
                              jacobian_pushed_forward_3rd_derivatives[point][i]
                                                                     [j][l][mr]
                                                                     [n] *
                              data.covariant[point][m][mr];
                        }

              // push-forward the n-coordinate
              for (unsigned int i = 0; i < spacedim; ++i)
                for (unsigned int j = 0; j < spacedim; ++j)
                  for (unsigned int l = 0; l < spacedim; ++l)
                    for (unsigned int m = 0; m < spacedim; ++m)
                      for (unsigned int n = 0; n < spacedim; ++n)
                        {
                          jacobian_pushed_forward_3rd_derivatives
                            [point][i][j][l][m][n] =
                              tmp[i][j][l][m][0] * data.covariant[point][n][0];
                          for (unsigned int nr = 1; nr < dim; ++nr)
                            jacobian_pushed_forward_3rd_derivatives[point][i][j]
                                                                   [l][m][n] +=
                              tmp[i][j][l][m][nr] *
                              data.covariant[point][n][nr];
                        }
            }
        }
    }


    /**
     * Depending on what information is called for in the update flags of the
     * @p data object, compute the various pieces of information that is
     * required by the fill_fe_face_values() and fill_fe_subface_values()
     * functions.  This function simply unifies the work that would be done by
     * those two functions.
     *
     * The resulting data is put into the @p output_data argument.
     */
    template <int dim, int spacedim, typename VectorType>
    void
    maybe_compute_face_data(
      const Mapping<dim, spacedim>                               &mapping,
      const typename Triangulation<dim, spacedim>::cell_iterator &cell,
      const unsigned int                                          face_no,
      const unsigned int                                          subface_no,
      const typename QProjector<dim>::DataSetDescriptor           data_set,
      const typename MappingFEField<dim, spacedim, VectorType>::InternalData
        &data,
      internal::FEValuesImplementation::MappingRelatedData<dim, spacedim>
        &output_data)
    {
      const UpdateFlags update_flags = data.update_each;

      if (update_flags & update_boundary_forms)
        {
          const unsigned int n_q_points = output_data.boundary_forms.size();
          if (update_flags & update_normal_vectors)
            AssertDimension(output_data.normal_vectors.size(), n_q_points);
          if (update_flags & update_JxW_values)
            AssertDimension(output_data.JxW_values.size(), n_q_points);

          // map the unit tangentials to the real cell. checking for d!=dim-1
          // eliminates compiler warnings regarding unsigned int expressions <
          // 0.
          for (unsigned int d = 0; d != dim - 1; ++d)
            {
              Assert(face_no + cell->n_faces() * d <
                       data.unit_tangentials.size(),
                     ExcInternalError());
              Assert(
                data.aux[d].size() <=
                  data.unit_tangentials[face_no + cell->n_faces() * d].size(),
                ExcInternalError());

              mapping.transform(
                make_array_view(
                  data.unit_tangentials[face_no + cell->n_faces() * d]),
                mapping_contravariant,
                data,
                make_array_view(data.aux[d]));
            }

          // if dim==spacedim, we can use the unit tangentials to compute the
          // boundary form by simply taking the cross product
          if (dim == spacedim)
            {
              for (unsigned int i = 0; i < n_q_points; ++i)
                switch (dim)
                  {
                    case 1:
                      // in 1d, we don't have access to any of the data.aux
                      // fields (because it has only dim-1 components), but we
                      // can still compute the boundary form by simply looking
                      // at the number of the face
                      output_data.boundary_forms[i][0] =
                        (face_no == 0 ? -1 : +1);
                      break;
                    case 2:
                      output_data.boundary_forms[i] =
                        cross_product_2d(data.aux[0][i]);
                      break;
                    case 3:
                      output_data.boundary_forms[i] =
                        cross_product_3d(data.aux[0][i], data.aux[1][i]);
                      break;
                    default:
                      DEAL_II_NOT_IMPLEMENTED();
                  }
            }
          else //(dim < spacedim)
            {
              // in the codim-one case, the boundary form results from the
              // cross product of all the face tangential vectors and the cell
              // normal vector
              //
              // to compute the cell normal, use the same method used in
              // fill_fe_values for cells above
              AssertDimension(data.contravariant.size(), n_q_points);

              for (unsigned int point = 0; point < n_q_points; ++point)
                {
                  if (dim == 1)
                    {
                      // J is a tangent vector
                      output_data.boundary_forms[point] =
                        data.contravariant[point].transpose()[0];
                      output_data.boundary_forms[point] /=
                        (face_no == 0 ? -1. : +1.) *
                        output_data.boundary_forms[point].norm();
                    }

                  if (dim == 2)
                    {
                      const DerivativeForm<1, spacedim, dim> DX_t =
                        data.contravariant[point].transpose();

                      Tensor<1, spacedim> cell_normal =
                        cross_product_3d(DX_t[0], DX_t[1]);
                      cell_normal /= cell_normal.norm();

                      // then compute the face normal from the face tangent
                      // and the cell normal:
                      output_data.boundary_forms[point] =
                        cross_product_3d(data.aux[0][point], cell_normal);
                    }
                }
            }

          if (update_flags & (update_normal_vectors | update_JxW_values))
            for (unsigned int i = 0; i < output_data.boundary_forms.size(); ++i)
              {
                if (update_flags & update_JxW_values)
                  {
                    output_data.JxW_values[i] =
                      output_data.boundary_forms[i].norm() *
                      data.quadrature_weights[i + data_set];

                    if (subface_no != numbers::invalid_unsigned_int)
                      {
                        // TODO
                        const double area_ratio =
                          GeometryInfo<dim>::subface_ratio(
                            cell->subface_case(face_no), subface_no);
                        output_data.JxW_values[i] *= area_ratio;
                      }
                  }

                if (update_flags & update_normal_vectors)
                  output_data.normal_vectors[i] =
                    Point<spacedim>(output_data.boundary_forms[i] /
                                    output_data.boundary_forms[i].norm());
              }
        }
    }

    /**
     * Do the work of MappingFEField::fill_fe_face_values() and
     * MappingFEField::fill_fe_subface_values() in a generic way, using the
     * 'data_set' to differentiate whether we will work on a face (and if so,
     * which one) or subface.
     */
    template <int dim, int spacedim, typename VectorType>
    void
    do_fill_fe_face_values(
      const Mapping<dim, spacedim>                               &mapping,
      const typename Triangulation<dim, spacedim>::cell_iterator &cell,
      const unsigned int                                          face_no,
      const unsigned int                                          subface_no,
      const typename QProjector<dim>::DataSetDescriptor           data_set,
      const typename MappingFEField<dim, spacedim, VectorType>::InternalData
                                                   &data,
      const internal::ComponentDoFs<dim, spacedim> &component_dofs,
      internal::FEValuesImplementation::MappingRelatedData<dim, spacedim>
        &output_data)
    {
      maybe_compute_q_points<dim, spacedim, VectorType>(
        data_set, data, component_dofs, output_data.quadrature_points);

      maybe_update_Jacobians<dim, spacedim, VectorType>(data_set,
                                                        data,
                                                        component_dofs);

      const UpdateFlags  update_flags = data.update_each;
      const unsigned int n_q_points   = data.contravariant.size();

      if (update_flags & update_jacobians)
        for (unsigned int point = 0; point < n_q_points; ++point)
          output_data.jacobians[point] = data.contravariant[point];

      if (update_flags & update_inverse_jacobians)
        for (unsigned int point = 0; point < n_q_points; ++point)
          output_data.inverse_jacobians[point] =
            data.covariant[point].transpose();

      maybe_update_jacobian_grads<dim, spacedim, VectorType>(
        data_set, data, component_dofs, output_data.jacobian_grads);

      maybe_update_jacobian_pushed_forward_grads<dim, spacedim, VectorType>(
        data_set,
        data,
        component_dofs,
        output_data.jacobian_pushed_forward_grads);

      maybe_update_jacobian_2nd_derivatives<dim, spacedim, VectorType>(
        data_set, data, component_dofs, output_data.jacobian_2nd_derivatives);

      maybe_update_jacobian_pushed_forward_2nd_derivatives<dim,
                                                           spacedim,
                                                           VectorType>(
        data_set,
        data,
        component_dofs,
        output_data.jacobian_pushed_forward_2nd_derivatives);

      maybe_update_jacobian_3rd_derivatives<dim, spacedim, VectorType>(
        data_set, data, component_dofs, output_data.jacobian_3rd_derivatives);

      maybe_update_jacobian_pushed_forward_3rd_derivatives<dim,
                                                           spacedim,
                                                           VectorType>(
        data_set,
        data,
        component_dofs,
        output_data.jacobian_pushed_forward_3rd_derivatives);

      maybe_compute_face_data<dim, spacedim, VectorType>(
        mapping, cell, face_no, subface_no, data_set, data, output_data);
    }
  } // namespace MappingFEFieldImplementation
} // namespace internal


// Note that the CellSimilarity flag is modifiable, since MappingFEField can
// need to recalculate data even when cells are similar.
template <int dim, int spacedim, typename VectorType>
CellSimilarity::Similarity
MappingFEField<dim, spacedim, VectorType>::fill_fe_values(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const CellSimilarity::Similarity,
  const Quadrature<dim>                                   &quadrature,
  const typename Mapping<dim, spacedim>::InternalDataBase &internal_data,
  internal::FEValuesImplementation::MappingRelatedData<dim, spacedim>
    &output_data) const
{
  // convert data object to internal data for this class. fails with an
  // exception if that is not possible
  Assert(dynamic_cast<const InternalData *>(&internal_data) != nullptr,
         ExcInternalError());
  const InternalData &data = static_cast<const InternalData &>(internal_data);

  const unsigned int n_q_points = quadrature.size();

  update_internal_dofs(cell, data);

  internal::MappingFEFieldImplementation::
    maybe_compute_q_points<dim, spacedim, VectorType>(
      QProjector<dim>::DataSetDescriptor::cell(),
      data,
      component_dofs,
      output_data.quadrature_points);

  internal::MappingFEFieldImplementation::
    maybe_update_Jacobians<dim, spacedim, VectorType>(
      QProjector<dim>::DataSetDescriptor::cell(), data, component_dofs);

  const UpdateFlags          update_flags = data.update_each;
  const std::vector<double> &weights      = quadrature.get_weights();

  // Multiply quadrature weights by absolute value of Jacobian determinants or
  // the area element g=sqrt(DX^t DX) in case of codim > 0

  if (update_flags & (update_normal_vectors | update_JxW_values))
    {
      AssertDimension(output_data.JxW_values.size(), n_q_points);

      Assert(!(update_flags & update_normal_vectors) ||
               (output_data.normal_vectors.size() == n_q_points),
             ExcDimensionMismatch(output_data.normal_vectors.size(),
                                  n_q_points));


      for (unsigned int point = 0; point < n_q_points; ++point)
        {
          if (dim == spacedim)
            {
              const double det = data.volume_elements[point];

              // check for distorted cells.

              // TODO: this allows for anisotropies of up to 1e6 in 3d and
              // 1e12 in 2d. might want to find a finer
              // (dimension-independent) criterion
              Assert(det > 1e-12 * Utilities::fixed_power<dim>(
                                     cell->diameter() / std::sqrt(double(dim))),
                     (typename Mapping<dim, spacedim>::ExcDistortedMappedCell(
                       cell->center(), det, point)));
              output_data.JxW_values[point] = weights[point] * det;
            }
          // if dim==spacedim, then there is no cell normal to
          // compute. since this is for FEValues (and not FEFaceValues),
          // there are also no face normals to compute
          else // codim>0 case
            {
              Tensor<1, spacedim> DX_t[dim];
              for (unsigned int i = 0; i < spacedim; ++i)
                for (unsigned int j = 0; j < dim; ++j)
                  DX_t[j][i] = data.contravariant[point][i][j];

              Tensor<2, dim> G; // First fundamental form
              for (unsigned int i = 0; i < dim; ++i)
                for (unsigned int j = 0; j < dim; ++j)
                  G[i][j] = DX_t[i] * DX_t[j];

              output_data.JxW_values[point] =
                std::sqrt(determinant(G)) * weights[point];

              if (update_flags & update_normal_vectors)
                {
                  Assert(spacedim - dim == 1,
                         ExcMessage("There is no cell normal in codim 2."));

                  if (dim == 1)
                    output_data.normal_vectors[point] =
                      cross_product_2d(-DX_t[0]);
                  else
                    {
                      Assert(dim == 2, ExcInternalError());

                      // dim-1==1 for the second argument, but this
                      // avoids a compiler warning about array bounds:
                      output_data.normal_vectors[point] =
                        cross_product_3d(DX_t[0], DX_t[dim - 1]);
                    }

                  output_data.normal_vectors[point] /=
                    output_data.normal_vectors[point].norm();

                  if (cell->direction_flag() == false)
                    output_data.normal_vectors[point] *= -1.;
                }
            } // codim>0 case
        }
    }

  // copy values from InternalData to vector given by reference
  if (update_flags & update_jacobians)
    {
      AssertDimension(output_data.jacobians.size(), n_q_points);
      for (unsigned int point = 0; point < n_q_points; ++point)
        output_data.jacobians[point] = data.contravariant[point];
    }

  // copy values from InternalData to vector given by reference
  if (update_flags & update_inverse_jacobians)
    {
      AssertDimension(output_data.inverse_jacobians.size(), n_q_points);
      for (unsigned int point = 0; point < n_q_points; ++point)
        output_data.inverse_jacobians[point] =
          data.covariant[point].transpose();
    }

  // calculate derivatives of the Jacobians
  internal::MappingFEFieldImplementation::
    maybe_update_jacobian_grads<dim, spacedim, VectorType>(
      QProjector<dim>::DataSetDescriptor::cell(),
      data,
      component_dofs,
      output_data.jacobian_grads);

  // calculate derivatives of the Jacobians pushed forward to real cell
  // coordinates
  internal::MappingFEFieldImplementation::
    maybe_update_jacobian_pushed_forward_grads<dim, spacedim, VectorType>(
      QProjector<dim>::DataSetDescriptor::cell(),
      data,
      component_dofs,
      output_data.jacobian_pushed_forward_grads);

  // calculate hessians of the Jacobians
  internal::MappingFEFieldImplementation::
    maybe_update_jacobian_2nd_derivatives<dim, spacedim, VectorType>(
      QProjector<dim>::DataSetDescriptor::cell(),
      data,
      component_dofs,
      output_data.jacobian_2nd_derivatives);

  // calculate hessians of the Jacobians pushed forward to real cell coordinates
  internal::MappingFEFieldImplementation::
    maybe_update_jacobian_pushed_forward_2nd_derivatives<dim,
                                                         spacedim,
                                                         VectorType>(
      QProjector<dim>::DataSetDescriptor::cell(),
      data,
      component_dofs,
      output_data.jacobian_pushed_forward_2nd_derivatives);

  // calculate gradients of the hessians of the Jacobians
  internal::MappingFEFieldImplementation::
    maybe_update_jacobian_3rd_derivatives<dim, spacedim, VectorType>(
      QProjector<dim>::DataSetDescriptor::cell(),
      data,
      component_dofs,
      output_data.jacobian_3rd_derivatives);

  // calculate gradients of the hessians of the Jacobians pushed forward to real
  // cell coordinates
  internal::MappingFEFieldImplementation::
    maybe_update_jacobian_pushed_forward_3rd_derivatives<dim,
                                                         spacedim,
                                                         VectorType>(
      QProjector<dim>::DataSetDescriptor::cell(),
      data,
      component_dofs,
      output_data.jacobian_pushed_forward_3rd_derivatives);

  return CellSimilarity::invalid_next_cell;
}



template <int dim, int spacedim, typename VectorType>
void
MappingFEField<dim, spacedim, VectorType>::fill_fe_face_values(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const unsigned int                                          face_no,
  const hp::QCollection<dim - 1>                             &quadrature,
  const typename Mapping<dim, spacedim>::InternalDataBase    &internal_data,
  internal::FEValuesImplementation::MappingRelatedData<dim, spacedim>
    &output_data) const
{
  AssertDimension(quadrature.size(), 1);

  // convert data object to internal data for this class. fails with an
  // exception if that is not possible
  Assert(dynamic_cast<const InternalData *>(&internal_data) != nullptr,
         ExcInternalError());
  const InternalData &data = static_cast<const InternalData &>(internal_data);

  update_internal_dofs(cell, data);

  internal::MappingFEFieldImplementation::
    do_fill_fe_face_values<dim, spacedim, VectorType>(
      *this,
      cell,
      face_no,
      numbers::invalid_unsigned_int,
      QProjector<dim>::DataSetDescriptor::face(reference_cell,
                                               face_no,
                                               cell->combined_face_orientation(
                                                 face_no),
                                               quadrature[0].size()),
      data,
      component_dofs,
      output_data);
}


template <int dim, int spacedim, typename VectorType>
void
MappingFEField<dim, spacedim, VectorType>::fill_fe_subface_values(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const unsigned int                                          face_no,
  const unsigned int                                          subface_no,
  const Quadrature<dim - 1>                                  &quadrature,
  const typename Mapping<dim, spacedim>::InternalDataBase    &internal_data,
  internal::FEValuesImplementation::MappingRelatedData<dim, spacedim>
    &output_data) const
{
  // convert data object to internal data for this class. fails with an
  // exception if that is not possible
  Assert(dynamic_cast<const InternalData *>(&internal_data) != nullptr,
         ExcInternalError());
  const InternalData &data = static_cast<const InternalData &>(internal_data);

  update_internal_dofs(cell, data);

  internal::MappingFEFieldImplementation::do_fill_fe_face_values<dim,
                                                                 spacedim,
                                                                 VectorType>(
    *this,
    cell,
    face_no,
    numbers::invalid_unsigned_int,
    QProjector<dim>::DataSetDescriptor::subface(reference_cell,
                                                face_no,
                                                subface_no,
                                                cell->combined_face_orientation(
                                                  face_no),
                                                quadrature.size(),
                                                cell->subface_case(face_no)),
    data,
    component_dofs,
    output_data);
}



template <int dim, int spacedim, typename VectorType>
void
MappingFEField<dim, spacedim, VectorType>::fill_fe_immersed_surface_values(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const NonMatching::ImmersedSurfaceQuadrature<dim>          &quadrature,
  const typename Mapping<dim, spacedim>::InternalDataBase    &internal_data,
  internal::FEValuesImplementation::MappingRelatedData<dim, spacedim>
    &output_data) const
{
  AssertDimension(dim, spacedim);
  Assert(dynamic_cast<const InternalData *>(&internal_data) != nullptr,
         ExcInternalError());
  const InternalData &data = static_cast<const InternalData &>(internal_data);

  const unsigned int n_q_points = quadrature.size();

  update_internal_dofs(cell, data);

  internal::MappingFEFieldImplementation::
    maybe_compute_q_points<dim, spacedim, VectorType>(
      QProjector<dim>::DataSetDescriptor::cell(),
      data,
      component_dofs,
      output_data.quadrature_points);

  internal::MappingFEFieldImplementation::
    maybe_update_Jacobians<dim, spacedim, VectorType>(
      QProjector<dim>::DataSetDescriptor::cell(), data, component_dofs);

  const UpdateFlags          update_flags = data.update_each;
  const std::vector<double> &weights      = quadrature.get_weights();

  if (update_flags & (update_normal_vectors | update_JxW_values))
    {
      AssertDimension(output_data.JxW_values.size(), n_q_points);

      Assert(!(update_flags & update_normal_vectors) ||
               (output_data.normal_vectors.size() == n_q_points),
             ExcDimensionMismatch(output_data.normal_vectors.size(),
                                  n_q_points));


      for (unsigned int point = 0; point < n_q_points; ++point)
        {
          const double det = data.volume_elements[point];

          // check for distorted cells.

          // TODO: this allows for anisotropies of up to 1e6 in 3d and
          // 1e12 in 2d. might want to find a finer
          // (dimension-independent) criterion
          Assert(det > 1e-12 * Utilities::fixed_power<dim>(
                                 cell->diameter() / std::sqrt(double(dim))),
                 (typename Mapping<dim, spacedim>::ExcDistortedMappedCell(
                   cell->center(), det, point)));

          // The normals are n = J^{-T} * \hat{n} before normalizing.
          Tensor<1, spacedim> normal;
          for (unsigned int d = 0; d < spacedim; d++)
            normal[d] =
              data.covariant[point][d] * quadrature.normal_vector(point);

          output_data.JxW_values[point] = weights[point] * det * normal.norm();

          if ((update_flags & update_normal_vectors) != 0u)
            {
              normal /= normal.norm();
              output_data.normal_vectors[point] = normal;
            }
        }

      // copy values from InternalData to vector given by reference
      if (update_flags & update_jacobians)
        {
          AssertDimension(output_data.jacobians.size(), n_q_points);
          for (unsigned int point = 0; point < n_q_points; ++point)
            output_data.jacobians[point] = data.contravariant[point];
        }

      // copy values from InternalData to vector given by reference
      if (update_flags & update_inverse_jacobians)
        {
          AssertDimension(output_data.inverse_jacobians.size(), n_q_points);
          for (unsigned int point = 0; point < n_q_points; ++point)
            output_data.inverse_jacobians[point] =
              data.covariant[point].transpose();
        }

      // calculate derivatives of the Jacobians
      internal::MappingFEFieldImplementation::
        maybe_update_jacobian_grads<dim, spacedim, VectorType>(
          QProjector<dim>::DataSetDescriptor::cell(),
          data,
          component_dofs,
          output_data.jacobian_grads);

      // calculate derivatives of the Jacobians pushed forward to real cell
      // coordinates
      internal::MappingFEFieldImplementation::
        maybe_update_jacobian_pushed_forward_grads<dim, spacedim, VectorType>(
          QProjector<dim>::DataSetDescriptor::cell(),
          data,
          component_dofs,
          output_data.jacobian_pushed_forward_grads);

      // calculate hessians of the Jacobians
      internal::MappingFEFieldImplementation::
        maybe_update_jacobian_2nd_derivatives<dim, spacedim, VectorType>(
          QProjector<dim>::DataSetDescriptor::cell(),
          data,
          component_dofs,
          output_data.jacobian_2nd_derivatives);

      // calculate hessians of the Jacobians pushed forward to real cell
      // coordinates
      internal::MappingFEFieldImplementation::
        maybe_update_jacobian_pushed_forward_2nd_derivatives<dim,
                                                             spacedim,
                                                             VectorType>(
          QProjector<dim>::DataSetDescriptor::cell(),
          data,
          component_dofs,
          output_data.jacobian_pushed_forward_2nd_derivatives);

      // calculate gradients of the hessians of the Jacobians
      internal::MappingFEFieldImplementation::
        maybe_update_jacobian_3rd_derivatives<dim, spacedim, VectorType>(
          QProjector<dim>::DataSetDescriptor::cell(),
          data,
          component_dofs,
          output_data.jacobian_3rd_derivatives);

      // calculate gradients of the hessians of the Jacobians pushed forward to
      // real cell coordinates
      internal::MappingFEFieldImplementation::
        maybe_update_jacobian_pushed_forward_3rd_derivatives<dim,
                                                             spacedim,
                                                             VectorType>(
          QProjector<dim>::DataSetDescriptor::cell(),
          data,
          component_dofs,
          output_data.jacobian_pushed_forward_3rd_derivatives);
    }
}

namespace internal
{
  namespace MappingFEFieldImplementation
  {
    template <int dim, int spacedim, int rank, typename VectorType>
    void
    transform_fields(
      const ArrayView<const Tensor<rank, dim>>                &input,
      const MappingKind                                        mapping_kind,
      const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
      const ArrayView<Tensor<rank, spacedim>>                 &output)
    {
      AssertDimension(input.size(), output.size());
      Assert(
        (dynamic_cast<const typename MappingFEField<dim, spacedim, VectorType>::
                        InternalData *>(&mapping_data) != nullptr),
        ExcInternalError());
      const typename MappingFEField<dim, spacedim, VectorType>::InternalData
        &data =
          static_cast<const typename MappingFEField<dim, spacedim, VectorType>::
                        InternalData &>(mapping_data);

      switch (mapping_kind)
        {
          case mapping_contravariant:
            {
              Assert(data.update_each & update_contravariant_transformation,
                     typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                       "update_contravariant_transformation"));

              for (unsigned int i = 0; i < output.size(); ++i)
                output[i] =
                  apply_transformation(data.contravariant[i], input[i]);

              return;
            }

          case mapping_piola:
            {
              Assert(data.update_each & update_contravariant_transformation,
                     typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                       "update_contravariant_transformation"));
              Assert(data.update_each & update_volume_elements,
                     typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                       "update_volume_elements"));
              Assert(rank == 1, ExcMessage("Only for rank 1"));
              for (unsigned int i = 0; i < output.size(); ++i)
                {
                  output[i] =
                    apply_transformation(data.contravariant[i], input[i]);
                  output[i] /= data.volume_elements[i];
                }
              return;
            }


          // We still allow this operation as in the
          // reference cell Derivatives are Tensor
          // rather than DerivativeForm
          case mapping_covariant:
            {
              Assert(data.update_each & update_contravariant_transformation,
                     typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                       "update_contravariant_transformation"));

              for (unsigned int i = 0; i < output.size(); ++i)
                output[i] = apply_transformation(data.covariant[i], input[i]);

              return;
            }

          default:
            DEAL_II_NOT_IMPLEMENTED();
        }
    }


    template <int dim, int spacedim, int rank, typename VectorType>
    void
    transform_differential_forms(
      const ArrayView<const DerivativeForm<rank, dim, spacedim>> &input,
      const MappingKind                                           mapping_kind,
      const typename Mapping<dim, spacedim>::InternalDataBase    &mapping_data,
      const ArrayView<Tensor<rank + 1, spacedim>>                &output)
    {
      AssertDimension(input.size(), output.size());
      Assert(
        (dynamic_cast<const typename MappingFEField<dim, spacedim, VectorType>::
                        InternalData *>(&mapping_data) != nullptr),
        ExcInternalError());
      const typename MappingFEField<dim, spacedim, VectorType>::InternalData
        &data =
          static_cast<const typename MappingFEField<dim, spacedim, VectorType>::
                        InternalData &>(mapping_data);

      switch (mapping_kind)
        {
          case mapping_covariant:
            {
              Assert(data.update_each & update_contravariant_transformation,
                     typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                       "update_contravariant_transformation"));

              for (unsigned int i = 0; i < output.size(); ++i)
                output[i] = apply_transformation(data.covariant[i], input[i]);

              return;
            }
          default:
            DEAL_II_NOT_IMPLEMENTED();
        }
    }
  } // namespace MappingFEFieldImplementation
} // namespace internal



template <int dim, int spacedim, typename VectorType>
void
MappingFEField<dim, spacedim, VectorType>::transform(
  const ArrayView<const Tensor<1, dim>>                   &input,
  const MappingKind                                        mapping_kind,
  const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
  const ArrayView<Tensor<1, spacedim>>                    &output) const
{
  AssertDimension(input.size(), output.size());

  internal::MappingFEFieldImplementation::
    transform_fields<dim, spacedim, 1, VectorType>(input,
                                                   mapping_kind,
                                                   mapping_data,
                                                   output);
}



template <int dim, int spacedim, typename VectorType>
void
MappingFEField<dim, spacedim, VectorType>::transform(
  const ArrayView<const DerivativeForm<1, dim, spacedim>> &input,
  const MappingKind                                        mapping_kind,
  const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
  const ArrayView<Tensor<2, spacedim>>                    &output) const
{
  AssertDimension(input.size(), output.size());

  internal::MappingFEFieldImplementation::
    transform_differential_forms<dim, spacedim, 1, VectorType>(input,
                                                               mapping_kind,
                                                               mapping_data,
                                                               output);
}



template <int dim, int spacedim, typename VectorType>
void
MappingFEField<dim, spacedim, VectorType>::transform(
  const ArrayView<const Tensor<2, dim>> &input,
  const MappingKind,
  const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
  const ArrayView<Tensor<2, spacedim>>                    &output) const
{
  (void)input;
  (void)output;
  (void)mapping_data;
  AssertDimension(input.size(), output.size());

  AssertThrow(false, ExcNotImplemented());
}



template <int dim, int spacedim, typename VectorType>
void
MappingFEField<dim, spacedim, VectorType>::transform(
  const ArrayView<const DerivativeForm<2, dim, spacedim>> &input,
  const MappingKind                                        mapping_kind,
  const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
  const ArrayView<Tensor<3, spacedim>>                    &output) const
{
  AssertDimension(input.size(), output.size());
  Assert(dynamic_cast<const InternalData *>(&mapping_data) != nullptr,
         ExcInternalError());
  const InternalData &data = static_cast<const InternalData &>(mapping_data);

  switch (mapping_kind)
    {
      case mapping_covariant_gradient:
        {
          Assert(data.update_each & update_covariant_transformation,
                 typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                   "update_covariant_transformation"));

          for (unsigned int q = 0; q < output.size(); ++q)
            output[q] =
              internal::apply_covariant_gradient(data.covariant[q], input[q]);

          return;
        }

      default:
        DEAL_II_NOT_IMPLEMENTED();
    }
}



template <int dim, int spacedim, typename VectorType>
void
MappingFEField<dim, spacedim, VectorType>::transform(
  const ArrayView<const Tensor<3, dim>> &input,
  const MappingKind /*mapping_kind*/,
  const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
  const ArrayView<Tensor<3, spacedim>>                    &output) const
{
  (void)input;
  (void)output;
  (void)mapping_data;
  AssertDimension(input.size(), output.size());

  AssertThrow(false, ExcNotImplemented());
}



template <int dim, int spacedim, typename VectorType>
Point<spacedim>
MappingFEField<dim, spacedim, VectorType>::transform_unit_to_real_cell(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const Point<dim>                                           &p) const
{
  //  Use the get_data function to create an InternalData with data vectors of
  //  the right size and transformation shape values already computed at point
  //  p.
  const Quadrature<dim> point_quadrature(p);
  std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase> mdata(
    get_data(update_quadrature_points | update_jacobians, point_quadrature));
  Assert(dynamic_cast<InternalData *>(mdata.get()) != nullptr,
         ExcInternalError());

  update_internal_dofs(cell, static_cast<InternalData &>(*mdata));

  return do_transform_unit_to_real_cell(static_cast<InternalData &>(*mdata));
}


template <int dim, int spacedim, typename VectorType>
Point<spacedim>
MappingFEField<dim, spacedim, VectorType>::do_transform_unit_to_real_cell(
  const InternalData &data) const
{
  Point<spacedim> p_real;

  for (unsigned int d = 0; d < spacedim; ++d)
    for (const auto &i : component_dofs[d])
      p_real[d] += data.local_dof_values[i] * data.shape(0, i);

  return p_real;
}



template <int dim, int spacedim, typename VectorType>
Point<dim>
MappingFEField<dim, spacedim, VectorType>::transform_real_to_unit_cell(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const Point<spacedim>                                      &p) const
{
  // first a Newton iteration based on the real mapping. It uses the center
  // point of the cell as a starting point
  Point<dim> initial_p_unit;
  try
    {
      initial_p_unit = get_default_linear_mapping(cell->get_triangulation())
                         .transform_real_to_unit_cell(cell, p);
    }
  catch (const typename Mapping<dim, spacedim>::ExcTransformationFailed &)
    {
      // mirror the conditions of the code below to determine if we need to
      // use an arbitrary starting point or if we just need to rethrow the
      // exception
      for (unsigned int d = 0; d < dim; ++d)
        initial_p_unit[d] = 0.5;
    }

  initial_p_unit = cell->reference_cell().closest_point(initial_p_unit);

  UpdateFlags update_flags = update_quadrature_points | update_jacobians;
  if (spacedim > dim)
    update_flags |= update_jacobian_grads;
  std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase> mdata(
    get_data(update_flags, Quadrature<dim>(initial_p_unit)));
  Assert(dynamic_cast<InternalData *>(mdata.get()) != nullptr,
         ExcInternalError());

  update_internal_dofs(cell, static_cast<InternalData &>(*mdata));

  return do_transform_real_to_unit_cell(cell,
                                        p,
                                        initial_p_unit,
                                        static_cast<InternalData &>(*mdata));
}


template <int dim, int spacedim, typename VectorType>
Point<dim>
MappingFEField<dim, spacedim, VectorType>::do_transform_real_to_unit_cell(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const Point<spacedim>                                      &p,
  const Point<dim>                                           &starting_guess,
  InternalData                                               &mdata) const
{
  const unsigned int n_shapes = mdata.shape_values.size();
  (void)n_shapes;
  Assert(n_shapes != 0, ExcInternalError());
  AssertDimension(mdata.shape_derivatives.size(), n_shapes);


  // Newton iteration to solve
  // f(x)=p(x)-p=0
  // x_{n+1}=x_n-[f'(x)]^{-1}f(x)
  // The start value was set to be the
  // linear approximation to the cell
  // The shape values and derivatives
  // of the mapping at this point are
  // previously computed.

  Point<dim> p_unit = starting_guess;
  Point<dim> f;
  mdata.reinit(mdata.update_each, Quadrature<dim>(starting_guess));

  Point<spacedim>     p_real(do_transform_unit_to_real_cell(mdata));
  Tensor<1, spacedim> p_minus_F              = p - p_real;
  const double        eps                    = 1.e-12 * cell->diameter();
  const unsigned int  newton_iteration_limit = 20;
  unsigned int        newton_iteration       = 0;
  while (p_minus_F.norm_square() > eps * eps)
    {
      // f'(x)
      Point<spacedim> DF[dim];
      Tensor<2, dim>  df;
      for (unsigned int d = 0; d < spacedim; ++d)
        for (const auto &i : component_dofs[d])
          {
            const Tensor<1, dim> &grad_i = mdata.derivative(0, i);
            for (unsigned int j = 0; j < dim; ++j)
              DF[j][d] += mdata.local_dof_values[i] * grad_i[j];
          }
      for (unsigned int j = 0; j < dim; ++j)
        {
          f[j] = DF[j] * p_minus_F;
          for (unsigned int l = 0; l < dim; ++l)
            df[j][l] = -DF[j] * DF[l];
        }
      // Solve  [f'(x)]d=f(x)
      const Tensor<1, dim> delta =
        invert(df) * static_cast<const Tensor<1, dim> &>(f);
      // do a line search
      double step_length = 1;
      do
        {
          // update of p_unit. The
          // spacedimth component of
          // transformed point is simply
          // ignored in codimension one
          // case. When this component is
          // not zero, then we are
          // projecting the point to the
          // surface or curve identified
          // by the cell.
          Point<dim> p_unit_trial = p_unit;
          for (unsigned int i = 0; i < dim; ++i)
            p_unit_trial[i] -= step_length * delta[i];
          // shape values and derivatives
          // at new p_unit point
          mdata.reinit(mdata.update_each, Quadrature<dim>(p_unit_trial));
          // f(x)
          const Point<spacedim> p_real_trial =
            do_transform_unit_to_real_cell(mdata);
          const Tensor<1, spacedim> f_trial = p - p_real_trial;
          // see if we are making progress with the current step length
          // and if not, reduce it by a factor of two and try again
          if (f_trial.norm() < p_minus_F.norm())
            {
              p_real    = p_real_trial;
              p_unit    = p_unit_trial;
              p_minus_F = f_trial;
              break;
            }
          else if (step_length > 0.05)
            step_length /= 2;
          else
            goto failure;
        }
      while (true);
      ++newton_iteration;
      if (newton_iteration > newton_iteration_limit)
        goto failure;
    }
  return p_unit;
  // if we get to the following label, then we have either run out
  // of Newton iterations, or the line search has not converged.
  // in either case, we need to give up, so throw an exception that
  // can then be caught
failure:
  AssertThrow(false,
              (typename Mapping<dim, spacedim>::ExcTransformationFailed()));
  // ...the compiler wants us to return something, though we can
  // of course never get here...
  return {};
}


template <int dim, int spacedim, typename VectorType>
unsigned int
MappingFEField<dim, spacedim, VectorType>::get_degree() const
{
  return euler_dof_handler->get_fe().degree;
}



template <int dim, int spacedim, typename VectorType>
ComponentMask
MappingFEField<dim, spacedim, VectorType>::get_component_mask() const
{
  return this->fe_mask;
}


template <int dim, int spacedim, typename VectorType>
std::unique_ptr<Mapping<dim, spacedim>>
MappingFEField<dim, spacedim, VectorType>::clone() const
{
  return std::make_unique<MappingFEField<dim, spacedim, VectorType>>(*this);
}


template <int dim, int spacedim, typename VectorType>
void
MappingFEField<dim, spacedim, VectorType>::update_internal_dofs(
  const typename Triangulation<dim, spacedim>::cell_iterator             &cell,
  const typename MappingFEField<dim, spacedim, VectorType>::InternalData &data)
  const
{
  Assert(euler_dof_handler != nullptr,
         ExcMessage("euler_dof_handler is empty"));

  typename DoFHandler<dim, spacedim>::cell_iterator dof_cell(*cell,
                                                             euler_dof_handler);
  Assert(uses_level_dofs || dof_cell->is_active() == true, ExcInactiveCell());
  if (uses_level_dofs)
    {
      AssertIndexRange(cell->level(), euler_vector.size());
      AssertDimension(euler_vector[cell->level()]->size(),
                      euler_dof_handler->n_dofs(cell->level()));
    }
  else
    AssertDimension(euler_vector[0]->size(), euler_dof_handler->n_dofs());

  if (uses_level_dofs)
    dof_cell->get_mg_dof_indices(data.local_dof_indices);
  else
    dof_cell->get_dof_indices(data.local_dof_indices);

  const VectorType &vector =
    uses_level_dofs ? *euler_vector[cell->level()] : *euler_vector[0];

  for (unsigned int i = 0; i < data.local_dof_values.size(); ++i)
    data.local_dof_values[i] =
      internal::ElementAccess<VectorType>::get(vector,
                                               data.local_dof_indices[i]);
}

DEAL_II_NAMESPACE_CLOSE

#endif
