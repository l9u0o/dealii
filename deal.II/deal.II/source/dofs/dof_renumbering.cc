//---------------------------------------------------------------------------
//    $id$
//    Version: $name$
//
//    Copyright (C) 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008 by the deal.II authors
//
//    This file is subject to QPL and may not be  distributed
//    without copyright and license information. Please refer
//    to the file deal.II/doc/license.html for the  text  and
//    further information on this license.
//
//---------------------------------------------------------------------------


//TODO:[WB] Unify lots of code of the two Cuthill-McKee dof renumbering functions
//    This should be rather
//    straightforward, since all the unified code needs to get is a
//    sparsity structure, possibly compressed and return a vector
//    of numbers. Simple task.

#include <base/thread_management.h>
#include <lac/sparsity_pattern.h>
#include <lac/compressed_sparsity_pattern.h>
#include <dofs/dof_accessor.h>
#include <grid/tria_iterator.h>
#include <grid/tria.h>
#include <dofs/dof_handler.h>
#include <hp/dof_handler.h>
#include <dofs/dof_constraints.h>
#include <dofs/dof_tools.h>
#include <fe/fe.h>
#include <dofs/dof_renumbering.h>

#include <multigrid/mg_dof_handler.h>
#include <multigrid/mg_dof_accessor.h>
#include <multigrid/mg_tools.h>

#include <boost/config.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/cuthill_mckee_ordering.hpp>
#include "boost/graph/minimum_degree_ordering.hpp"
#include <boost/graph/properties.hpp>
#include <boost/graph/bandwidth.hpp>

#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <functional>


// for whatever reason, the random_shuffle function used below needs
// lrand48 to be declared when using -ansi as compiler flag (rather
// than do so itself). however, inclusion of <cstdlib> or <stdlib.h>
// does not help, so we declare that function ourselves. Since this
// holds only for some compiler versions, do so conditionally on a
// ./configure-time test
#ifdef DEAL_II_DECLARE_LRAND48
extern "C" long int lrand48 (void);
#endif

DEAL_II_NAMESPACE_OPEN



// The following two classes are defined to be used in the compute_*
// functions. Using them allows to use the same function to compute
// level numbering for multigrid as well as numbering for the global
// system matrix.
template <class T>
class WrapDoFIterator : private T
{
  public:
    WrapDoFIterator (const T& t) : T(t) {}

    void get_dof_indices (std::vector<unsigned int>& v) const
      {
	(*this)->get_dof_indices(v);
      }

    template <class T2>
    bool operator != (const T2& i) const
      {
	return (! (T::operator==(i)));
      }
				     // Allow access to these operators of T
    T::operator->;
    T::operator++;
    T::operator==;
};



template <class T>
class WrapMGDoFIterator : private T
{
  public:
    WrapMGDoFIterator (const T& t) : T(t) {}

    void get_dof_indices (std::vector<unsigned int>& v) const
      {
	(*this)->get_mg_dof_indices(v);
      }

    bool operator != (const WrapMGDoFIterator<T>& i) const
      {
	return (! (T::operator==(i)));
      }
				     // Allow access to these operators of T
    T::operator->;
    T::operator++;
    T::operator==;
};



/**
 * Provide comparator for DoFCellAccessors: it returns @p true if the center
 * of the second cell is downstream of the center of the first one with
 * respect to the direction given to the constructor.
 */
template <int dim>
struct CompareDownstream
{
				     /**
				      * Constructor.
				      */
    CompareDownstream (const Point<dim> &dir)
                    :
		    dir(dir) 
      {}
				     /**
				      * Return true if c1 < c2.
				      */
    template <class DHCellIterator>
    bool operator () (const DHCellIterator &c1,
		      const DHCellIterator &c2) const
      {
	const Point<dim> diff = c2->center() - c1->center();
	return (diff*dir > 0);
      }

  private:
				     /**
				      * Flow direction.
				      */
    const Point<dim> dir;
};


namespace types
{
  using namespace boost;
  using namespace std;

  typedef vecS x;
    
  typedef adjacency_list<vecS, vecS, undirectedS, 
			 property<vertex_color_t, default_color_type,
				  property<vertex_degree_t,int> > > Graph;
  typedef graph_traits<Graph>::vertex_descriptor Vertex;
  typedef graph_traits<Graph>::vertices_size_type size_type;

  typedef std::pair<size_type, size_type> Pair;
}
  

namespace
{
  template <class DH>
  void create_graph (const DH                                                       &dof_handler,
		     const bool                                                      use_constraints,
		     types::Graph                                                   &graph,
		     types::property_map<types::Graph,types::vertex_degree_t>::type &graph_degree)
  {
    Assert (use_constraints == false, ExcNotImplemented());
    
    std::vector<unsigned int> dofs_on_this_cell;
    dofs_on_this_cell.reserve (DoFTools::max_dofs_per_cell(dof_handler));
    typename DH::active_cell_iterator cell = dof_handler.begin_active(),
				      endc = dof_handler.end();
    for (; cell!=endc; ++cell) 
      {
	const unsigned int dofs_per_cell = cell->get_fe().dofs_per_cell;
	dofs_on_this_cell.resize (dofs_per_cell);
	cell->get_dof_indices (dofs_on_this_cell);

	for (unsigned int i=0; i<dofs_per_cell; ++i)
	  for (unsigned int j=0; j<dofs_per_cell; ++j)
	    add_edge(dofs_on_this_cell[i], dofs_on_this_cell[j], graph);
      }


    types::graph_traits<types::Graph>::vertex_iterator ui, ui_end;

    graph_degree = get(boost::vertex_degree, graph);
    for (boost::tie(ui, ui_end) = vertices(graph); ui != ui_end; ++ui)
      graph_degree[*ui] = degree(*ui, graph);

  }
}

  
template <class DH>
void
DoFRenumbering::boost::
Cuthill_McKee (DH&              dof_handler,
	       const bool       reversed_numbering,
	       const bool       use_constraints)
{
  types::Graph
    graph(dof_handler.n_dofs());
  types::property_map<types::Graph,types::vertex_degree_t>::type
    graph_degree;

  create_graph (dof_handler, use_constraints, graph, graph_degree);
    
  types::property_map<types::Graph, types::vertex_index_t>::type
    index_map = get(::boost::vertex_index, graph);


  std::vector<types::Vertex> inv_perm(num_vertices(graph));

  if (reversed_numbering == false)
    ::boost::cuthill_mckee_ordering(graph, inv_perm.rbegin(),
				    get(::boost::vertex_color, graph),
				    make_degree_map(graph));
  else
    ::boost::cuthill_mckee_ordering(graph, inv_perm.begin(),
				    get(::boost::vertex_color, graph),
				    make_degree_map(graph));
    
  std::vector<unsigned int> perm(num_vertices(graph));
  for (types::size_type c = 0; c != inv_perm.size(); ++c)
    perm[index_map[inv_perm[c]]] = c;

  dof_handler.renumber_dofs (perm);
}


  


template <class DH>
void
DoFRenumbering::
Cuthill_McKee (DH&              dof_handler,
               const bool       reversed_numbering,
               const bool       use_constraints,
               const std::vector<unsigned int> &starting_indices)
{
  std::vector<unsigned int> renumbering(dof_handler.n_dofs(),
					DH::invalid_dof_index);
  compute_Cuthill_McKee(renumbering, dof_handler, reversed_numbering,
			use_constraints, starting_indices);

				   // actually perform renumbering;
				   // this is dimension specific and
				   // thus needs an own function
  dof_handler.renumber_dofs (renumbering);
}



template <class DH>
void
DoFRenumbering::
compute_Cuthill_McKee (std::vector<unsigned int>& new_indices,
                       const DH&                  dof_handler,
                       const bool                 reversed_numbering,
                       const bool                 use_constraints,
                       const std::vector<unsigned int>& starting_indices)
{
				   // make the connection graph. in
				   // more than 2d use an intermediate
				   // compressed sparsity pattern
				   // since the we don't have very
				   // good estimates for
				   // max_couplings_between_dofs() in
				   // 3d and this then leads to
				   // excessive memory consumption
				   //
				   // note that if constraints are not
				   // requested, then the
				   // 'constraints' object will be
				   // empty, and calling condense with
				   // it is a no-op
  ConstraintMatrix constraints;
  if (use_constraints) 
    DoFTools::make_hanging_node_constraints (dof_handler, constraints);
  constraints.close ();
    
  SparsityPattern sparsity;
  if (DH::dimension <= 2)
    {
      sparsity.reinit (dof_handler.n_dofs(),
		       dof_handler.n_dofs(),
		       dof_handler.max_couplings_between_dofs());
      DoFTools::make_sparsity_pattern (dof_handler, sparsity);
      constraints.condense (sparsity);
    }
  else
    {
      CompressedSparsityPattern csp (dof_handler.n_dofs(),
				     dof_handler.n_dofs());
      DoFTools::make_sparsity_pattern (dof_handler, csp);
      constraints.condense (csp);
      sparsity.copy_from (csp);
    }

				   // constraints are not needed anymore
  constraints.clear ();
  
  const unsigned int n_dofs = sparsity.n_rows();
				   // store the new dof numbers;
				   // invalid_dof_index means that no new
				   // number was chosen yet
  Assert(new_indices.size() == n_dofs,
	 ExcDimensionMismatch(new_indices.size(), n_dofs));
  
				   // store the indices of the dofs renumbered
				   // in the last round. Default to starting
				   // points
  std::vector<unsigned int> last_round_dofs (starting_indices);
  
				   // delete disallowed elements
  for (unsigned int i=0; i<last_round_dofs.size(); ++i)
    if ((last_round_dofs[i]==DH::invalid_dof_index) ||
	(last_round_dofs[i]>=n_dofs))
      last_round_dofs[i] = DH::invalid_dof_index;
  
  std::remove_if (last_round_dofs.begin(), last_round_dofs.end(),
		  std::bind2nd(std::equal_to<unsigned int>(),
			       DH::invalid_dof_index));
  
				   // now if no valid points remain:
				   // find dof with lowest coordination
				   // number
  
  if (last_round_dofs.size() == 0)
    {
      unsigned int starting_point   = DH::invalid_dof_index;
      unsigned int min_coordination = n_dofs;
      for (unsigned int row=0; row<n_dofs; ++row) 
	{
	  unsigned int j;

					   // loop until we hit the end
					   // of this row's entries
	  for (j=sparsity.get_rowstart_indices()[row];
	       j<sparsity.get_rowstart_indices()[row+1]; ++j)
	    if (sparsity.get_column_numbers()[j] == SparsityPattern::invalid_entry)
	      break;
					   // post-condition after loop:
					   // coordination, i.e. the number
					   // of entries in this row is now
					   // j-rowstart[row]
	  if (j-sparsity.get_rowstart_indices()[row] <  min_coordination)
	    {
	      min_coordination = j-sparsity.get_rowstart_indices()[row];
	      starting_point   = row;
	    };
	};
      
				       // now we still have to care for the
				       // case that no dof has a coordination
				       // number less than n_dofs. this rather
				       // exotic case only happens if we only
				       // have one cell, as far as I can see,
				       // but there may be others as well.
				       //
				       // if that should be the case, we can
				       // chose an arbitrary dof as starting
				       // point, e.g. the one with number zero
      if (starting_point == DH::invalid_dof_index)
	starting_point = 0;
      
				       // initialize the first dof
      last_round_dofs.push_back (starting_point);
    };


				   // store next free dof index
  unsigned int next_free_number = 0;

				   // enumerate the first round dofs
  for (unsigned int i=0; i!=last_round_dofs.size(); ++i)
    new_indices[last_round_dofs[i]] = next_free_number++;

  bool all_dofs_renumbered = false;

				   // now do as many steps as needed to
				   // renumber all dofs
  while (!all_dofs_renumbered) 
    {
				       // store the indices of the dofs to be
				       // renumbered in the next round
      std::vector<unsigned int> next_round_dofs;

				       // find all neighbors of the
				       // dofs numbered in the last
				       // round
      for (unsigned int i=0; i<last_round_dofs.size(); ++i)
	for (unsigned int j=sparsity.get_rowstart_indices()[last_round_dofs[i]];
	     j<sparsity.get_rowstart_indices()[last_round_dofs[i]+1]; ++j)
	  if (sparsity.get_column_numbers()[j] == SparsityPattern::invalid_entry)
	    break;
	  else
	    next_round_dofs.push_back (sparsity.get_column_numbers()[j]);
      
				       // sort dof numbers
      std::sort (next_round_dofs.begin(), next_round_dofs.end());

				       // delete multiple entries
      std::vector<unsigned int>::iterator end_sorted;
      end_sorted = std::unique (next_round_dofs.begin(), next_round_dofs.end());
      next_round_dofs.erase (end_sorted, next_round_dofs.end());

				       // eliminate dofs which are
				       // already numbered
      for (int s=next_round_dofs.size()-1; s>=0; --s)
	if (new_indices[next_round_dofs[s]] != DH::invalid_dof_index)
	  next_round_dofs.erase (next_round_dofs.begin() + s);

				       // check whether there are any new
				       // dofs
      all_dofs_renumbered = (next_round_dofs.size() == 0);
      if (all_dofs_renumbered)
					 // end loop if possible
	continue;


				       // store for each coordination
				       // number the dofs with these
				       // coordination number
      std::multimap<unsigned int, int> dofs_by_coordination;
      
				       // find coordination number for
				       // each of these dofs
      for (std::vector<unsigned int>::iterator s=next_round_dofs.begin();
	   s!=next_round_dofs.end(); ++s) 
	{
	  unsigned int coordination = 0;
	  for (unsigned int j=sparsity.get_rowstart_indices()[*s];
	       j<sparsity.get_rowstart_indices()[*s+1]; ++j)
	    if (sparsity.get_column_numbers()[j] == SparsityPattern::invalid_entry)
	      break;
	    else
	      ++coordination;

					   // insert this dof at its
					   // coordination number
	  const std::pair<const unsigned int, int> new_entry (coordination, *s);
	  dofs_by_coordination.insert (new_entry);
	};
      
				       // assign new DoF numbers to
				       // the elements of the present
				       // front:
      std::multimap<unsigned int, int>::iterator i;
      for (i = dofs_by_coordination.begin(); i!=dofs_by_coordination.end(); ++i) 
	new_indices[i->second] = next_free_number++;

				       // after that: copy this round's
				       // dofs for the next round
      last_round_dofs = next_round_dofs;
    };

#ifdef DEBUG
				   // test for all indices
				   // numbered. this mostly tests
				   // whether the
				   // front-marching-algorithm (which
				   // Cuthill-McKee actually is) has
				   // reached all points. it should
				   // usually do so, but might not for
				   // two reasons:
				   //
				   // - The algorithm above has a bug, or
				   // - The domain is not connected and
				   // consists of separate parts.
				   //
				   // In any case, if not all DoFs
				   // have been reached, renumbering
				   // will not be possible
  if (std::find (new_indices.begin(), new_indices.end(), DH::invalid_dof_index)
      !=
      new_indices.end())
    Assert (false, ExcRenumberingIncomplete());
  Assert (next_free_number == n_dofs,
	  ExcRenumberingIncomplete());
#endif

  if (reversed_numbering)
    for (std::vector<unsigned int>::iterator i=new_indices.begin();
	 i!=new_indices.end(); ++i)
      *i = n_dofs-*i-1;
}


template <int dim>
void DoFRenumbering::Cuthill_McKee (
  MGDoFHandler<dim>               &dof_handler,
  const unsigned int               level,
  const bool                       reversed_numbering,
  const std::vector<unsigned int> &starting_indices)
{
//TODO: we should be doing the same here as in the other compute_CMK function to preserve some memory
  
				   // make the connection graph
  SparsityPattern sparsity (dof_handler.n_dofs(level),
			    dof_handler.max_couplings_between_dofs());
  MGTools::make_sparsity_pattern (dof_handler, sparsity, level);
    
  const unsigned int n_dofs = sparsity.n_rows();
				   // store the new dof numbers; invalid_dof_index means
				   // that no new number was chosen yet
  std::vector<unsigned int> new_indices(n_dofs, DoFHandler<dim>::invalid_dof_index);
  
				   // store the indices of the dofs renumbered
				   // in the last round. Default to starting
				   // points
  std::vector<unsigned int> last_round_dofs (starting_indices);
  
				   // delete disallowed elements
  for (unsigned int i=0; i<last_round_dofs.size(); ++i)
    if ((last_round_dofs[i]==DoFHandler<dim>::invalid_dof_index) ||
	(last_round_dofs[i]>=n_dofs))
      last_round_dofs[i] = DoFHandler<dim>::invalid_dof_index;
  
  std::remove_if (last_round_dofs.begin(), last_round_dofs.end(),
		  std::bind2nd(std::equal_to<unsigned int>(),
			       DoFHandler<dim>::invalid_dof_index));
  
				   // now if no valid points remain:
				   // find dof with lowest coordination
				   // number
  
  if (last_round_dofs.size() == 0)
    {
      unsigned int starting_point   = DoFHandler<dim>::invalid_dof_index;
      unsigned int min_coordination = n_dofs;
      for (unsigned int row=0; row<n_dofs; ++row) 
	{
	  unsigned int j;
	  for (j=sparsity.get_rowstart_indices()[row];
	       j<sparsity.get_rowstart_indices()[row+1]; ++j)
	    if (sparsity.get_column_numbers()[j] == SparsityPattern::invalid_entry)
	      break;
					   // post-condition after loop:
					   // coordination is now
					   // j-rowstart[row]
	  if (j-sparsity.get_rowstart_indices()[row] <  min_coordination)
	    {
	      min_coordination = j-sparsity.get_rowstart_indices()[row];
	      starting_point   = row;
	    };
	};
				       // initialize the first dof
      last_round_dofs.push_back (starting_point);
    };


				   // store next free dof index
  unsigned int next_free_number = 0;

				   // enumerate the first round dofs
  for (unsigned int i=0; i!=last_round_dofs.size(); ++i)
    new_indices[last_round_dofs[i]] = next_free_number++;

  bool all_dofs_renumbered = false;

				   // now do as many steps as needed to
				   // renumber all dofs
  while (!all_dofs_renumbered) 
    {
				       // store the indices of the dofs to be
				       // renumbered in the next round
      std::vector<unsigned int> next_round_dofs;

				       // find all neighbors of the
				       // dofs numbered in the last
				       // round
      for (unsigned int i=0; i<last_round_dofs.size(); ++i)
	for (unsigned int j=sparsity.get_rowstart_indices()[last_round_dofs[i]];
	     j<sparsity.get_rowstart_indices()[last_round_dofs[i]+1]; ++j)
	  if (sparsity.get_column_numbers()[j] == SparsityPattern::invalid_entry)
	    break;
	  else
	    next_round_dofs.push_back (sparsity.get_column_numbers()[j]);
      
				       // sort dof numbers
      std::sort (next_round_dofs.begin(), next_round_dofs.end());

				       // delete multiple entries
      std::vector<unsigned int>::iterator end_sorted;
      end_sorted = std::unique (next_round_dofs.begin(), next_round_dofs.end());
      next_round_dofs.erase (end_sorted, next_round_dofs.end());

				       // eliminate dofs which are
				       // already numbered
      for (int s=next_round_dofs.size()-1; s>=0; --s)
	if (new_indices[next_round_dofs[s]] != DoFHandler<dim>::invalid_dof_index)
	  next_round_dofs.erase (next_round_dofs.begin() + s);

				       // check whether there are any new
				       // dofs
      all_dofs_renumbered = (next_round_dofs.size() == 0);
      if (all_dofs_renumbered)
					 // end loop if possible
	continue;


				       // store for each coordination
				       // number the dofs with these
				       // coordination number
      std::multimap<unsigned int, int> dofs_by_coordination;
      
				       // find coordination number for
				       // each of these dofs
      for (std::vector<unsigned int>::iterator s=next_round_dofs.begin();
	   s!=next_round_dofs.end(); ++s) 
	{
	  unsigned int coordination = 0;
	  for (unsigned int j=sparsity.get_rowstart_indices()[*s];
	       j<sparsity.get_rowstart_indices()[*s+1]; ++j)
	    if (sparsity.get_column_numbers()[j] == SparsityPattern::invalid_entry)
	      break;
	    else
	      ++coordination;

					   // insert this dof at its
					   // coordination number
	  const std::pair<const unsigned int, int> new_entry (coordination, *s);
	  dofs_by_coordination.insert (new_entry);
	};
      
				       ////
      std::multimap<unsigned int, int>::iterator i;
      for (i = dofs_by_coordination.begin(); i!=dofs_by_coordination.end(); ++i) 
	new_indices[i->second] = next_free_number++;

				       // after that: copy this round's
				       // dofs for the next round
      last_round_dofs = next_round_dofs;
    };

#ifdef DEBUG
				   //  test for all indices numbered
//TODO: Test fails. Do not check before unification.
  if (std::find (new_indices.begin(), new_indices.end(),
		 DoFHandler<dim>::invalid_dof_index)
      !=
      new_indices.end())
    Assert (false, ExcRenumberingIncomplete());
  Assert (next_free_number == n_dofs,
	  ExcRenumberingIncomplete());
#endif

  if (reversed_numbering)
    for (std::vector<unsigned int>::iterator i=new_indices.begin(); i!=new_indices.end(); ++i)
      *i = n_dofs-*i;

				   // actually perform renumbering;
				   // this is dimension specific and
				   // thus needs an own function
  dof_handler.renumber_dofs (level, new_indices);
}


template <int dim>
void
DoFRenumbering::
component_wise (DoFHandler<dim>                 &dof_handler,
                const std::vector<unsigned int> &component_order_arg)
{
  std::vector<unsigned int> renumbering (dof_handler.n_dofs(),
					 DoFHandler<dim>::invalid_dof_index);

  typedef
    WrapDoFIterator<typename DoFHandler<dim>::active_cell_iterator> ITERATOR;

  typename DoFHandler<dim>::active_cell_iterator
    istart = dof_handler.begin_active();
  ITERATOR start = istart;
  const typename DoFHandler<dim>::cell_iterator
    end = dof_handler.end();

  unsigned int result =
  compute_component_wise<dim, ITERATOR,
    typename DoFHandler<dim>::cell_iterator>(renumbering,
					     start, end,
					     component_order_arg);

  if (result == 0) return;
  
  Assert (result == dof_handler.n_dofs(),
	  ExcRenumberingIncomplete());
  
  dof_handler.renumber_dofs (renumbering);
}



template <int dim>
void DoFRenumbering::
component_wise (MGDoFHandler<dim> &dof_handler,
                const unsigned int level,
                const std::vector<unsigned int> &component_order_arg)
{
  std::vector<unsigned int> renumbering (dof_handler.n_dofs(level),
					 DoFHandler<dim>::invalid_dof_index);

  typedef
    WrapMGDoFIterator<typename MGDoFHandler<dim>::cell_iterator> ITERATOR;
  
  typename MGDoFHandler<dim>::cell_iterator
    istart =dof_handler.begin(level);
  ITERATOR start = istart;
  typename MGDoFHandler<dim>::cell_iterator
    iend = dof_handler.end(level);
  const ITERATOR end = iend;

  const unsigned int result =
    compute_component_wise<dim, ITERATOR, ITERATOR>(
      renumbering, start, end, component_order_arg);

  if (result == 0) return;
  
  Assert (result == dof_handler.n_dofs(level),
	  ExcRenumberingIncomplete());
  
  if (renumbering.size()!=0)
    dof_handler.renumber_dofs (level, renumbering);
}



template <int dim>
void DoFRenumbering::
component_wise (MGDoFHandler<dim> &dof_handler,
                const std::vector<unsigned int> &component_order_arg)
{
  Threads::ThreadGroup<> threads;

  void (*non_mg_part) (DoFHandler<dim> &, const std::vector<unsigned int> &)
    = &DoFRenumbering::component_wise<dim>;
  void (*mg_part) (MGDoFHandler<dim> &, unsigned int, const std::vector<unsigned int> &)
    = &DoFRenumbering::component_wise<dim>;
  
  threads += Threads::spawn (non_mg_part) (static_cast<DoFHandler<dim>&> (dof_handler),
					   component_order_arg);
  for (unsigned int level=0; level<dof_handler.get_tria().n_levels(); ++level)
    threads += Threads::spawn (mg_part) (dof_handler, level, component_order_arg);

  threads.join_all();
}




template <int dim, class ITERATOR, class ENDITERATOR>
unsigned int
DoFRenumbering::
compute_component_wise (std::vector<unsigned int>& new_indices,
                        ITERATOR& start,
                        const ENDITERATOR& end,
                        const std::vector<unsigned int> &component_order_arg)
{
//TODO: Modify for hp
  const FiniteElement<dim>& fe = start->get_fe();
  const unsigned int dofs_per_cell = fe.dofs_per_cell;

//    Assert (new_indices.size() ==  start->dof_handler().n_dofs(),
//  	  ExcDimensionMismatch(new_indices.size(),
//  			       start->dof_handler.n_dofs()));

				   // do nothing if the FE has only
				   // one component
  if (fe.n_components() == 1)
    {
      new_indices.resize(0);
      return 0;
    }

				   // Copy last argument into a
				   // writable vector.
  std::vector<unsigned int> component_order (component_order_arg);
				   // If the last argument was an
				   // empty vector, set up things to
				   // store components in the order
				   // found in the system.
  if (component_order.size() == 0)
    for (unsigned int i=0; i<fe.n_components(); ++i)
      component_order.push_back (i);

  Assert (component_order.size() == fe.n_components(),
	  ExcDimensionMismatch(component_order.size(), fe.n_components()));
  
  for (unsigned int i=0; i< component_order.size(); ++i)
    Assert(component_order[i] < fe.n_components(),
	   ExcIndexRange(component_order[i], 0, fe.n_components()));
  
				   // vector to hold the dof indices on
				   // the cell we visit at a time
  std::vector<unsigned int> local_dof_indices(dofs_per_cell);

                                   // prebuilt list to which component
				   // a given dof on a cell
				   // should go. note that we get into
				   // trouble here if the shape
				   // function is not primitive, since
				   // then there is no single vector
				   // component to which it
				   // belongs. in this case, assign it
				   // to the first vector component to
				   // which it belongs
  std::vector<unsigned int> component_list (dofs_per_cell);
  for (unsigned int i=0; i<component_list.size(); ++i)
    if (fe.is_primitive(i))
      component_list[i]
	= component_order[fe.system_to_component_index(i).first];
    else
      {
        const unsigned int base_element =
          fe.system_to_base_index(i).first.first;
        const unsigned int base_multiplicity =
          fe.system_to_base_index(i).first.second;
                                         // sum up the number of
                                         // components all the
                                         // elements before that have
        unsigned int c=0;
        for (unsigned int b=0; b<base_element; ++b)
          c += fe.base_element(b).n_components() *
               fe.element_multiplicity(b);
        for (unsigned int m=0; m<base_multiplicity; ++m)
          c += fe.base_element(base_element).n_components();
                                         // then associate this degree
                                         // of freedom with this
                                         // component
        component_list[i] = component_order[c];
      };
  
				   // set up a map where for each
				   // component the respective degrees
				   // of freedom are collected.
				   //
				   // note that this map is sorted by
				   // component but that within each
				   // component it is NOT sorted by
				   // dof index. note also that some
				   // dof indices are entered
				   // multiply, so we will have to
				   // take care of that
  std::vector<std::vector<unsigned int> >
    component_to_dof_map (fe.n_components());
  for (;start!=end;++start)
    {
				       // on each cell: get dof indices
				       // and insert them into the global
				       // list using their component
      start.get_dof_indices (local_dof_indices);
      for (unsigned int i=0; i<dofs_per_cell; ++i)
	component_to_dof_map[component_list[i]].push_back (local_dof_indices[i]);
    };
  
				   // now we've got all indices sorted
				   // into buckets labelled with their
				   // target component number. we've
				   // only got to traverse this list
				   // and assign the new indices
				   //
				   // however, we first want to sort
				   // the indices entered into the
				   // buckets to preserve the order
				   // within each component and during
				   // this also remove duplicate
				   // entries
				   //
				   // note that we no longer have to
				   // care about non-primitive shape
				   // functions since the buckets
				   // corresponding to the second and
				   // following vector components of a
				   // non-primitive FE will simply be
				   // empty, everything being shoved
				   // into the first one. The same
				   // holds if several components were
				   // joined into a single target.
  for (unsigned int component=0; component<fe.n_components(); ++component)
    {
      std::sort (component_to_dof_map[component].begin(),
		 component_to_dof_map[component].end());
      component_to_dof_map[component]
        .erase (std::unique (component_to_dof_map[component].begin(),
                             component_to_dof_map[component].end()),
                component_to_dof_map[component].end());
    };

                                   // now concatenate all the
                                   // components in the order the user
                                   // desired to see
  unsigned int next_free_index = 0;
  for (unsigned int c=0; c<fe.n_components(); ++c)
    {
      const unsigned int component = c;
      
      const typename std::vector<unsigned int>::const_iterator
	begin_of_component = component_to_dof_map[component].begin(),
	end_of_component   = component_to_dof_map[component].end();
            
      for (typename std::vector<unsigned int>::const_iterator
             dof_index = begin_of_component;
	   dof_index != end_of_component; ++dof_index)
	new_indices[*dof_index] = next_free_index++;
    };

  return next_free_index;
}



template <class DH>
void
DoFRenumbering::sort_selected_dofs_back (
  DH&                      dof_handler,
  const std::vector<bool>& selected_dofs)
{
  std::vector<unsigned int> renumbering(dof_handler.n_dofs(),
					DH::invalid_dof_index);
  compute_sort_selected_dofs_back(renumbering, dof_handler, selected_dofs);

  dof_handler.renumber_dofs(renumbering);
}



template <class DH>
void
DoFRenumbering::compute_sort_selected_dofs_back (
  std::vector<unsigned int>& new_indices,
  const DH&                  dof_handler,
  const std::vector<bool>&   selected_dofs)
{
  const unsigned int n_dofs = dof_handler.n_dofs();
  Assert (selected_dofs.size() == n_dofs,
	  ExcDimensionMismatch (selected_dofs.size(), n_dofs));

				   // re-sort the dofs according to
				   // their selection state
  Assert (new_indices.size() == n_dofs,
	  ExcDimensionMismatch(new_indices.size(), n_dofs));
  
  const unsigned int   n_selected_dofs = std::count (selected_dofs.begin(),
						     selected_dofs.end(),
						     false);
  
  unsigned int next_unselected = 0;
  unsigned int next_selected   = n_selected_dofs;
  for (unsigned int i=0; i<n_dofs; ++i)
    if (selected_dofs[i] == false)
      {
	new_indices[i] = next_unselected;
	++next_unselected;
      }
    else
      {
	new_indices[i] = next_selected;
	++next_selected;
      };
  Assert (next_unselected == n_selected_dofs, ExcInternalError());
  Assert (next_selected == n_dofs, ExcInternalError());
}



template <class DH>
void
DoFRenumbering::cell_wise_dg (
  DH& dof,
  const std::vector<typename DH::cell_iterator>& cells)
{
  std::vector<unsigned int> renumbering(dof.n_dofs());
  std::vector<unsigned int> reverse(dof.n_dofs());
  compute_cell_wise_dg(renumbering, reverse, dof, cells);
  
  dof.renumber_dofs(renumbering);
}


template <class DH>
void
DoFRenumbering::cell_wise (
  DH& dof,
  const std::vector<typename DH::cell_iterator>& cells)
{
  std::vector<unsigned int> renumbering(dof.n_dofs());
  std::vector<unsigned int> reverse(dof.n_dofs());
  compute_cell_wise(renumbering, reverse, dof, cells);
  
  dof.renumber_dofs(renumbering);
}


//TODO: Discuss if we cannot replace this function by the next
template <class DH>
void
DoFRenumbering::compute_cell_wise_dg (
  std::vector<unsigned int>& new_indices,
  std::vector<unsigned int>& reverse,
  const DH& dof,
  const typename std::vector<typename DH::cell_iterator>& cells)
{
  Assert(cells.size() == dof.get_tria().n_active_cells(),
	 ExcDimensionMismatch(cells.size(),
			      dof.get_tria().n_active_cells()));

  unsigned int n_global_dofs = dof.n_dofs();

  // Actually, we compute the inverse of the reordering vector, called reverse here.
  // Later, irs inverse is computed into new_indices, which is the return argument.
  
  Assert(new_indices.size() == n_global_dofs,
	 ExcDimensionMismatch(new_indices.size(), n_global_dofs));
  Assert(reverse.size() == n_global_dofs,
	 ExcDimensionMismatch(reverse.size(), n_global_dofs));
  
  std::vector<unsigned int> cell_dofs;

  unsigned int global_index = 0;
  
  typename std::vector<typename DH::cell_iterator>::const_iterator cell;

  for(cell = cells.begin(); cell != cells.end(); ++cell)
    {
      Assert((*cell)->get_fe().n_dofs_per_face()==0, ExcNotDGFEM());
				       // Determine the number of dofs
				       // on this cell and reinit the
				       // vector storing these
				       // numbers.
      unsigned int n_cell_dofs = (*cell)->get_fe().n_dofs_per_cell();
      cell_dofs.resize(n_cell_dofs);
      
      (*cell)->get_dof_indices(cell_dofs);

				       // Sort here to make sure that
				       // degrees of freedom inside a
				       // single cell are in the same
				       // order after renumbering.
      std::sort(cell_dofs.begin(), cell_dofs.end());

      for (unsigned int i=0;i<n_cell_dofs;++i)
	{
	  reverse[global_index++] = cell_dofs[i];
	}
    }
  Assert(global_index == n_global_dofs, ExcRenumberingIncomplete());

  for (unsigned int i=0;i<reverse.size(); ++i)
    new_indices[reverse[i]] = i;
}


template <class DH>
void
DoFRenumbering::compute_cell_wise (
  std::vector<unsigned int>& new_indices,
  std::vector<unsigned int>& reverse,
  const DH& dof,
  const typename std::vector<typename DH::cell_iterator>& cells)
{
  Assert(cells.size() == dof.get_tria().n_active_cells(),
	 ExcDimensionMismatch(cells.size(),
			      dof.get_tria().n_active_cells()));

  unsigned int n_global_dofs = dof.n_dofs();
  
  // Actually, we compute the inverse of the reordering vector, called reverse here.
  // Later, irs inverse is computed into new_indices, which is the return argument.
  
  Assert(new_indices.size() == n_global_dofs,
	 ExcDimensionMismatch(new_indices.size(), n_global_dofs));
  Assert(reverse.size() == n_global_dofs,
	 ExcDimensionMismatch(reverse.size(), n_global_dofs));

				   // For continuous elements, we must
				   // make sure, that each dof is
				   // reordered only once.
  std::vector<bool> already_sorted(n_global_dofs, false);
  std::vector<unsigned int> cell_dofs;

  unsigned int global_index = 0;
  
  typename std::vector<typename DH::cell_iterator>::const_iterator cell;

  for(cell = cells.begin(); cell != cells.end(); ++cell)
    {
				       // Determine the number of dofs
				       // on this cell and reinit the
				       // vector storing these
				       // numbers.
      unsigned int n_cell_dofs = (*cell)->get_fe().n_dofs_per_cell();
      cell_dofs.resize(n_cell_dofs);
      
      (*cell)->get_dof_indices(cell_dofs);

				       // Sort here to make sure that
				       // degrees of freedom inside a
				       // single cell are in the same
				       // order after renumbering.
      std::sort(cell_dofs.begin(), cell_dofs.end());

      for (unsigned int i=0;i<n_cell_dofs;++i)
	{
	  if (!already_sorted[cell_dofs[i]])
	    {
	      already_sorted[cell_dofs[i]] = true;
	      reverse[global_index++] = cell_dofs[i];
	    }
	}
    }
  Assert(global_index == n_global_dofs, ExcRenumberingIncomplete());

  for (unsigned int i=0;i<reverse.size(); ++i)
    new_indices[reverse[i]] = i;
}


template <int dim>
void DoFRenumbering::cell_wise_dg (
  MGDoFHandler<dim>& dof,
  const unsigned int level,
  const typename std::vector<typename MGDoFHandler<dim>::cell_iterator>& cells)
{
  std::vector<unsigned int> renumbering(dof.n_dofs(level));
  std::vector<unsigned int> reverse(dof.n_dofs(level));
  
  compute_cell_wise_dg(renumbering, reverse, dof, level, cells);
  dof.renumber_dofs(level, renumbering);
}


template <int dim>
void DoFRenumbering::cell_wise (
  MGDoFHandler<dim>& dof,
  const unsigned int level,
  const typename std::vector<typename MGDoFHandler<dim>::cell_iterator>& cells)
{
  std::vector<unsigned int> renumbering(dof.n_dofs(level));
  std::vector<unsigned int> reverse(dof.n_dofs(level));
  
  compute_cell_wise(renumbering, reverse, dof, level, cells);
  dof.renumber_dofs(level, renumbering);
}


template <int dim>
void DoFRenumbering::compute_cell_wise_dg (
  std::vector<unsigned int>& new_order,
  std::vector<unsigned int>& reverse,
  const MGDoFHandler<dim>& dof,
  const unsigned int level,
  const typename std::vector<typename MGDoFHandler<dim>::cell_iterator>& cells)
{
  Assert(cells.size() == dof.get_tria().n_cells(level),
	 ExcDimensionMismatch(cells.size(),
			      dof.get_tria().n_cells(level)));
  switch (dim)
    {
      case 3:
	    Assert(dof.get_fe().n_dofs_per_quad()==0,
		   ExcNotDGFEM());
      case 2:
	    Assert(dof.get_fe().n_dofs_per_line()==0,
		   ExcNotDGFEM());
      default:
	    Assert(dof.get_fe().n_dofs_per_vertex()==0,
		   ExcNotDGFEM());
    }
  
  Assert (new_order.size() == dof.n_dofs(level),
	  ExcDimensionMismatch(new_order.size(), dof.n_dofs(level)));
  Assert (reverse.size() == dof.n_dofs(level),
	  ExcDimensionMismatch(reverse.size(), dof.n_dofs(level)));
  
  unsigned int n_global_dofs = dof.n_dofs(level);
  unsigned int n_cell_dofs = dof.get_fe().n_dofs_per_cell();
  
  std::vector<unsigned int> cell_dofs(n_cell_dofs);
  
  unsigned int global_index = 0;
  
  typename std::vector<typename MGDoFHandler<dim>::cell_iterator>::const_iterator cell;

  for(cell = cells.begin(); cell != cells.end(); ++cell)
    {
      Assert ((*cell)->level() == (int) level, ExcInternalError());

      (*cell)->get_mg_dof_indices(cell_dofs);
      std::sort(cell_dofs.begin(), cell_dofs.end());

      for (unsigned int i=0;i<n_cell_dofs;++i)
	{
	  reverse[global_index++] = cell_dofs[i];
	}
    }
  Assert(global_index == n_global_dofs, ExcRenumberingIncomplete());

  for (unsigned int i=0;i<new_order.size(); ++i)
    new_order[reverse[i]] = i;  
}



template <int dim>
void DoFRenumbering::compute_cell_wise (
  std::vector<unsigned int>& new_order,
  std::vector<unsigned int>& reverse,
  const MGDoFHandler<dim>& dof,
  const unsigned int level,
  const typename std::vector<typename MGDoFHandler<dim>::cell_iterator>& cells)
{
  Assert(cells.size() == dof.get_tria().n_cells(level),
	 ExcDimensionMismatch(cells.size(),
			      dof.get_tria().n_cells(level)));
  Assert (new_order.size() == dof.n_dofs(level),
	  ExcDimensionMismatch(new_order.size(), dof.n_dofs(level)));
  Assert (reverse.size() == dof.n_dofs(level),
	  ExcDimensionMismatch(reverse.size(), dof.n_dofs(level)));
  
  unsigned int n_global_dofs = dof.n_dofs(level);
  unsigned int n_cell_dofs = dof.get_fe().n_dofs_per_cell();
  
  std::vector<bool> already_sorted(n_global_dofs, false);
  std::vector<unsigned int> cell_dofs(n_cell_dofs);
  
  unsigned int global_index = 0;
  
  typename std::vector<typename MGDoFHandler<dim>::cell_iterator>::const_iterator cell;

  for(cell = cells.begin(); cell != cells.end(); ++cell)
    {
      Assert ((*cell)->level() == (int) level, ExcInternalError());

      (*cell)->get_mg_dof_indices(cell_dofs);
      std::sort(cell_dofs.begin(), cell_dofs.end());
      
      for (unsigned int i=0;i<n_cell_dofs;++i)
	{
	  if (!already_sorted[cell_dofs[i]])
	    {
	      already_sorted[cell_dofs[i]] = true;
	      reverse[global_index++] = cell_dofs[i];
	    }
	}
    }
  Assert(global_index == n_global_dofs, ExcRenumberingIncomplete());

  for (unsigned int i=0;i<new_order.size(); ++i)
    new_order[reverse[i]] = i;  
}



template <class DH, int dim>
void
DoFRenumbering::downstream_dg (DH& dof, const Point<dim>& direction)
{
  std::vector<unsigned int> renumbering(dof.n_dofs());
  compute_downstream_dg(renumbering, dof, direction);

  dof.renumber_dofs(renumbering);
}



template <class DH, int dim>
void
DoFRenumbering::downstream (DH& dof, const Point<dim>& direction)
{
  std::vector<unsigned int> renumbering(dof.n_dofs());
  std::vector<unsigned int> reverse(dof.n_dofs());
  compute_downstream(renumbering, reverse, dof, direction);

  dof.renumber_dofs(renumbering);
}



template <class DH, int dim>
void
DoFRenumbering::compute_downstream_dg (
  std::vector<unsigned int>& new_indices,
  const DH& dof,
  const Point<dim>& direction)
{
  std::vector<typename DH::cell_iterator>
    ordered_cells(dof.get_tria().n_active_cells());
  const CompareDownstream<dim> comparator(direction);
  
  typename DH::active_cell_iterator begin = dof.begin_active();
  typename DH::active_cell_iterator end = dof.end();
  
  copy (begin, end, ordered_cells.begin());
  std::sort (ordered_cells.begin(), ordered_cells.end(), comparator);

  std::vector<unsigned int> reverse(new_indices.size());
  compute_cell_wise_dg(new_indices, reverse, dof, ordered_cells);
}


template <class DH, int dim>
void
DoFRenumbering::compute_downstream_dg (
  std::vector<unsigned int>& new_indices,
  std::vector<unsigned int>& reverse,
  const DH& dof,
  const Point<dim>& direction)
{
  std::vector<typename DH::cell_iterator>
    ordered_cells(dof.get_tria().n_active_cells());
  const CompareDownstream<dim> comparator(direction);
  
  typename DH::active_cell_iterator begin = dof.begin_active();
  typename DH::active_cell_iterator end = dof.end();
  
  copy (begin, end, ordered_cells.begin());
  std::sort (ordered_cells.begin(), ordered_cells.end(), comparator);
  
  compute_cell_wise_dg(new_indices, reverse, dof, ordered_cells);
}


template <class DH, int dim>
void
DoFRenumbering::compute_downstream (
  std::vector<unsigned int>& new_indices,
  std::vector<unsigned int>& reverse,
  const DH& dof,
  const Point<dim>& direction)
{
  std::vector<typename DH::cell_iterator>
    ordered_cells(dof.get_tria().n_active_cells());
  const CompareDownstream<dim> comparator(direction);
  
  typename DH::active_cell_iterator begin = dof.begin_active();
  typename DH::active_cell_iterator end = dof.end();
  
  copy (begin, end, ordered_cells.begin());
  std::sort (ordered_cells.begin(), ordered_cells.end(), comparator);
  
  compute_cell_wise(new_indices, reverse, dof, ordered_cells);
}


template <int dim>
void DoFRenumbering::downstream_dg (MGDoFHandler<dim>& dof,
				    const unsigned int level,
				    const Point<dim>& direction)
{
  std::vector<unsigned int> renumbering(dof.n_dofs(level));
  std::vector<unsigned int> reverse(dof.n_dofs(level));
  compute_downstream_dg(renumbering, reverse, dof, level, direction);
  
  dof.renumber_dofs(level, renumbering);
}


template <int dim>
void DoFRenumbering::downstream (MGDoFHandler<dim>& dof,
				 const unsigned int level,
				 const Point<dim>& direction)
{
  std::vector<unsigned int> renumbering(dof.n_dofs(level));
  std::vector<unsigned int> reverse(dof.n_dofs(level));
  compute_downstream(renumbering, reverse, dof, level, direction);
  
  dof.renumber_dofs(level, renumbering);
}


template <int dim>
void
DoFRenumbering::compute_downstream_dg (
  std::vector<unsigned int>& new_indices,
  std::vector<unsigned int>& reverse,
  const MGDoFHandler<dim>& dof,
  const unsigned int level,
  const Point<dim>& direction)
{
  std::vector<typename MGDoFHandler<dim>::cell_iterator>
    ordered_cells(dof.get_tria().n_cells(level));
  const CompareDownstream<dim> comparator(direction);
  
  typename MGDoFHandler<dim>::cell_iterator begin = dof.begin(level);
  typename MGDoFHandler<dim>::cell_iterator end = dof.end(level);
  
  std::copy (begin, end, ordered_cells.begin());
  std::sort (ordered_cells.begin(), ordered_cells.end(), comparator);
  
  compute_cell_wise_dg(new_indices, reverse, dof, level, ordered_cells);
}


template <int dim>
void
DoFRenumbering::compute_downstream (
  std::vector<unsigned int>& new_indices,
  std::vector<unsigned int>& reverse,
  const MGDoFHandler<dim>& dof,
  const unsigned int level,
  const Point<dim>& direction)
{
  std::vector<typename MGDoFHandler<dim>::cell_iterator>
    ordered_cells(dof.get_tria().n_cells(level));
  const CompareDownstream<dim> comparator(direction);
  
  typename MGDoFHandler<dim>::cell_iterator begin = dof.begin(level);
  typename MGDoFHandler<dim>::cell_iterator end = dof.end(level);
  
  std::copy (begin, end, ordered_cells.begin());
  std::sort (ordered_cells.begin(), ordered_cells.end(), comparator);
  
  compute_cell_wise(new_indices, reverse, dof, level, ordered_cells);
}



/**
 * Provide comparator for DoFCellAccessors
 */

template <int dim>
struct ClockCells
{
				     /**
				      * Center of rotation.
				      */
    const Point<dim>& center;
				     /**
				      * Revert sorting order.
				      */
    bool counter;
    
				     /**
				      * Constructor.
				      */
    ClockCells (const Point<dim>& center, bool counter) :
		    center(center),
		    counter(counter)
      {}
				     /**
				      * Comparison operator
				      */
    template <class DHCellIterator>
    bool operator () (const DHCellIterator& c1,
		      const DHCellIterator& c2) const
      {
	
	const Point<dim> v1 = c1->center() - center;
	const Point<dim> v2 = c2->center() - center;
	const double s1 = std::atan2(v1(0), v1(1));
	const double s2 = std::atan2(v2(0), v2(1));
	return ( counter ? (s1>s2) : (s2>s1));
      }
};


template <class DH, int dim>
void
DoFRenumbering::clockwise_dg (
  DH& dof,
  const Point<dim>& center,
  const bool counter)
{
  std::vector<unsigned int> renumbering(dof.n_dofs());
  compute_clockwise_dg(renumbering, dof, center, counter);

  dof.renumber_dofs(renumbering);
}



template <class DH, int dim>
void
DoFRenumbering::compute_clockwise_dg (
  std::vector<unsigned int>& new_indices,
  const DH& dof,
  const Point<dim>& center,
  const bool counter)
{
  std::vector<typename DH::cell_iterator>
    ordered_cells(dof.get_tria().n_active_cells());
  ClockCells<dim> comparator(center, counter);
  
  typename DH::active_cell_iterator begin = dof.begin_active();
  typename DH::active_cell_iterator end = dof.end();
  
  std::copy (begin, end, ordered_cells.begin());
  std::sort (ordered_cells.begin(), ordered_cells.end(), comparator);

  std::vector<unsigned int> reverse(new_indices.size());
  compute_cell_wise_dg(new_indices, reverse, dof, ordered_cells);
}


template <int dim>
void DoFRenumbering::clockwise_dg (MGDoFHandler<dim>& dof,
				   const unsigned int level,
				   const Point<dim>& center,
				   const bool counter)
{
  std::vector<typename MGDoFHandler<dim>::cell_iterator>
    ordered_cells(dof.get_tria().n_cells(level));
  ClockCells<dim> comparator(center, counter);
  
  typename MGDoFHandler<dim>::cell_iterator begin = dof.begin(level);
  typename MGDoFHandler<dim>::cell_iterator end = dof.end(level);
  
  std::copy (begin, end, ordered_cells.begin());
  std::sort (ordered_cells.begin(), ordered_cells.end(), comparator);

  cell_wise_dg(dof, level, ordered_cells);
}



template <class DH>
void
DoFRenumbering::random (DH& dof_handler)
{
  std::vector<unsigned int> renumbering(dof_handler.n_dofs(),
					DH::invalid_dof_index);
  compute_random(renumbering, dof_handler);

  dof_handler.renumber_dofs(renumbering);
}


template <class DH>
void
DoFRenumbering::compute_random (
  std::vector<unsigned int>& new_indices,
  const DH&                  dof_handler)
{
  const unsigned int n_dofs = dof_handler.n_dofs();
  Assert(new_indices.size() == n_dofs,
	 ExcDimensionMismatch(new_indices.size(), n_dofs));
  
  for (unsigned i=0; i<n_dofs; ++i)
    new_indices[i] = i;
  
  std::random_shuffle (new_indices.begin(), new_indices.end());
}



template <int dim>
void
DoFRenumbering::subdomain_wise (DoFHandler<dim> &dof_handler)
{
  std::vector<unsigned int> renumbering(dof_handler.n_dofs(),
					DoFHandler<dim>::invalid_dof_index);
  compute_subdomain_wise(renumbering, dof_handler);

  dof_handler.renumber_dofs(renumbering);
}


template <int dim>
void
DoFRenumbering::
compute_subdomain_wise (std::vector<unsigned int> &new_dof_indices,
                        const DoFHandler<dim>     &dof_handler)
{
  const unsigned int n_dofs = dof_handler.n_dofs();
  Assert (new_dof_indices.size() == n_dofs,
	  ExcDimensionMismatch (new_dof_indices.size(), n_dofs));

                                   // first get the association of each dof
                                   // with a subdomain and determine the total
                                   // number of subdomain ids used
  std::vector<unsigned int> subdomain_association (n_dofs);
  DoFTools::get_subdomain_association (dof_handler,
                                       subdomain_association);
  const unsigned int n_subdomains
    = *std::max_element (subdomain_association.begin(),
                         subdomain_association.end()) + 1;
  
                                   // then renumber the subdomains by first
                                   // looking at those belonging to subdomain
                                   // 0, then those of subdomain 1, etc. note
                                   // that the algorithm is stable, i.e. if
                                   // two dofs i,j have i<j and belong to the
                                   // same subdomain, then they will be in
                                   // this order also after reordering
  std::fill (new_dof_indices.begin(), new_dof_indices.end(),
             numbers::invalid_unsigned_int);
  unsigned int next_free_index = 0;
  for (unsigned int subdomain=0; subdomain<n_subdomains; ++subdomain)
    for (unsigned int i=0; i<n_dofs; ++i)
      if (subdomain_association[i] == subdomain)
        {
          Assert (new_dof_indices[i] == numbers::invalid_unsigned_int,
                  ExcInternalError());
          new_dof_indices[i] = next_free_index;
          ++next_free_index;
        }

                                   // we should have numbered all dofs
  Assert (next_free_index == n_dofs, ExcInternalError());
  Assert (std::find (new_dof_indices.begin(), new_dof_indices.end(),
                     numbers::invalid_unsigned_int)
          == new_dof_indices.end(),
          ExcInternalError());
}




// explicit instantiations
template void DoFRenumbering::boost::Cuthill_McKee (DoFHandler<deal_II_dimension> &, bool, bool);


template
void DoFRenumbering::Cuthill_McKee<DoFHandler<deal_II_dimension> >
(DoFHandler<deal_II_dimension>&,
 const bool,
 const bool,
 const std::vector<unsigned int>&);

template
void
DoFRenumbering::compute_Cuthill_McKee<DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&,
 const DoFHandler<deal_II_dimension>&,
 const bool,
 const bool,
 const std::vector<unsigned int>&);


template
void DoFRenumbering::Cuthill_McKee<hp::DoFHandler<deal_II_dimension> >
(hp::DoFHandler<deal_II_dimension>&,
 const bool,
 const bool,
 const std::vector<unsigned int>&);

template
void
DoFRenumbering::compute_Cuthill_McKee<hp::DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&,
 const hp::DoFHandler<deal_II_dimension>&,
 const bool,
 const bool,
 const std::vector<unsigned int>&);


template
void DoFRenumbering::component_wise<deal_II_dimension>
(DoFHandler<deal_II_dimension>&,
 const std::vector<unsigned int>&);

//  template
//  void
//  DoFRenumbering::compute_component_wise<deal_II_dimension>
//  (std::vector<unsigned int>&,
//   const DoFHandler<deal_II_dimension>&,
//   const std::vector<unsigned int>&);

template
void DoFRenumbering::component_wise<deal_II_dimension>
(MGDoFHandler<deal_II_dimension>&,
 unsigned int,
 const std::vector<unsigned int>&);

template
void DoFRenumbering::component_wise<deal_II_dimension>
(MGDoFHandler<deal_II_dimension>&,
 const std::vector<unsigned int>&);

// DG renumbering for DoFHandler

template
void
DoFRenumbering::cell_wise_dg<DoFHandler<deal_II_dimension> >
(DoFHandler<deal_II_dimension>&,
 const std::vector<DoFHandler<deal_II_dimension>::cell_iterator>&);

template
void
DoFRenumbering::compute_cell_wise_dg<DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&, std::vector<unsigned int>&,
 const DoFHandler<deal_II_dimension>&,
 const std::vector<DoFHandler<deal_II_dimension>::cell_iterator>&);

template
void
DoFRenumbering::downstream_dg<DoFHandler<deal_II_dimension> >
(DoFHandler<deal_II_dimension>&,
 const Point<deal_II_dimension>&);

template
void
DoFRenumbering::compute_downstream_dg<DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&,std::vector<unsigned int>&,
 const DoFHandler<deal_II_dimension>&,
 const Point<deal_II_dimension>&);
template
void
DoFRenumbering::compute_downstream_dg<DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&,
 const DoFHandler<deal_II_dimension>&,
 const Point<deal_II_dimension>&);

template void
DoFRenumbering::cell_wise<DoFHandler<deal_II_dimension> >
(DoFHandler<deal_II_dimension>&,
 const std::vector<DoFHandler<deal_II_dimension>::cell_iterator>&);

template void
DoFRenumbering::compute_cell_wise<DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&, std::vector<unsigned int>&,
 const DoFHandler<deal_II_dimension>&,
 const std::vector<DoFHandler<deal_II_dimension>::cell_iterator>&);

template void
DoFRenumbering::downstream<DoFHandler<deal_II_dimension> >
(DoFHandler<deal_II_dimension>&, const Point<deal_II_dimension>&);

template void
DoFRenumbering::compute_downstream<DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&,std::vector<unsigned int>&,
 const DoFHandler<deal_II_dimension>&, const Point<deal_II_dimension>&);

template
void
DoFRenumbering::clockwise_dg<DoFHandler<deal_II_dimension> >
(DoFHandler<deal_II_dimension>&, const Point<deal_II_dimension>&, bool);

template
void
DoFRenumbering::compute_clockwise_dg<DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&, const DoFHandler<deal_II_dimension>&,
 const Point<deal_II_dimension>&, const bool);

// Renumbering for hp::DoFHandler

template void
DoFRenumbering::cell_wise_dg<hp::DoFHandler<deal_II_dimension> >
(hp::DoFHandler<deal_II_dimension>&,
 const std::vector<hp::DoFHandler<deal_II_dimension>::cell_iterator>&);

template void
DoFRenumbering::compute_cell_wise_dg<hp::DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&, std::vector<unsigned int>&,
 const hp::DoFHandler<deal_II_dimension>&,
 const std::vector<hp::DoFHandler<deal_II_dimension>::cell_iterator>&);

template void
DoFRenumbering::cell_wise<hp::DoFHandler<deal_II_dimension> >
(hp::DoFHandler<deal_II_dimension>&,
 const std::vector<hp::DoFHandler<deal_II_dimension>::cell_iterator>&);

template void
DoFRenumbering::compute_cell_wise<hp::DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&, std::vector<unsigned int>&,
 const hp::DoFHandler<deal_II_dimension>&,
 const std::vector<hp::DoFHandler<deal_II_dimension>::cell_iterator>&);

template void
DoFRenumbering::downstream_dg<hp::DoFHandler<deal_II_dimension> >
(hp::DoFHandler<deal_II_dimension>&,
 const Point<deal_II_dimension>&);

template void
DoFRenumbering::compute_downstream_dg<hp::DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&,std::vector<unsigned int>&,
 const hp::DoFHandler<deal_II_dimension>&,
 const Point<deal_II_dimension>&);
template
void
DoFRenumbering::compute_downstream_dg<hp::DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&,
 const hp::DoFHandler<deal_II_dimension>&,
 const Point<deal_II_dimension>&);

template void
DoFRenumbering::downstream<hp::DoFHandler<deal_II_dimension> >
(hp::DoFHandler<deal_II_dimension>&,
 const Point<deal_II_dimension>&);

template void
DoFRenumbering::compute_downstream<hp::DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&,std::vector<unsigned int>&,
 const hp::DoFHandler<deal_II_dimension>&,
 const Point<deal_II_dimension>&);

template
void
DoFRenumbering::clockwise_dg<hp::DoFHandler<deal_II_dimension> >
(hp::DoFHandler<deal_II_dimension>&,
 const Point<deal_II_dimension>&, bool);

template
void
DoFRenumbering::compute_clockwise_dg<hp::DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&,
 const hp::DoFHandler<deal_II_dimension>&,
 const Point<deal_II_dimension>&,
 const bool);

// MG

template
void DoFRenumbering::downstream_dg<deal_II_dimension>
(MGDoFHandler<deal_II_dimension>&, const unsigned int,
 const Point<deal_II_dimension>&);

template
void DoFRenumbering::downstream<deal_II_dimension>
(MGDoFHandler<deal_II_dimension>&, const unsigned int,
 const Point<deal_II_dimension>&);

template
void DoFRenumbering::clockwise_dg<deal_II_dimension>
(MGDoFHandler<deal_II_dimension>&,
 const unsigned int,
 const Point<deal_II_dimension>&, bool);

// Generic numbering schemes

template
void DoFRenumbering::random<DoFHandler<deal_II_dimension> >
(DoFHandler<deal_II_dimension> &);

template
void
DoFRenumbering::compute_random<DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&,
 const DoFHandler<deal_II_dimension> &);

template
void DoFRenumbering::sort_selected_dofs_back<DoFHandler<deal_II_dimension> >
(DoFHandler<deal_II_dimension> &,
 const std::vector<bool> &);

template
void
DoFRenumbering::compute_sort_selected_dofs_back<DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&,
 const DoFHandler<deal_II_dimension> &,
 const std::vector<bool> &);

template
void DoFRenumbering::random<hp::DoFHandler<deal_II_dimension> >
(hp::DoFHandler<deal_II_dimension> &);

template
void
DoFRenumbering::compute_random<hp::DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&,
 const hp::DoFHandler<deal_II_dimension> &);

template
void DoFRenumbering::sort_selected_dofs_back<hp::DoFHandler<deal_II_dimension> >
(hp::DoFHandler<deal_II_dimension> &,
 const std::vector<bool> &);

template
void
DoFRenumbering::compute_sort_selected_dofs_back<hp::DoFHandler<deal_II_dimension> >
(std::vector<unsigned int>&,
 const hp::DoFHandler<deal_II_dimension> &,
 const std::vector<bool> &);

template
void DoFRenumbering::subdomain_wise<deal_II_dimension>
(DoFHandler<deal_II_dimension> &);

template
void
DoFRenumbering::compute_subdomain_wise<deal_II_dimension>
(std::vector<unsigned int>&,
 const DoFHandler<deal_II_dimension> &);

template
void DoFRenumbering::Cuthill_McKee<deal_II_dimension>
(MGDoFHandler<deal_II_dimension>&,
 const unsigned int,
 const bool,
 const std::vector<unsigned int>&);

template
void DoFRenumbering::cell_wise_dg<deal_II_dimension>
(MGDoFHandler<deal_II_dimension>&,
 const unsigned int,
 const std::vector<MGDoFHandler<deal_II_dimension>::cell_iterator>&);
template
void DoFRenumbering::compute_cell_wise_dg<deal_II_dimension>
(std::vector<unsigned int>&, std::vector<unsigned int>&,
 const MGDoFHandler<deal_II_dimension>&, const unsigned int,
 const std::vector<MGDoFHandler<deal_II_dimension>::cell_iterator>&);
template
void DoFRenumbering::compute_downstream_dg<deal_II_dimension>
(std::vector<unsigned int>&, std::vector<unsigned int>&,
 const MGDoFHandler<deal_II_dimension>&, const unsigned int,
 const Point<deal_II_dimension>&);

DEAL_II_NAMESPACE_CLOSE
