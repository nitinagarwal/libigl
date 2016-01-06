// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#include "extract_cells.h"
#include "../../extract_manifold_patches.h"
#include "../../facet_components.h"
#include "../../triangle_triangle_adjacency.h"
#include "../../unique_edge_map.h"
#include "../../get_seconds.h"
#include "closest_facet.h"
#include "order_facets_around_edge.h"
#include "outer_facet.h"

#include <vector>
#include <queue>

//#define EXTRACT_CELLS_DEBUG

template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedC >
IGL_INLINE size_t igl::copyleft::cgal::extract_cells(
  const Eigen::PlainObjectBase<DerivedV>& V,
  const Eigen::PlainObjectBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedC>& cells)
{
  const size_t num_faces = F.rows();
  // Construct edge adjacency
  Eigen::MatrixXi E, uE;
  Eigen::VectorXi EMAP;
  std::vector<std::vector<size_t> > uE2E;
  igl::unique_edge_map(F, E, uE, EMAP, uE2E);
  // Cluster into manifold patches
  Eigen::VectorXi P;
  igl::extract_manifold_patches(F, EMAP, uE2E, P);
  // Extract cells
  DerivedC per_patch_cells;
  const size_t num_cells =
    igl::copyleft::cgal::extract_cells(V,F,P,E,uE,uE2E,EMAP,per_patch_cells);
  // Distribute per-patch cell information to each face
  cells.resize(num_faces, 2);
  for (size_t i=0; i<num_faces; i++) 
  {
    cells.row(i) = per_patch_cells.row(P[i]);
  }
  return num_cells;
}


template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedP,
  typename DerivedE,
  typename DeriveduE,
  typename uE2EType,
  typename DerivedEMAP,
  typename DerivedC >
IGL_INLINE size_t igl::copyleft::cgal::extract_cells(
  const Eigen::PlainObjectBase<DerivedV>& V,
  const Eigen::PlainObjectBase<DerivedF>& F,
  const Eigen::PlainObjectBase<DerivedP>& P,
  const Eigen::PlainObjectBase<DerivedE>& E,
  const Eigen::PlainObjectBase<DeriveduE>& uE,
  const std::vector<std::vector<uE2EType> >& uE2E,
  const Eigen::PlainObjectBase<DerivedEMAP>& EMAP,
  Eigen::PlainObjectBase<DerivedC>& cells) 
{
#ifdef EXTRACT_CELLS_DEBUG
  const auto & tictoc = []() -> double
  {
    static double t_start = igl::get_seconds();
    double diff = igl::get_seconds()-t_start;
    t_start += diff;
    return diff;
  };
  const auto log_time = [&](const std::string& label) -> void {
    std::cout << "extract_cells." << label << ": "
      << tictoc() << std::endl;
  };
  tictoc();
#else
  // no-op
  const auto log_time = [](const std::string){};
#endif
  const size_t num_faces = F.rows();
  typedef typename DerivedF::Scalar Index;
  const size_t num_patches = P.maxCoeff()+1;

  // Extract all cells...
  DerivedC raw_cells;
  const size_t num_raw_cells =
    extract_cells_single_component(V,F,P,uE,uE2E,EMAP,raw_cells);
  log_time("extract_single_component_cells");

  // Compute triangle-triangle adjacency data-structure
  std::vector<std::vector<std::vector<Index > > > TT,_1;
  igl::triangle_triangle_adjacency(E, EMAP, uE2E, false, TT, _1);
  log_time("compute_face_adjacency");

  // Compute connected components of the mesh
  Eigen::VectorXi C, counts;
  igl::facet_components(TT, C, counts);
  log_time("form_components");

  const size_t num_components = counts.size();
  // components[c] --> list of face indices into F of faces in component c
  std::vector<std::vector<size_t> > components(num_components);
  // Loop over all faces
  for (size_t i=0; i<num_faces; i++) 
  {
    components[C[i]].push_back(i);
  }
  // Convert vector lists to Eigen lists...
  std::vector<Eigen::VectorXi> Is(num_components);
  for (size_t i=0; i<num_components; i++)
  {
    Is[i].resize(components[i].size());
    std::copy(components[i].begin(), components[i].end(),Is[i].data());
  }

  // Find outer facets, their orientations and cells for each component
  Eigen::VectorXi outer_facets(num_components);
  Eigen::VectorXi outer_facet_orientation(num_components);
  Eigen::VectorXi outer_cells(num_components);
  for (size_t i=0; i<num_components; i++)
  {
    bool flipped;
    igl::copyleft::cgal::outer_facet(V, F, Is[i], outer_facets[i], flipped);
    outer_facet_orientation[i] = flipped?1:0;
    outer_cells[i] = raw_cells(P[outer_facets[i]], outer_facet_orientation[i]);
  }
#ifdef EXTRACT_CELLS_DEBUG
  log_time("outer_facet_per_component");
#endif

  // Compute barycenter of a triangle in mesh (V,F)
  //
  // Inputs:
  //   fid  index into F
  // Returns row-vector of barycenter coordinates
  const auto get_triangle_center = [&V,&F](const size_t fid) 
  {
    return ((V.row(F(fid,0))+V.row(F(fid,1))+V.row(F(fid,2)))/3.0).eval();
  };
  std::vector<std::vector<size_t> > nested_cells(num_raw_cells);
  std::vector<std::vector<size_t> > ambient_cells(num_raw_cells);
  std::vector<std::vector<size_t> > ambient_comps(num_components);
  // Only bother if there's more than one component
  if(num_components > 1) 
  {
    // construct bounding boxes for each component
    DerivedV bbox_min(num_components, 3);
    DerivedV bbox_max(num_components, 3);
    // Why not just initialize to numeric_limits::min, numeric_limits::max?
    bbox_min.rowwise() = V.colwise().maxCoeff().eval();
    bbox_max.rowwise() = V.colwise().minCoeff().eval();
    // Loop over faces
    for (size_t i=0; i<num_faces; i++)
    {
      // component of this face
      const auto comp_id = C[i];
      const auto& f = F.row(i);
      for (size_t j=0; j<3; j++) 
      {
        for(size_t d=0;d<3;d++)
        {
          bbox_min(comp_id,d) = std::min(bbox_min(comp_id,d), V(f[j],d));
          bbox_max(comp_id,d) = std::max(bbox_max(comp_id,d), V(f[j],d));
        }
      }
    }
    // Return true if box of component ci intersects that of cj
    const auto bbox_intersects = [&bbox_max,&bbox_min](size_t ci, size_t cj)
    {
      return !(
        bbox_max(ci,0) < bbox_min(cj,0) ||
        bbox_max(ci,1) < bbox_min(cj,1) ||
        bbox_max(ci,2) < bbox_min(cj,2) ||
        bbox_max(cj,0) < bbox_min(ci,0) ||
        bbox_max(cj,1) < bbox_min(ci,1) ||
        bbox_max(cj,2) < bbox_min(ci,2));
    };
    // Loop over components. This section is O(m²)
    for (size_t i=0; i<num_components; i++)
    {
      // List of components that could overlap with component i
      std::vector<size_t> candidate_comps;
      candidate_comps.reserve(num_components);
      // Loop over components
      for (size_t j=0; j<num_components; j++) 
      {
        if (i == j) continue;
        if (bbox_intersects(i,j)) candidate_comps.push_back(j);
      }

      const size_t num_candidate_comps = candidate_comps.size();
      if (num_candidate_comps == 0) continue;

      // Get query points on each candidate component: barycenter of
      // outer-facet 
      DerivedV queries(num_candidate_comps, 3);
      for (size_t j=0; j<num_candidate_comps; j++)
      {
        const size_t index = candidate_comps[j];
        queries.row(j) = get_triangle_center(outer_facets[index]);
      }

      // Gather closest facets to each query point and their orientations
      const auto& I = Is[i];
      Eigen::VectorXi closest_facets, closest_facet_orientations;
      closest_facet(V, F, I, queries,
        uE2E, EMAP, closest_facets, closest_facet_orientations);
      // Loop over all candidates
      for (size_t j=0; j<num_candidate_comps; j++)
      {
        const size_t index = candidate_comps[j];
        const size_t closest_patch = P[closest_facets[j]];
        const size_t closest_patch_side = closest_facet_orientations[j] ? 0:1;
        const size_t ambient_cell =
          raw_cells(closest_patch,closest_patch_side);
        if (ambient_cell != (size_t)outer_cells[i])
        {
          nested_cells[ambient_cell].push_back(outer_cells[index]);
          ambient_cells[outer_cells[index]].push_back(ambient_cell);
          ambient_comps[index].push_back(i);
        }
      }
    }
  }
#ifdef EXTRACT_CELLS_DEBUG
    log_time("nested_relationship");
#endif

    const size_t INVALID = std::numeric_limits<size_t>::max();
    const size_t INFINITE_CELL = num_raw_cells;
    std::vector<size_t> embedded_cells(num_raw_cells, INVALID);
    for (size_t i=0; i<num_components; i++) {
        const size_t outer_cell = outer_cells[i];
        const auto& ambient_comps_i = ambient_comps[i];
        const auto& ambient_cells_i = ambient_cells[outer_cell];
        const size_t num_ambient_comps = ambient_comps_i.size();
        assert(num_ambient_comps == ambient_cells_i.size());
        if (num_ambient_comps > 0) {
            size_t embedded_comp = INVALID;
            size_t embedded_cell = INVALID;
            for (size_t j=0; j<num_ambient_comps; j++) {
                if (ambient_comps[ambient_comps_i[j]].size() ==
                        num_ambient_comps-1) {
                    embedded_comp = ambient_comps_i[j];
                    embedded_cell = ambient_cells_i[j];
                    break;
                }
            }
            assert(embedded_comp != INVALID);
            assert(embedded_cell != INVALID);
            embedded_cells[outer_cell] = embedded_cell;
        } else {
            embedded_cells[outer_cell] = INFINITE_CELL;
        }
    }
    for (size_t i=0; i<num_patches; i++) {
        if (embedded_cells[raw_cells(i,0)] != INVALID) {
            raw_cells(i,0) = embedded_cells[raw_cells(i, 0)];
        }
        if (embedded_cells[raw_cells(i,1)] != INVALID) {
            raw_cells(i,1) = embedded_cells[raw_cells(i, 1)];
        }
    }

    size_t count = 0;
    std::vector<size_t> mapped_indices(num_raw_cells+1, INVALID);
    // Always map infinite cell to index 0.
    mapped_indices[INFINITE_CELL] = count;
    count++;

    for (size_t i=0; i<num_patches; i++) {
        const size_t old_positive_cell_id = raw_cells(i, 0);
        const size_t old_negative_cell_id = raw_cells(i, 1);
        size_t positive_cell_id, negative_cell_id;
        if (mapped_indices[old_positive_cell_id] == INVALID) {
            mapped_indices[old_positive_cell_id] = count;
            positive_cell_id = count;
            count++;
        } else {
            positive_cell_id = mapped_indices[old_positive_cell_id];
        }
        if (mapped_indices[old_negative_cell_id] == INVALID) {
            mapped_indices[old_negative_cell_id] = count;
            negative_cell_id = count;
            count++;
        } else {
            negative_cell_id = mapped_indices[old_negative_cell_id];
        }
        raw_cells(i, 0) = positive_cell_id;
        raw_cells(i, 1) = negative_cell_id;
    }
    cells = raw_cells;
#ifdef EXTRACT_CELLS_DEBUG
    log_time("finalize");
#endif
    return count;
}

template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedP,
  typename DeriveduE,
  typename uE2EType,
  typename DerivedEMAP,
  typename DerivedC>
IGL_INLINE size_t igl::copyleft::cgal::extract_cells_single_component(
  const Eigen::PlainObjectBase<DerivedV>& V,
  const Eigen::PlainObjectBase<DerivedF>& F,
  const Eigen::PlainObjectBase<DerivedP>& P,
  const Eigen::PlainObjectBase<DeriveduE>& uE,
  const std::vector<std::vector<uE2EType> >& uE2E,
  const Eigen::PlainObjectBase<DerivedEMAP>& EMAP,
  Eigen::PlainObjectBase<DerivedC>& cells)
{
  const size_t num_faces = F.rows();
  // Input:
  //   index  index into #F*3 list of undirect edges
  // Returns index into face
  const auto edge_index_to_face_index = [&num_faces](size_t index)
  {
    return index % num_faces;
  };
  // Determine if a face (containing undirected edge {s,d} is consistently
  // oriented with directed edge {s,d} (or otherwise it is with {d,s})
  // 
  // Inputs:
  //   fid  face index into F
  //   s  source index of edge
  //   d  destination index of edge
  // Returns true if face F(fid,:) is consistent with {s,d}
  const auto is_consistent = 
    [&F](const size_t fid, const size_t s, const size_t d) -> bool
  {
    if ((size_t)F(fid, 0) == s && (size_t)F(fid, 1) == d) return false;
    if ((size_t)F(fid, 1) == s && (size_t)F(fid, 2) == d) return false;
    if ((size_t)F(fid, 2) == s && (size_t)F(fid, 0) == d) return false;

    if ((size_t)F(fid, 0) == d && (size_t)F(fid, 1) == s) return true;
    if ((size_t)F(fid, 1) == d && (size_t)F(fid, 2) == s) return true;
    if ((size_t)F(fid, 2) == d && (size_t)F(fid, 0) == s) return true;
    throw "Invalid face!";
    return false;
  };

  const size_t num_unique_edges = uE.rows();
  const size_t num_patches = P.maxCoeff() + 1;

  // patch_edge_adj[p] --> list {e,f,g,...} such that p is a patch id and
  //   e,f,g etc. are edge indices into 
  std::vector<std::vector<size_t> > patch_edge_adj(num_patches);
  // orders[u] --> J  where u is an index into unique edges uE and J is a
  //   #adjacent-faces list of face-edge indices into F*3 sorted cyclicaly around
  //   edge u.
  std::vector<Eigen::VectorXi> orders(num_unique_edges);
  // orientations[u] ---> list {f1,f2, ...} where u is an index into unique edges uE
  //   and points to #adj-facets long list of flags whether faces are oriented
  //   to point their normals clockwise around edge when looking along the
  //   edge.
  std::vector<std::vector<bool> > orientations(num_unique_edges);
  // Loop over unique edges
  for (size_t i=0; i<num_unique_edges; i++) 
  {
    const size_t s = uE(i,0);
    const size_t d = uE(i,1);
    const auto adj_faces = uE2E[i];
    // If non-manifold (more than two incident faces)
    if (adj_faces.size() > 2) 
    {
      // signed_adj_faces[a] --> sid  where a is an index into adj_faces
      // (list of face edges on {s,d}) and sid is a signed index for resolve
      // co-planar duplicates consistently (i.e. using simulation of
      // simplicity).
      std::vector<int> signed_adj_faces;
      for (auto ei : adj_faces)
      {
        const size_t fid = edge_index_to_face_index(ei);
        bool cons = is_consistent(fid, s, d);
        signed_adj_faces.push_back((fid+1)*(cons ? 1:-1));
      }
      {
        // Sort adjacent faces cyclically around {s,d}
        auto& order = orders[i];
        // order[f] will reveal the order of face f in signed_adj_faces
        order_facets_around_edge(V, F, s, d, signed_adj_faces, order);
        // Determine if each facet is oriented to point its normal clockwise or
        // not around the {s,d} (normals are not explicitly computed/used)
        auto& orientation = orientations[i];
        orientation.resize(order.size());
        std::transform(
          order.data(), 
          order.data() + order.size(),
          orientation.begin(), 
          [&signed_adj_faces](const int i){ return signed_adj_faces[i] > 0;});
        // re-index order from adjacent faces to full face list. Now order
        // indexes F directly
        std::transform(
          order.data(), 
          order.data() + order.size(),
          order.data(), 
          [&adj_faces](const int index){ return adj_faces[index];});
      }
      // loop over adjacent faces, remember that patch is adjacent to this edge
      for(const auto & ei : adj_faces)
      {
        const size_t fid = edge_index_to_face_index(ei);
        patch_edge_adj[P[fid]].push_back(ei);
      }
    }
  }

  // Initialize all patch-to-cell indices as "invalid" (unlabeled)
  const int INVALID = std::numeric_limits<int>::max();
  cells.resize(num_patches, 2);
  cells.setConstant(INVALID);

  // Given a "seed" patch id, a cell id, and which side of the patch that cell
  // lies, identify all other patches bounding this cell (and tell them that
  // they do)
  //
  // Inputs:
  //   seed_patch_id  index into patches
  //   cell_idx  index into cells
  //   seed_patch_side   0 or 1 depending on whether cell_idx is on left or
  //     right side of seed_patch_id 
  //   cells  #cells by 2 list of current assignment of cells incident on each
  //     side of a patch
  // Outputs:
  //   cells  udpated (see input)
  // 
  const auto & peel_cell_bd = 
    [&P,&patch_edge_adj,&EMAP,&orders,&orientations,&num_faces](
    const size_t seed_patch_id, 
    const short seed_patch_side, 
    const size_t cell_idx,
    Eigen::PlainObjectBase<DerivedC>& cells)
  {
    typedef std::pair<size_t, short> PatchSide;
    // Initialize a queue of {p,side} patch id and patch side pairs to BFS over
    // all patches
    std::queue<PatchSide> Q;
    Q.emplace(seed_patch_id, seed_patch_side);
    // assign cell id of seed patch on appropriate side
    cells(seed_patch_id, seed_patch_side) = cell_idx;
    while (!Q.empty())
    {
      // Pop patch from Q
      const auto entry = Q.front();
      Q.pop();
      const size_t patch_id = entry.first;
      const short side = entry.second;
      // face-edges adjacent to patch
      const auto& adj_edges = patch_edge_adj[patch_id];
      // Loop over **ALL EDGES IN THE ENTIRE PATCH** not even just the boundary
      // edges, all edges... O(n)
      for (const auto& ei : adj_edges)
      {
        // unique edge
        const size_t uei = EMAP[ei];
        // ordering of face-edges stored at edge
        const auto& order = orders[uei];
        // consistent orientation flags at face-edges stored at edge
        const auto& orientation = orientations[uei];
        const size_t edge_valance = order.size();
        // Search for ei's (i.e. patch_id's) place in the ordering: O(#patches)
        size_t curr_i = 0;
        for (curr_i=0; curr_i < edge_valance; curr_i++)
        {
          if ((size_t)order[curr_i] == ei) break;
        }
        assert(curr_i < edge_valance && "Failed to find edge.");
        // is the starting face consistent?
        const bool cons = orientation[curr_i];
        // Look clockwise or counter-clockwise for the next face, depending on
        // whether looking to left or right side and whether consistently
        // oriented or not.
        size_t next;
        if (side == 0)
        {
          next = (cons ? (curr_i + 1) :
          (curr_i + edge_valance - 1)) % edge_valance;
        } else {
          next = (cons ? (curr_i+edge_valance-1) : (curr_i+1))%edge_valance;
        }
        // Determine face-edge index of next face
        const size_t next_ei = order[next];
        // Determine whether next is consistently oriented
        const bool next_cons = orientation[next];
        // Determine patch of next
        const size_t next_patch_id = P[next_ei % num_faces];
        // Determine which side of patch cell_idx is on, based on whether the
        // consistency of next matches the consistency of this patch and which
        // side of this patch we're on.
        const short next_patch_side = (next_cons != cons) ?  side:abs(side-1);
        // If that side of next's patch is not labeled, then label and add to
        // queue
        if (cells(next_patch_id, next_patch_side) == INVALID) 
        {
          Q.emplace(next_patch_id, next_patch_side);
          cells(next_patch_id, next_patch_side) = cell_idx;
        }else 
        {
          assert(
            (size_t)cells(next_patch_id, next_patch_side) == cell_idx && 
            "Encountered cell assignment inconsistency");
        }
      }
    }
  };

  size_t count=0;
  // Loop over all patches
  for (size_t i=0; i<num_patches; i++)
  {
    // If left side of patch is still unlabeled, create a new cell and find all
    // patches also on its boundary
    if (cells(i, 0) == INVALID)
    {
      peel_cell_bd(i, 0, count,cells);
      count++;
    }
    // Likewise for right side
    if (cells(i, 1) == INVALID)
    {
      peel_cell_bd(i, 1, count,cells);
      count++;
    }
  }
  return count;
}


#ifdef IGL_STATIC_LIBRARY
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
template unsigned long igl::copyleft::cgal::extract_cells<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, unsigned long, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<unsigned long, std::allocator<unsigned long> >, std::allocator<std::vector<unsigned long, std::allocator<unsigned long> > > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif