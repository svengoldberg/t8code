/*
  This file is part of t8code.
  t8code is a C library to manage a collection (a forest) of multiple
  connected adaptive space-trees of general element classes in parallel.

  Copyright (C) 2015 the developers

  t8code is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  t8code is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with t8code; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

#include <t8_forest/t8_forest_ghost.h>
#include <t8_forest/t8_forest_types.h>
#include <t8_forest/t8_forest_private.h>
#include <t8_forest.h>
#include <t8_cmesh/t8_cmesh_trees.h>
#include <t8_element_cxx.hxx>

/* We want to export the whole implementation to be callable from "C" */
T8_EXTERN_C_BEGIN ();

/* The information for a remote process, what data
 * we have to send to them.
 */
typedef struct
{
  int                 recv_rank;        /* The rank to which we send. */
  size_t              num_bytes;        /* The number of bytes that we send. */
  sc_MPI_Request     *request;  /* Commuication request, not owned by this struct. */
  char               *buffer;   /* The send buffer. */
} t8_ghost_mpi_send_info_t;

/* The information stored for the ghost trees */
typedef struct
{
  t8_gloidx_t         global_id;        /* global id of the tree */
  t8_eclass_t         eclass;   /* The trees element class */
  sc_array_t          elements; /* The ghost elements of that tree */
} t8_ghost_tree_t;

/* The data structure stored in the global_tree_to_ghost_tree hash table. */
typedef struct
{
  t8_gloidx_t         global_id;        /* global tree id */
  size_t              index;    /* the index of that global tree in the ghost_trees array. */
} t8_ghost_gtree_hash_t;

/* The data structure stored in the process_offsets array. */
typedef struct
{
  int                 mpirank;  /* rank of the process */
  size_t              tree_index;       /* index of first ghost of this process in ghost_trees */
  size_t              first_element;    /* the index of the first element in the elements array
                                           of the ghost tree. */
} t8_ghost_process_hash_t;

/* The information stored for the remote trees.
 * Each remote process stores an array of these */
typedef struct
{
  t8_gloidx_t         global_id;        /* global id of the tree */
  int                 mpirank;  /* The mpirank of the remote process */
  t8_eclass_t         eclass;   /* The trees element class */
  sc_array_t          elements; /* The ghost elements of that tree */
} t8_ghost_remote_tree_t;

typedef struct
{
  int                 remote_rank;      /* The rank of the remote process */
  sc_array_t          remote_trees;     /* Array of the remote trees of this process */
} t8_ghost_remote_t;

#if 0
/* Compare two ghost_tree entries. We need this function to sort the
 * ghost_trees array by global_id. */
static int
t8_ghost_tree_compare (const void *tree_a, const void *tree_b)
{
  const t8_ghost_tree_t *A = (const t8_ghost_tree_t *) tree_a;
  const t8_ghost_tree_t *B = (const t8_ghost_tree_t *) tree_b;

  if (A->global_id < B->global_id) {
    return -1;
  }
  return A->global_id != B->global_id;
}
#endif

/* The hash function for the global tree hash.
 * As hash value we just return the global tree id. */
static unsigned
t8_ghost_gtree_hash_function (const void *ghost_gtree_hash, const void *data)
{
  const t8_ghost_gtree_hash_t *object =
    (const t8_ghost_gtree_hash_t *) ghost_gtree_hash;

  return (unsigned) object->global_id;
}

/* The equal function for two global tree hash objects.
 * Two t8_ghost_gtree_hash_t are considered equal if theit global
 * tree ids are the same.
 */
static int
t8_ghost_gtree_equal_function (const void *ghost_gtreea,
                               const void *ghost_gtreeb, const void *user)
{
  const t8_ghost_gtree_hash_t *objecta =
    (const t8_ghost_gtree_hash_t *) ghost_gtreea;
  const t8_ghost_gtree_hash_t *objectb =
    (const t8_ghost_gtree_hash_t *) ghost_gtreeb;

  /* return true if and only if the global_ids are the same */
  return objecta->global_id == objectb->global_id;
}

/* The hash value for an entry of the process_offsets hash is the
 * processes mpirank. */
static unsigned
t8_ghost_process_hash_function (const void *process_data,
                                const void *user_data)
{
  const t8_ghost_process_hash_t *process =
    (const t8_ghost_process_hash_t *) process_data;

  return process->mpirank;
}

/* The equal function for the process_offsets array.
 * Two entries are the same if their mpiranks are equal. */
static int
t8_ghost_process_equal_function (const void *process_dataa,
                                 const void *process_datab, const void *user)
{
  const t8_ghost_process_hash_t *processa =
    (const t8_ghost_process_hash_t *) process_dataa;
  const t8_ghost_process_hash_t *processb =
    (const t8_ghost_process_hash_t *) process_datab;

  return processa->mpirank == processb->mpirank;
}

/* The hash funtion for the remote_ghosts hash table.
 * The hash value for an mpirank is just the rank */
static unsigned
t8_ghost_remote_hash_function (const void *remote_data, const void *user_data)
{
  const t8_ghost_remote_t *remote = (const t8_ghost_remote_t *) remote_data;

  return remote->remote_rank;
}

/* The equal function for the remote hash table.
 * Two entries are the same if they have the same rank. */
static int
t8_ghost_remote_equal_function (const void *remote_dataa,
                                const void *remote_datab, const void *user)
{
  const t8_ghost_remote_t *remotea = (const t8_ghost_remote_t *) remote_dataa;
  const t8_ghost_remote_t *remoteb = (const t8_ghost_remote_t *) remote_datab;

  return remotea->remote_rank == remoteb->remote_rank;
}

void
t8_forest_ghost_init (t8_forest_ghost_t * pghost, t8_ghost_type_t ghost_type)
{
  t8_forest_ghost_t   ghost;

  /* We currently only support face-neighbor ghosts */
  T8_ASSERT (ghost_type == T8_GHOST_FACES);

  /* Allocate memory for ghost */
  ghost = *pghost = T8_ALLOC_ZERO (t8_forest_ghost_struct_t, 1);
  /* initialize the reference counter */
  t8_refcount_init (&ghost->rc);
  /* Set the ghost type */
  ghost->ghost_type = ghost_type;

  /* Allocate the trees array */
  ghost->ghost_trees = sc_array_new (sizeof (t8_ghost_tree_t));

  /* initialize the global_tree_to_ghost_tree hash table */
  ghost->glo_tree_mempool = sc_mempool_new (sizeof (t8_ghost_gtree_hash_t));
  ghost->global_tree_to_ghost_tree =
    sc_hash_new (t8_ghost_gtree_hash_function, t8_ghost_gtree_equal_function,
                 NULL, NULL);

  /* initialize the process_offset hash table */
  ghost->proc_offset_mempool =
    sc_mempool_new (sizeof (t8_ghost_process_hash_t));
  ghost->process_offsets =
    sc_hash_new (t8_ghost_process_hash_function,
                 t8_ghost_process_equal_function, NULL, NULL);
  /* initialize the remote ghosts hash table */
  ghost->remote_ghosts =
    sc_hash_array_new (sizeof (t8_ghost_remote_t),
                       t8_ghost_remote_hash_function,
                       t8_ghost_remote_equal_function, NULL);
  /* initialize the remote processes array */
  ghost->remote_processes = sc_array_new (sizeof (int));
}

/* return the number of trees in a ghost */
t8_locidx_t
t8_forest_ghost_num_trees (t8_forest_t forest)
{
  if (forest->ghosts == 0) {
    return 0;
  }
  T8_ASSERT (forest->ghosts != NULL);
  T8_ASSERT (forest->ghosts->ghost_trees != NULL);

  return forest->ghosts->ghost_trees->elem_count;
}

/* Given an index into the ghost_trees array return the ghost tree */
static t8_ghost_tree_t *
t8_forest_ghost_get_tree (t8_forest_t forest, t8_locidx_t lghost_tree)
{
  t8_ghost_tree_t    *ghost_tree;
  t8_forest_ghost_t   ghost;

  T8_ASSERT (t8_forest_is_committed (forest));
  ghost = forest->ghosts;
  T8_ASSERT (ghost != NULL);
  T8_ASSERT (ghost->ghost_trees != NULL);
  T8_ASSERT (0 <= lghost_tree &&
             lghost_tree < t8_forest_ghost_num_trees (forest));

  ghost_tree =
    (t8_ghost_tree_t *) t8_sc_array_index_locidx (ghost->ghost_trees,
                                                  lghost_tree);
  return ghost_tree;
}

/* Given an index in the ghost_tree array, return this tree's number of elements */
t8_locidx_t
t8_forest_ghost_tree_num_elements (t8_forest_t forest,
                                   t8_locidx_t lghost_tree)
{
  t8_ghost_tree_t    *ghost_tree;

  T8_ASSERT (t8_forest_is_committed (forest));

  ghost_tree = t8_forest_ghost_get_tree (forest, lghost_tree);
  return ghost_tree->elements.elem_count;
}

/* Given an index in the ghost_tree array, return this tree's element class */
t8_eclass_t
t8_forest_ghost_get_tree_class (t8_forest_t forest, t8_locidx_t lghost_tree)
{
  t8_ghost_tree_t    *ghost_tree;
  T8_ASSERT (t8_forest_is_committed (forest));

  ghost_tree = t8_forest_ghost_get_tree (forest, lghost_tree);
  return ghost_tree->eclass;
}

/* Given an index in the ghost_tree array, return this tree's global id */
t8_gloidx_t
t8_forest_ghost_get_global_treeid (t8_forest_t forest,
                                   t8_locidx_t lghost_tree)
{
  t8_ghost_tree_t    *ghost_tree;
  T8_ASSERT (t8_forest_is_committed (forest));

  ghost_tree = t8_forest_ghost_get_tree (forest, lghost_tree);
  return ghost_tree->global_id;
}

/* Given an index into the ghost_trees array and for that tree an element index,
 * return the corresponding element. */
t8_element_t
  * t8_forest_ghost_get_element (t8_forest_t forest, t8_locidx_t lghost_tree,
                                 t8_locidx_t lelement)
{
  t8_ghost_tree_t    *ghost_tree;

  T8_ASSERT (t8_forest_is_committed (forest));

  ghost_tree = t8_forest_ghost_get_tree (forest, lghost_tree);
  T8_ASSERT (0 <= lelement &&
             lelement < t8_forest_ghost_tree_num_elements (forest,
                                                           lghost_tree));
  return (t8_element_t *) t8_sc_array_index_locidx (&ghost_tree->elements,
                                                    lelement);
}

#if 0
/* Given a local tree in a forest add all non-local face neighbor trees
 * to a ghost structure. If the trees already exist in the ghost structure
 * they are not added.
 */
static void
t8_forest_ghost_add_tree (t8_forest_t forest, t8_forest_ghost_t ghost,
                          t8_gloidx_t gtreeid)
{
  t8_cmesh_t          cmesh;
  t8_eclass_t         eclass;
  t8_locidx_t         num_cmesh_local_trees;
  t8_locidx_t         lctreeid;
  sc_mempool_t       *hash_mempool;
  t8_ghost_gtree_hash_t *global_to_index_entry;
  int                 is_inserted;

  T8_ASSERT (t8_forest_is_committed (forest));
  T8_ASSERT (ghost != NULL);

  cmesh = t8_forest_get_cmesh (forest);
  /* Compute the cmesh local id of the tree */
  lctreeid = gtreeid - t8_cmesh_get_first_treeid (cmesh);
  num_cmesh_local_trees = t8_cmesh_get_num_local_trees (cmesh);
  /* The tree must be a local tree or ghost tree in the cmesh */
  T8_ASSERT (0 <= lctreeid && lctreeid < num_cmesh_local_trees
             + t8_cmesh_get_num_ghosts (cmesh));

  /* Get the coarse tree and its face-neighbor information */
  if (lctreeid < num_cmesh_local_trees) {
    /* The tree is a local tree in the cmesh */
    eclass = t8_cmesh_get_tree_class (cmesh, lctreeid);
  }
  else {
    /* The tree is a ghost in the cmesh */
    eclass = t8_cmesh_get_ghost_class (cmesh,
                                       lctreeid - cmesh->num_local_trees);
  }

  /* Build a new entry for the global_tree_to_ghost_tree hashtable */
  hash_mempool = ghost->global_tree_to_ghost_tree->allocator;
  global_to_index_entry =
    (t8_ghost_gtree_hash_t *) sc_mempool_alloc (hash_mempool);
  global_to_index_entry->global_id = gtreeid;
  /* Try to add the entry to the array */
  is_inserted = sc_hash_insert_unique (ghost->global_tree_to_ghost_tree,
                                       global_to_index_entry, NULL);
  if (!is_inserted) {
    /* The tree was already added.
     * clean-up and exit */
    sc_mempool_free (hash_mempool, global_to_index_entry);
    return;
  }
  else {
    t8_ghost_tree_t    *ghost_tree;
    /* The tree was not already added. */
    /* Create the entry in the ghost_trees array */
    ghost_tree = (t8_ghost_tree_t *) sc_array_push (ghost->ghost_trees);
    ghost_tree->eclass = eclass;
    ghost_tree->global_id = gtreeid;
    sc_array_init (&ghost_tree->elements, sizeof (t8_element_t *));
    /* Store the array-index of ghost_tree in the hashtable */
    global_to_index_entry->index = ghost->ghost_trees->elem_count - 1;
  }
}

/* Fill the ghost_trees array of a ghost structure with an entry
 * for each ghost tree of the forest.
 * This function does not create the process_offset table yet. */
/* TODO: For the first and last tree we may add more trees than
 *       neccessary, since we add all non-local faceneighbors, and
 *       for these trees not all face-neighbors must contain ghost elements.
 */
static void
t8_forest_ghost_fill_ghost_tree_array (t8_forest_t forest,
                                       t8_forest_ghost_t ghost)
{
  t8_locidx_t         itree, num_local_trees;
  t8_cmesh_t          cmesh;
  t8_ctree_t          ctree;
  t8_locidx_t        *face_neighbors, lneighid, first_ctreeid;
  int                 iface, num_faces;
  t8_ghost_gtree_hash_t global_tree_tgt_search;
  t8_ghost_gtree_hash_t **pglobal_tree_tgt_entry, *global_tree_tgt_entry;
  t8_ghost_tree_t    *ghost_tree;
  size_t              it;

  T8_ASSERT (t8_forest_is_committed (forest));
  T8_ASSERT (ghost != NULL);

  num_local_trees = t8_forest_get_num_local_trees (forest);
  /* If the first tree of the forest is shared with other
   * processes, then it must contain ghost elements */
  if (t8_forest_first_tree_shared (forest)) {
    t8_forest_ghost_add_tree (forest, ghost,
                              t8_forest_get_first_local_tree_id (forest));
  }
  /* If the last tree of the forest is shared with other
   * processes, then it must contain ghost elements */
  if (t8_forest_last_tree_shared (forest)) {
    t8_forest_ghost_add_tree (forest, ghost,
                              t8_forest_get_first_local_tree_id (forest)
                              + num_local_trees - 1);
  }

  cmesh = t8_forest_get_cmesh (forest);
  first_ctreeid = t8_cmesh_get_first_treeid (cmesh);
  /* Iterate over all tree */
  for (itree = 0; itree < num_local_trees; itree++) {
    /* Get a pointer to the coarse mesh tree and its face_neighbors */
    ctree = t8_forest_get_coarse_tree_ext (forest, itree, &face_neighbors,
                                           NULL);
    num_faces = t8_eclass_num_faces[ctree->eclass];
    /* Iterate over all faces of this tree */
    for (iface = 0; iface < num_faces; iface++) {
      /* Compute the (theoretical) forest local id of the neighbor */
      lneighid = t8_forest_cmesh_ltreeid_to_ltreeid (forest,
                                                     face_neighbors[iface]);
      if (lneighid == -1) {
        /* This face neighbor is not a forest local tree.
         * We thus add it to the ghost trees */
        t8_forest_ghost_add_tree (forest, ghost,
                                  ctree->treeid + first_ctreeid);
      }
    }
  }
  /* Now we have added all trees to the array, we have to sort them
   * according to their global_id */
  sc_array_sort (ghost->ghost_trees, t8_ghost_tree_compare);
  /* After sorting, we have to reset the global_tree_to_ghost_tree entries
   * since these store for a global tree id the index in ghost->ghost_trees,
   * which has changed now. */
  for (it = 0; it < ghost->ghost_trees->elem_count; it++) {
    ghost_tree = (t8_ghost_tree_t *) sc_array_index (ghost->ghost_trees, it);
    /* Find the entry belonging to this ghost_tree in the hash table */
    global_tree_tgt_search.global_id = ghost_tree->global_id;
    sc_hash_insert_unique (ghost->global_tree_to_ghost_tree,
                           &global_tree_tgt_search,
                           (void ***) &pglobal_tree_tgt_entry);
    global_tree_tgt_entry = *pglobal_tree_tgt_entry;
    /* Check if the entry that we found was already included and
     * not added to the hash table */
    T8_ASSERT (global_tree_tgt_entry != &global_tree_tgt_search);
    /* Also check for obvious equality */
    T8_ASSERT (global_tree_tgt_entry->global_id == ghost_tree->global_id);
    /* Set the new array index */
    global_tree_tgt_entry->index = it;
  }
}
#endif

/* Initialize a t8_ghost_remote_tree_t */
static void
t8_ghost_init_remote_tree (t8_forest_t forest, t8_gloidx_t gtreeid,
                           int remote_rank,
                           t8_eclass_t eclass,
                           t8_ghost_remote_tree_t * remote_tree)
{
  t8_eclass_scheme_c *ts;
  t8_locidx_t         local_treeid;

  T8_ASSERT (remote_tree != NULL);

  ts = t8_forest_get_eclass_scheme (forest, eclass);
  local_treeid = gtreeid - t8_forest_get_first_local_tree_id (forest);
  /* Set the entries of the new remote tree */
  remote_tree->global_id = gtreeid;
  remote_tree->mpirank = remote_rank;
  remote_tree->eclass = t8_forest_get_eclass (forest, local_treeid);
  /* Initialize the array to store the element */
  sc_array_init (&remote_tree->elements, ts->t8_element_size ());
}

/* Add a new element to the remote hash table (if not already in it).
 * Must be called for elements in linear order */
static void
t8_ghost_add_remote (t8_forest_t forest, t8_forest_ghost_t ghost,
                     int remote_rank, t8_locidx_t ltreeid,
                     t8_element_t * elem)
{
  t8_ghost_remote_t   remote_entry_lookup, *remote_entry;
  t8_ghost_remote_tree_t *remote_tree;
  t8_element_t       *elem_copy;
  t8_eclass_scheme_c *ts;
  t8_eclass_t         eclass;
  sc_array_t         *remote_array;
  size_t              index;
  t8_gloidx_t         gtreeid;
  int                *remote_process_entry;
  int                 level, copy_level;

  /* Get the tree's element class and the scheme */
  eclass = t8_forest_get_tree_class (forest, ltreeid);
  ts = t8_forest_get_eclass_scheme (forest, eclass);
  gtreeid = t8_forest_get_first_local_tree_id (forest) + ltreeid;

  /* Check whether the remote_rank is already present in the remote ghosts
   * array. */
  remote_entry_lookup.remote_rank = remote_rank;
  remote_entry = (t8_ghost_remote_t *)
    sc_hash_array_insert_unique (ghost->remote_ghosts,
                                 (void *) &remote_entry_lookup, &index);
  if (remote_entry != NULL) {
    /* The remote rank was not in the array and was inserted now */
    remote_entry->remote_rank = remote_rank;
    /* Initialize the tree array of the new entry */
    sc_array_init_size (&remote_entry->remote_trees,
                        sizeof (t8_ghost_remote_tree_t), 1);
    /* Get a pointer to the new entry */
    remote_tree = (t8_ghost_remote_tree_t *)
      sc_array_index (&remote_entry->remote_trees, 0);
    t8_debugf ("[H]\t\t Calling init\n");
    /* initialize the remote_tree */
    t8_ghost_init_remote_tree (forest, gtreeid, remote_rank, eclass,
                               remote_tree);
    /* Since the rank is a new remote rank, we also add it to the
     * remote ranks array */
    remote_process_entry = (int *) sc_array_push (ghost->remote_processes);
    *remote_process_entry = remote_rank;
  }
  else {
    /* The remote rank alrady is contained in the remotes array at
     * position index. */
    remote_array = &ghost->remote_ghosts->a;
    remote_entry = (t8_ghost_remote_t *) sc_array_index (remote_array, index);
    T8_ASSERT (remote_entry->remote_rank == remote_rank);
    /* Check whether the tree has already an entry for this process.
     * Since we only add in local tree order the current tree is either
     * the last entry or does not have an entry yet. */
    remote_tree = (t8_ghost_remote_tree_t *)
      sc_array_index (&remote_entry->remote_trees,
                      remote_entry->remote_trees.elem_count - 1);
    if (remote_tree->global_id != gtreeid) {
      /* The tree does not exist in the array. We thus need to add it and
       * initialize it. */
      remote_tree = (t8_ghost_remote_tree_t *)
        sc_array_push (&remote_entry->remote_trees);
      t8_ghost_init_remote_tree (forest, gtreeid, remote_rank, eclass,
                                 remote_tree);
    }
  }
  /* remote_tree now points to a valid entry fore the tree.
   * We can add a copy of the element to the elements array
   * if it does not exist already. If it exists it is the last entry in
   * the array. */
  elem_copy = NULL;
  level = ts->t8_element_level (elem);
  if (remote_tree->elements.elem_count > 0) {
    elem_copy =
      (t8_element_t *) sc_array_index (&remote_tree->elements,
                                       remote_tree->elements.elem_count - 1);
    copy_level = ts->t8_element_level (elem_copy);
  }
  /* Check if the element was not contained in the array.
   * If so, we add a copy of elem to the array.
   * Otherwise, we do nothing. */
  if (elem_copy == NULL ||
      level != copy_level ||
      ts->t8_element_get_linear_id (elem_copy, copy_level) !=
      ts->t8_element_get_linear_id (elem, level)) {
    elem_copy = (t8_element_t *) sc_array_push (&remote_tree->elements);
    ts->t8_element_copy (elem, elem_copy);
  }
}

/* Fill the remote ghosts of a ghost structure.
 * We iterate through all elements and check if their neighbors
 * lie on remote processes. If so, we add the element to the
 * remote_ghosts array of ghost.
 * We also fill the remote_processes here.
 * If ghost_method is 0, then we assume a balanced forest and
 * construct the remote processes by looking at the half neighbors of an element.
 * Otherwise, we use the owners_at_face method.
 */
static void
t8_forest_ghost_fill_remote (t8_forest_t forest, t8_forest_ghost_t ghost,
                             int ghost_method)
{
  t8_element_t       *elem, **half_neighbors, *face_neighbor;
  t8_locidx_t         num_local_trees, num_tree_elems;
  t8_locidx_t         itree, ielem;
  t8_tree_t           tree;
  t8_eclass_t         tree_class, neigh_class, last_class;
  t8_gloidx_t         neighbor_tree;
  t8_eclass_scheme_c *ts, *neigh_scheme, *prev_neigh_scheme;

  int                 iface, num_faces;
  int                 num_face_children, max_num_face_children = 0;
  int                 ichild, owner;
  sc_array_t          owners;
  int                 is_atom;

  last_class = T8_ECLASS_COUNT;
  num_local_trees = t8_forest_get_num_local_trees (forest);

  if (ghost_method != 0) {
    sc_array_init (&owners, sizeof (int));
  }

  t8_debugf ("[H] Start filling remotes.\n");
  /* Loop over the trees of the forest */
  for (itree = 0; itree < num_local_trees; itree++) {
    /* Get a pointer to the tree, the class of the tree, the
     * scheme associated to the class and the number of elements in
     * this tree. */
    tree = t8_forest_get_tree (forest, itree);
    tree_class = t8_forest_get_tree_class (forest, itree);
    ts = t8_forest_get_eclass_scheme (forest, tree_class);

    /* Loop over the elements of this tree */
    num_tree_elems = t8_forest_get_tree_element_count (tree);
    for (ielem = 0; ielem < num_tree_elems; ielem++) {
      /* Get the element of the tree */
      elem = t8_forest_get_tree_element (tree, ielem);
      num_faces = ts->t8_element_num_faces (elem);
      if (ts->t8_element_level (elem) == ts->t8_element_maxlevel ()) {
        /* flag to decide whether this element is at the maximum level */
        is_atom = 1;
      }
      else {
        is_atom = 0;
      }
      for (iface = 0; iface < num_faces; iface++) {
        /* TODO: Check whether the neighbor element is inside the forest,
         *       if not then do not compute the half_neighbors.
         *       This will save computing time. Needs an "element is in forest" function
         *       Currently we perform this check in the half_neighbors function. */

        /* Get the element class of the neighbor tree */
        neigh_class =
          t8_forest_element_neighbor_eclass (forest, itree, elem, iface);
        neigh_scheme = t8_forest_get_eclass_scheme (forest, neigh_class);
        if (ghost_method == 0) {
          /* Use half neighbors */
          /* Get the number of face children of the element at this face */
          num_face_children = ts->t8_element_num_face_children (elem, iface);
          /* regrow the half_neighbors array if neccessary.
           * We also need to reallocate it, if the element class of the neighbor
           * changes */
          if (max_num_face_children < num_face_children ||
              last_class != neigh_class) {
            if (max_num_face_children > 0) {
              /* Clean-up memory */
              prev_neigh_scheme->t8_element_destroy (max_num_face_children,
                                                     half_neighbors);
              T8_FREE (half_neighbors);
            }
            half_neighbors = T8_ALLOC (t8_element_t *, num_face_children);
            /* Allocate memory for the half size face neighbors */
            neigh_scheme->t8_element_new (num_face_children, half_neighbors);
            max_num_face_children = num_face_children;
            last_class = neigh_class;
            prev_neigh_scheme = neigh_scheme;
          }
          if (!is_atom) {
            /* Construct each half size neighbor */
            neighbor_tree =
              t8_forest_element_half_face_neighbors (forest, itree, elem,
                                                     half_neighbors, iface,
                                                     num_face_children);
          }
          else {
            int                 dummy_neigh_face;
            /* This element has maximum level, we only construct its neighbor */
            neighbor_tree =
              t8_forest_element_face_neighbor (forest, itree, elem,
                                               half_neighbors[0], iface,
                                               &dummy_neigh_face);
          }
          if (neighbor_tree >= 0) {
            /* If there exist face neighbor elements (we are not at a domain boundary */
            /* Find the owner process of each face_child */
            for (ichild = 0; ichild < num_face_children; ichild++) {
              /* find the owner */
              owner =
                t8_forest_element_find_owner (forest, neighbor_tree,
                                              half_neighbors[ichild],
                                              neigh_class);
              T8_ASSERT (0 <= owner && owner < forest->mpisize);
              if (owner != forest->mpirank) {
                /* Add the element as a remote element */
                t8_ghost_add_remote (forest, ghost, owner, itree, elem);
              }
            }
          }
        }                       /* end ghost_method 0 */
        else {
          size_t              iowner;
          int                 neigh_face;
          /* Use t8_forest_element_owners_at_face */
          ts->t8_element_new (1, &face_neighbor);
          /* Construct the face neighbor of element */
          neighbor_tree =
            t8_forest_element_face_neighbor (forest, itree, elem,
                                             face_neighbor, iface,
                                             &neigh_face);
          if (neighbor_tree >= 0) {
            /* Build a list of all owners of element that touch face */

            t8_forest_element_owners_at_face (forest, neighbor_tree,
                                              face_neighbor, neigh_class,
                                              neigh_face, &owners);
            T8_ASSERT (owners.elem_count > 0);
            /* Iterate over all owners and if any is not the current process,
             * add this element as remote */
            for (iowner = 0; iowner < owners.elem_count; iowner++) {
              owner = *(int *) sc_array_index (&owners, iowner);
              T8_ASSERT (0 <= owner && owner < forest->mpisize);
              if (owner != forest->mpirank) {
                /* Add the element as a remote element */
                t8_ghost_add_remote (forest, ghost, owner, itree, elem);
              }
            }
            sc_array_truncate (&owners);
          }
          ts->t8_element_destroy (1, &face_neighbor);
        }
      }                         /* end face loop */
    }                           /* end element loop */
  }                             /* end tree loop */

  if (forest->profile != NULL) {
    /* If profiling is enabled, we count the number of remote processes. */
    forest->profile->ghosts_remotes = ghost->remote_processes->elem_count;
  }
  /* Clean-up memory */
  if (ghost_method == 0) {
    neigh_scheme->t8_element_destroy (max_num_face_children, half_neighbors);
    T8_FREE (half_neighbors);
  }
  else {
    sc_array_reset (&owners);
  }
  t8_debugf ("[H] Done filling remotes.\n");
}

/* Begin sending the ghost elements from the remote ranks
 * using non-blocking communication.
 * Afterward
 *  t8_forest_ghost_send_end
 * must be called to end the communication.
 * Returns an array of mpi_send_info_t, one for each remote rank.
 */
static t8_ghost_mpi_send_info_t *
t8_forest_ghost_send_start (t8_forest_t forest, t8_forest_ghost_t ghost,
                            sc_MPI_Request ** requests)
{
  int                 proc_index, remote_rank;
  int                 num_remotes;
  int                 entry_is_found;
  t8_ghost_remote_t   proc_hash;
  size_t              remote_index, first_remote_index;
  sc_array_t         *remotes;
  t8_ghost_remote_t  *remote_entry;
  sc_array_t         *remote_trees;
  t8_ghost_remote_tree_t *remote_tree;
  t8_ghost_mpi_send_info_t *send_info, *current_send_info;
  char               *current_buffer;
  size_t              bytes_written, element_bytes;
  int                 mpiret;

  /* Allocate a send_buffer for each remote rank */
  num_remotes = ghost->remote_processes->elem_count;
  send_info = T8_ALLOC (t8_ghost_mpi_send_info_t, num_remotes);
  *requests = T8_ALLOC (sc_MPI_Request, num_remotes);

  /* Loop over all remote processes */
  for (proc_index = 0; proc_index < (int) ghost->remote_processes->elem_count;
       proc_index++) {
    current_send_info = send_info + proc_index;
    /* Get the rank of the current remote process. */
    remote_rank = *(int *) sc_array_index_int (ghost->remote_processes,
                                               proc_index);
    t8_debugf ("Filling send buffer for process %i\n", remote_rank);
    /* initialize the send_info for the current rank */
    current_send_info->recv_rank = remote_rank;
    current_send_info->num_bytes = 0;
    current_send_info->request = *requests + proc_index;
    /* Lookup the ghost elements for the first tree of this remote */
    proc_hash.remote_rank = remote_rank;
    entry_is_found = sc_hash_array_lookup (ghost->remote_ghosts,
                                           &proc_hash, &first_remote_index);
    /* There must exist remote elements for this rank. */
    T8_ASSERT (entry_is_found);
    /* Get a pointer to the tree entry */
    remotes = &ghost->remote_ghosts->a;
    remote_entry = (t8_ghost_remote_t *) sc_array_index (remotes,
                                                         first_remote_index);
    T8_ASSERT (remote_entry->remote_rank == remote_rank);
    /* Loop over all trees of the remote rank and count the bytes */
    /* At first we store the number of remote trees in the buffer */
    current_send_info->num_bytes += sizeof (size_t);
    /* add padding before the eclass */
    current_send_info->num_bytes +=
      T8_ADD_PADDING (current_send_info->num_bytes);
    /* TODO: put this in a funtion */
    /* TODO: Use remote_entry to count the number of bytes while inserting
     *        the remote ghosts. */
    remote_trees = &remote_entry->remote_trees;
    for (remote_index = 0; remote_index < remote_trees->elem_count;
         remote_index++) {
      /* Get the next remote tree. */
      remote_tree = (t8_ghost_remote_tree_t *) sc_array_index (remote_trees,
                                                               remote_index);
      /* We will store the global tree id, the element class and the list
       * of elements in the send_buffer. */
      current_send_info->num_bytes += sizeof (t8_gloidx_t);
      /* add padding before the eclass */
      current_send_info->num_bytes +=
        T8_ADD_PADDING (current_send_info->num_bytes);
      current_send_info->num_bytes += sizeof (t8_eclass_t);
      /* add padding before the elements */
      current_send_info->num_bytes +=
        T8_ADD_PADDING (current_send_info->num_bytes);
      /* The byte count of the elements */
      element_bytes = remote_tree->elements.elem_size
        * remote_tree->elements.elem_count;
      /* We will store the number of elements */
      current_send_info->num_bytes += sizeof (size_t);
      /* add padding before the elements */
      current_send_info->num_bytes +=
        T8_ADD_PADDING (current_send_info->num_bytes);
      current_send_info->num_bytes += element_bytes;
      /* add padding after the elements */
      current_send_info->num_bytes +=
        T8_ADD_PADDING (current_send_info->num_bytes);
    }

    /* We now now the number of bytes for our send_buffer and thus
     * allocate it. */
    current_send_info->buffer = T8_ALLOC_ZERO (char,
                                               current_send_info->num_bytes);

    /* We iterate through the tree again and store the tree info and the elements
     * into the send_buffer. */
    current_buffer = current_send_info->buffer;
    bytes_written = 0;
    /* Start with the number of remote trees in the buffer */
    memcpy (current_buffer + bytes_written, &remote_trees->elem_count,
            sizeof (size_t));
    bytes_written += sizeof (size_t);
    bytes_written += T8_ADD_PADDING (bytes_written);
    for (remote_index = 0; remote_index < remote_trees->elem_count;
         remote_index++) {
      /* Get a pointer to the tree */
      remote_tree =
        (t8_ghost_remote_tree_t *) sc_array_index (remote_trees,
                                                   remote_index);
      T8_ASSERT (remote_tree->mpirank == remote_rank);

      /* Copy the global tree id */
      memcpy (current_buffer + bytes_written, &remote_tree->global_id,
              sizeof (t8_gloidx_t));
      bytes_written += sizeof (t8_gloidx_t);
      bytes_written += T8_ADD_PADDING (bytes_written);
      /* Copy the trees element class */
      memcpy (current_buffer + bytes_written, &remote_tree->eclass,
              sizeof (t8_eclass_t));
      bytes_written += sizeof (t8_eclass_t);
      bytes_written += T8_ADD_PADDING (bytes_written);
      /* Store the number of elements in the buffer */
      memcpy (current_buffer + bytes_written,
              &(remote_tree->elements.elem_count), sizeof (size_t));
      bytes_written += sizeof (size_t);
      bytes_written += T8_ADD_PADDING (bytes_written);
      /* The byte count of the elements */
      element_bytes = remote_tree->elements.elem_size
        * remote_tree->elements.elem_count;
      /* Copy the elements into the send buffer */
      memcpy (current_buffer + bytes_written, remote_tree->elements.array,
              element_bytes);
      bytes_written += element_bytes;
      /* add padding after the elements */
      bytes_written += T8_ADD_PADDING (bytes_written);

      /* Add to the counter of remote elements. */
      ghost->num_remote_elements += remote_tree->elements.elem_count;
    }                           /* End tree loop */

    T8_ASSERT (bytes_written == current_send_info->num_bytes);
    /* We can now post the MPI_Isend for the remote process */
    t8_debugf
      ("[H] Post send of %i trees  %i elements = %i (==%i) bytes to rank %i.\n",
       (int) remote_trees->elem_count, (int) remote_tree->elements.elem_count,
       (int) current_send_info->num_bytes, (int) bytes_written, remote_rank);
    mpiret =
      sc_MPI_Isend (current_buffer, bytes_written, sc_MPI_BYTE, remote_rank,
                    T8_MPI_GHOST_FOREST, forest->mpicomm,
                    *requests + proc_index);
    SC_CHECK_MPI (mpiret);
  }                             /* end process loop */
  return send_info;
}

static void
t8_forest_ghost_send_end (t8_forest_t forest, t8_forest_ghost_t ghost,
                          t8_ghost_mpi_send_info_t * send_info,
                          sc_MPI_Request * requests)
{
  int                 num_remotes;
  int                 proc_pos, mpiret;

  T8_ASSERT (t8_forest_is_committed (forest));
  T8_ASSERT (ghost != NULL);

  /* Get the number of remote processes */
  num_remotes = ghost->remote_processes->elem_count;

  /* We wait for all communication to end. */
  mpiret = sc_MPI_Waitall (num_remotes, requests, sc_MPI_STATUSES_IGNORE);
  SC_CHECK_MPI (mpiret);

  /* Clean-up */
  for (proc_pos = 0; proc_pos < num_remotes; proc_pos++) {
    T8_FREE (send_info[proc_pos].buffer);
  }
  T8_FREE (send_info);
  T8_FREE (requests);
}

/* Receive a single message from a remote process, after the message was
 * successfully probed.
 * Returns the allocated receive buffer and the number of bytes received */
static char        *
t8_forest_ghost_receive_message (int recv_rank, sc_MPI_Comm comm,
                                 sc_MPI_Status status, int *recv_bytes)
{
  char               *recv_buffer;
  int                 mpiret;

  T8_ASSERT (recv_rank == status.MPI_SOURCE);
  T8_ASSERT (status.MPI_TAG == T8_MPI_GHOST_FOREST);

  /* Get the number of bytes in the message */
  mpiret = sc_MPI_Get_count (&status, sc_MPI_BYTE, recv_bytes);

  /* Allocate receive buffer */
  recv_buffer = T8_ALLOC_ZERO (char, *recv_bytes);
  t8_debugf ("[H] Receiving %i bytes from %i\n", *recv_bytes, recv_rank);
  /* receive the message */
  mpiret = sc_MPI_Recv (recv_buffer, *recv_bytes, sc_MPI_BYTE, recv_rank,
                        T8_MPI_GHOST_FOREST, comm, sc_MPI_STATUS_IGNORE);
  SC_CHECK_MPI (mpiret);
  t8_debugf ("[H] received\n");

  return recv_buffer;
}

/* Parse a message from a remote process and correctly include the received
 * elements in the ghost structure.
 * The message looks like:
 * num_trees | pad | treeid 0 | pad | eclass 0 | pad | num_elems 0 | pad | elements | pad | treeid 1 | ...
 *  size_t   |     |t8_gloidx |     |t8_eclass |     | size_t      |     | t8_element_t |
 *
 * pad is paddind, see T8_ADD_PADDING
 */
/* Currently we expect that the message arrive in order of the sender's rank. */
static void
t8_forest_ghost_parse_received_message (t8_forest_t forest,
                                        t8_forest_ghost_t ghost,
                                        int recv_rank, char *recv_buffer,
                                        int recv_bytes)
{
  size_t              bytes_read, first_tree_index, first_element_index;
  t8_locidx_t         num_trees, itree;
  t8_gloidx_t         global_id;
  t8_eclass_t         eclass;
  size_t              num_elements, old_elem_count;
  t8_ghost_gtree_hash_t *tree_hash, **pfound_tree, *found_tree;
  t8_ghost_tree_t    *ghost_tree;
  t8_eclass_scheme_c *ts;
  t8_element_t       *element_insert;
  t8_ghost_process_hash_t *process_hash;
  int                 added_process;

  t8_debugf ("[H] Parsing received message from rank %i\n", recv_rank);
  bytes_read = 0;
  /* read the number of trees */
  num_trees = *(size_t *) recv_buffer;
  bytes_read += sizeof (size_t);
  bytes_read += T8_ADD_PADDING (bytes_read);

  t8_debugf ("Received %li trees from %i (%i bytes)\n",
             (long) num_trees, recv_rank, recv_bytes);

  for (itree = 0; itree < num_trees; itree++) {
    /* Get tree id */
    /* search if tree was inserted */
    /* if not: new entry, add elements */
    /* if yes: add the elements to the end of the tree's element array. */

    /* read the global id of this tree. */
    global_id = *(t8_gloidx_t *) (recv_buffer + bytes_read);
    bytes_read += sizeof (t8_gloidx_t);
    bytes_read += T8_ADD_PADDING (bytes_read);
    /* read the element class of the tree */
    eclass = *(t8_eclass_t *) (recv_buffer + bytes_read);
    bytes_read += sizeof (t8_eclass_t);
    bytes_read += T8_ADD_PADDING (bytes_read);
    /* read the number of elements sent */
    num_elements = *(size_t *) (recv_buffer + bytes_read);

    /* Add to the counter of ghost elements. */
    ghost->num_ghosts_elements += num_elements;

    bytes_read += sizeof (size_t);
    bytes_read += T8_ADD_PADDING (bytes_read);
    /* Search for the tree in the ghost_trees array */
    tree_hash =
      (t8_ghost_gtree_hash_t *) sc_mempool_alloc (ghost->glo_tree_mempool);
    tree_hash->global_id = global_id;

    /* Get the element scheme for this tree */
    ts = t8_forest_get_eclass_scheme (forest, eclass);
    if (sc_hash_insert_unique (ghost->global_tree_to_ghost_tree, tree_hash,
                               (void ***) &pfound_tree)) {
      /* The tree was not stored already, tree_hash is now an entry in the hash table. */
      /* If the tree was not contained, it is the newest tree in the array and
       * thus has as index the number of currently inserted trees. */
      tree_hash->index = ghost->ghost_trees->elem_count;
      found_tree = tree_hash;
      /* We grow the array by one and initilize the entry */
      ghost_tree = (t8_ghost_tree_t *) sc_array_push (ghost->ghost_trees);
      ghost_tree->global_id = global_id;
      ghost_tree->eclass = eclass;
      /* Initialize the element array */
      sc_array_init_size (&ghost_tree->elements, ts->t8_element_size (),
                          num_elements);
      /* pointer to where the elements are to be inserted */
      element_insert = (t8_element_t *) ghost_tree->elements.array;
      /* Allocate a new tree_hash for the next search */
      old_elem_count = 0;
      tree_hash =
        (t8_ghost_gtree_hash_t *) sc_mempool_alloc (ghost->glo_tree_mempool);
    }
    else {
      /* The entry was found in the trees array */
      found_tree = *pfound_tree;
      T8_ASSERT (found_tree->global_id == global_id);
      /* Get a pointer to the tree */
      ghost_tree = (t8_ghost_tree_t *) sc_array_index (ghost->ghost_trees,
                                                       found_tree->index);
      T8_ASSERT (ghost_tree->eclass == eclass);
      T8_ASSERT (ghost_tree->global_id == global_id);
      T8_ASSERT (ghost_tree->elements.elem_size == ts->t8_element_size ());

      old_elem_count = ghost_tree->elements.elem_count;

      /* Grow the elements array of the tree to fit the new elements */
      sc_array_resize (&ghost_tree->elements, old_elem_count + num_elements);
      /* Get a pointer to where the new elements are to be inserted */
      element_insert =
        (t8_element_t *) sc_array_index (&ghost_tree->elements,
                                         old_elem_count);
    }
    if (itree == 0) {
      /* We store the index of the first tree and the first element of this
       * rank */
      first_tree_index = found_tree->index;
      first_element_index = old_elem_count;
    }
    /* Insert the new elements */
    memcpy (element_insert, recv_buffer + bytes_read,
            num_elements * ts->t8_element_size ());

    bytes_read += num_elements * ts->t8_element_size ();
    bytes_read += T8_ADD_PADDING (bytes_read);
  }
  T8_ASSERT (bytes_read == (size_t) recv_bytes);
  T8_FREE (recv_buffer);

  /* At last we add the receiving rank to the ghosts process_offset hash table */
  process_hash =
    (t8_ghost_process_hash_t *) sc_mempool_alloc (ghost->proc_offset_mempool);
  process_hash->mpirank = recv_rank;
  process_hash->tree_index = first_tree_index;
  process_hash->first_element = first_element_index;
  /* Insert this rank into the hash table. We assert if the rank was not already
   * contained. */
  added_process = sc_hash_insert_unique (ghost->process_offsets, process_hash,
                                         NULL);
  T8_ASSERT (added_process);
}

/* In forest_ghost_receive we need a lookup table to give us the position
 * of a process in the ghost->remote_processes array, given the rank of
 * a process. We implement this via a hash table with the following struct
 * as entry. */
typedef struct t8_recv_list_entry_struct
{
  int                 rank;     /* The rank of this process */
  int                 pos_in_remote_processes;  /* The position of this process in the remote_processes array */
} t8_recv_list_entry_t;

/* We hash these entries by their rank */
unsigned
t8_recv_list_entry_hash (const void *v1, const void *u)
{
  const t8_recv_list_entry_t *e1 = (const t8_recv_list_entry_t *) v1;

  return e1->rank;
}

/* two entries are considered equal if they have the same rank. */
int
t8_recv_list_entry_equal (const void *v1, const void *v2, const void *u)
{
  const t8_recv_list_entry_t *e1 = (const t8_recv_list_entry_t *) v1;
  const t8_recv_list_entry_t *e2 = (const t8_recv_list_entry_t *) v2;

  return e1->rank == e2->rank;
}

/* Probe for all incoming messages from the remote ranks and receive them.
 * We receive the message in the order in which they arrive. To achieve this,
 * we have to use polling. */
static void
t8_forest_ghost_receive (t8_forest_t forest, t8_forest_ghost_t ghost)
{
  int                 num_remotes;
  int                 proc_pos;
  int                 recv_rank;
  int                 mpiret;
  sc_MPI_Comm         comm;
  sc_MPI_Status       status;

  T8_ASSERT (t8_forest_is_committed (forest));
  T8_ASSERT (ghost != NULL);

  comm = forest->mpicomm;
  /* Get the number of remote processes */
  num_remotes = ghost->remote_processes->elem_count;

  if (num_remotes == 0) {
    /* There is nothing to do */
    return;
  }

#if 1
  {
    /*       This code receives the message in order of there arrival.
     *       This is effective in terms of runtime, but makes it more difficult
     *       to store the received data, since the data has to be stored in order of
     *       ascending ranks.
     *       We include the received data into the ghost structure in order of the
     *       ranks of the receivers and we do this as soon as the message from
     *       the next rank that we can include was received. */
    char              **buffer;
    int                *recv_bytes;
    int                 received_messages = 0;
    int                *received_flag;
    int                 last_rank_parsed = -1, parse_it;
    t8_recv_list_entry_t recv_list_entry, *recv_list_entries, **pfound,
      *found;
    int                 ret;
    sc_hash_t          *recv_list_entries_hash;
#if 0
    sc_link_t          *proc_it, *prev;
    int                 iprobe_flag;
    sc_list_t          *receivers;
#endif

    buffer = T8_ALLOC (char *, num_remotes);
    recv_bytes = T8_ALLOC (int, num_remotes);
    received_flag = T8_ALLOC_ZERO (int, num_remotes);
    recv_list_entries_hash = sc_hash_new (t8_recv_list_entry_hash,
                                          t8_recv_list_entry_equal, NULL,
                                          NULL);
    recv_list_entries = T8_ALLOC (t8_recv_list_entry_t, num_remotes);

    /* Sort the array of remote processes, such that the ranks are in
     * ascending order. */
    sc_array_sort (ghost->remote_processes, sc_int_compare);

    /* We build a hash table of all ranks from which we receive and their position
     * in the remote_processes array. */
#if 0                           /* polling */
    receivers = sc_list_new (NULL);
#endif
    for (proc_pos = 0; proc_pos < num_remotes; proc_pos++) {
      recv_list_entries[proc_pos].rank =
        *(int *) sc_array_index_int (ghost->remote_processes, proc_pos);
      recv_list_entries[proc_pos].pos_in_remote_processes = proc_pos;
      ret = sc_hash_insert_unique (recv_list_entries_hash,
                                   recv_list_entries + proc_pos, NULL);
      T8_ASSERT (ret == 1);
#if 0                           /* polling */
      sc_list_append (receivers, recv_list_entries + proc_pos);
#endif
    }

  /****     Actual communication    ****/

    /* Until there is only one sender left we iprobe for a message for each
     * sender and if there is one we receive it and remove the sender from
     * the list.
     * The last message can be received via probe */
    while (received_messages < num_remotes) {
#if 0
      /* TODO: This part of the code using polling and IProbe to receive the
       *       messages. We replaced with a non-polling version that uses the
       *       blocking Probe. */
      iprobe_flag = 0;
      prev = NULL;              /* ensure that if the first receive entry is matched first,
                                   it is removed properly. */
      for (proc_it = receivers->first; proc_it != NULL && iprobe_flag == 0;) {
        /* pointer to the rank of a receiver */
        recv_rank = ((t8_recv_list_entry_t *) proc_it->data)->rank;
        proc_pos =
          ((t8_recv_list_entry_t *) proc_it->data)->pos_in_remote_processes;
#endif
        /* nonblocking probe for a message. */
        mpiret = sc_MPI_Probe (sc_MPI_ANY_SOURCE, T8_MPI_GHOST_FOREST, comm,
                               &status);
        SC_CHECK_MPI (mpiret);
#if 0
        if (iprobe_flag == 0) {
          /* There is no message to receive, we continue */
          prev = proc_it;
          proc_it = proc_it->next;
        }
        else {
#endif
          /* There is a message to receive, we receive it. */
          recv_rank = status.MPI_SOURCE;
          /* Get the position of this rank in the remote processes array */
          recv_list_entry.rank = recv_rank;
          ret = sc_hash_lookup (recv_list_entries_hash, &recv_list_entry,
                                (void ***) &pfound);
          T8_ASSERT (ret != 0);
          found = *pfound;
          proc_pos = found->pos_in_remote_processes;

          T8_ASSERT (status.MPI_TAG == T8_MPI_GHOST_FOREST);
          t8_debugf ("[H] Receive message from %i [%i]\n", recv_rank,
                     proc_pos);
          buffer[proc_pos] =
            t8_forest_ghost_receive_message (recv_rank, comm, status,
                                             recv_bytes + proc_pos);
          /* mark this entry as received. */
          T8_ASSERT (received_flag[proc_pos] == 0);
          received_flag[proc_pos] = 1;
          received_messages++;
          /* Parse all messages that we can parse now.
           * We have to parse the messages in order of their rank. */
          T8_ASSERT (last_rank_parsed < proc_pos);
          /* For all ranks that we haven't parsed yet, but can be parsed in order */
          for (parse_it = last_rank_parsed + 1; parse_it < num_remotes &&
               received_flag[parse_it] == 1; parse_it++) {
            recv_rank =
              *(int *) sc_array_index_int (ghost->remote_processes, parse_it);
            t8_forest_ghost_parse_received_message (forest, ghost, recv_rank,
                                                    buffer[parse_it],
                                                    recv_bytes[parse_it]);
            last_rank_parsed++;
          }

#if 0                           /* polling */
          /* Remove the process from the list of receivers. */
          proc_it = proc_it->next;
          sc_list_remove (receivers, prev);
        }
      }                         /* end for */
#endif
    }                           /* end while */
#if 0
    /* polling */
    T8_ASSERT (receivers->elem_count == 1);
    /* Get the last rank from which we didnt receive yet */
    recv_list_entry = (t8_recv_list_entry_t *) sc_list_pop (receivers);
    recv_rank = recv_list_entry->rank;
    proc_pos = recv_list_entry->pos_in_remote_processes;
    /* destroy the list */
    sc_list_destroy (receivers);
    /* Blocking probe for the last message */
    mpiret = sc_MPI_Probe (recv_rank, T8_MPI_GHOST_FOREST, comm, &status);
    SC_CHECK_MPI (mpiret);
    /* Receive the message */
    T8_ASSERT (received_flag[proc_pos] == 0);
    buffer[proc_pos] = t8_forest_ghost_receive_message (recv_rank, comm,
                                                        status,
                                                        recv_bytes +
                                                        proc_pos);
    received_flag[proc_pos] = 1;
    received_messages++;
    T8_ASSERT (received_messages == num_remotes);
    /* parse all messages that are left */
    /* For all ranks that we haven't parsed yet, but can be parsed in order */
    for (parse_it = last_rank_parsed + 1; parse_it < num_remotes &&
         received_flag[parse_it] == 1; parse_it++) {
      recv_rank =
        *(int *) sc_array_index_int (ghost->remote_processes, parse_it);
      t8_forest_ghost_parse_received_message (forest, ghost, recv_rank,
                                              buffer[parse_it],
                                              recv_bytes[parse_it]);
      last_rank_parsed++;
    }
#endif
#ifdef T8_ENABLE_DEBUG
    for (parse_it = 0; parse_it < num_remotes; parse_it++) {
      T8_ASSERT (received_flag[parse_it] == 1);
    }
#endif
    T8_ASSERT (last_rank_parsed == num_remotes - 1);

    /* clean-up */
    sc_hash_destroy (recv_list_entries_hash);
    T8_FREE (buffer);
    T8_FREE (received_flag);
    T8_FREE (recv_list_entries);
    T8_FREE (recv_bytes);

#endif
  }
#if 0
  /* Receive the message in order of the sender's rank,
   * this is a non-optimized version of the code below.
   * slow but simple. */

  /* Sort the array of remote processes, such that the ranks are in
   * ascending order. */
  sc_array_sort (ghost->remote_processes, sc_int_compare);

  for (proc_pos = 0; proc_pos < num_remotes; proc_pos++) {
    recv_rank =
      (int *) sc_array_index_int (ghost->remote_processes, proc_pos);
    /* blocking probe for a message. */
    mpiret = sc_MPI_Probe (*recv_rank, T8_MPI_GHOST_FOREST, comm, &status);
    SC_CHECK_MPI (mpiret);
    /* receive message */
    buffer =
      t8_forest_ghost_receive_message (*recv_rank, comm, status, &recv_bytes);
    t8_forest_ghost_parse_received_message (forest, ghost, *recv_rank, buffer,
                                            recv_bytes);
  }
#endif
}

/* Create one layer of ghost elements, following the algorithm
 * in p4est: Scalable Algorithms For Parallel Adaptive
 *           Mesh Refinement On Forests of Octrees
 *           C. Burstedde, L. C. Wilcox, O. Ghattas
 */
void
t8_forest_ghost_create (t8_forest_t forest)
{
  t8_forest_ghost_t   ghost;
  t8_ghost_mpi_send_info_t *send_info;
  sc_MPI_Request     *requests;

  if (forest->ghost_type == T8_GHOST_NONE) {
    t8_debugf ("WARNING: Trying to construct ghosts with ghost_type NONE. "
               "Ghost layer is not constructed.\n");
    return;
  }

  if (forest->profile != NULL) {
    /* If profiling is enabled, we measure the runtime of ghost_create */
    forest->profile->ghost_runtime = -sc_MPI_Wtime ();
  }

  /* Initialize the ghost structure */
  t8_forest_ghost_init (&forest->ghosts, forest->ghost_type);
  ghost = forest->ghosts;

  /* Construct the remote elements and processes. */
  t8_forest_ghost_fill_remote (forest, ghost, 0);

  /* Start sending the remote elements */
  send_info = t8_forest_ghost_send_start (forest, ghost, &requests);

  /* Reveive the ghost elements from the remote processes */
  t8_forest_ghost_receive (forest, ghost);

  /* End sending the remote elements */
  t8_forest_ghost_send_end (forest, ghost, send_info, requests);

  if (forest->profile != NULL) {
    /* If profiling is enabled, we measure the runtime of ghost_create */
    forest->profile->ghost_runtime += sc_MPI_Wtime ();
    /* We also store the number of ghosts and remotes */
    forest->profile->ghosts_received = ghost->num_ghosts_elements;
    forest->profile->ghosts_shipped = ghost->num_remote_elements;
  }
}

/* Print a forest ghost structure */
void
t8_forest_ghost_print (t8_forest_t forest)
{
  t8_forest_ghost_t   ghost;
  t8_ghost_remote_t   remote_search, *remote_found;
  t8_ghost_remote_tree_t *remote_tree;
  t8_ghost_process_hash_t proc_hash, **pfound, *found;
  size_t              iremote, itree;
  int                 ret;
  size_t              index;
  char                remote_buffer[BUFSIZ] = "";
  char                buffer[BUFSIZ] = "";

  ghost = forest->ghosts;
  snprintf (remote_buffer + strlen (remote_buffer),
            BUFSIZ - strlen (remote_buffer), "\tRemotes:\n");
  snprintf (buffer + strlen (buffer), BUFSIZ - strlen (buffer),
            "\tReceived:\n");

  for (iremote = 0; iremote < ghost->remote_processes->elem_count; iremote++) {
    /* Get the rank of the remote process */
    remote_search.remote_rank =
      *(int *) sc_array_index (ghost->remote_processes, iremote);
    /* Search for the remote process in the hash table */
    ret = sc_hash_array_lookup (ghost->remote_ghosts, &remote_search, &index);
    remote_found = (t8_ghost_remote_t *)
      sc_array_index (&ghost->remote_ghosts->a, index);
    T8_ASSERT (ret != 0);
    /* investigate the entry of this remote process */
    snprintf (remote_buffer + strlen (remote_buffer),
              BUFSIZ - strlen (remote_buffer), "\t[Rank %i] (%li trees):\n",
              remote_found->remote_rank,
              remote_found->remote_trees.elem_count);
    for (itree = 0; itree < remote_found->remote_trees.elem_count; itree++) {
      remote_tree = (t8_ghost_remote_tree_t *)
        sc_array_index (&remote_found->remote_trees, itree);
      snprintf (remote_buffer + strlen (remote_buffer),
                BUFSIZ - strlen (remote_buffer),
                "\t\t[id: %lli, class: %s, #elem: %li]\n",
                (long long) remote_tree->global_id,
                t8_eclass_to_string[remote_tree->eclass],
                (long) remote_tree->elements.elem_count);
    }

    /* Investigate the elements that we received from this process */
    proc_hash.mpirank = remote_search.remote_rank;
    /* look up this rank in the hash table */
    ret = sc_hash_insert_unique (ghost->process_offsets, &proc_hash,
                                 (void ***) &pfound);

    T8_ASSERT (ret == 0);
    found = *pfound;
    snprintf (buffer + strlen (buffer), BUFSIZ - strlen (buffer),
              "\t[Rank %i] First tree: %li\n\t\t First element: %li\n",
              remote_search.remote_rank,
              (long) found->tree_index, (long) found->first_element);
  }
  t8_debugf ("Ghost structure:\n%s\n%s\n", remote_buffer, buffer);
}

/* Completely destroy a ghost structure */
static void
t8_forest_ghost_reset (t8_forest_ghost_t * pghost)
{
  t8_forest_ghost_t   ghost;
  size_t              it, it_trees;
  t8_ghost_tree_t    *ghost_tree;
  t8_ghost_remote_t  *remote_entry;
  t8_ghost_remote_tree_t *remote_tree;

  T8_ASSERT (pghost != NULL);
  ghost = *pghost;
  T8_ASSERT (ghost != NULL);
  T8_ASSERT (ghost->rc.refcount == 0);

  /* Clean-up the arrays */
  for (it_trees = 0; it_trees < ghost->ghost_trees->elem_count; it_trees++) {
    ghost_tree = (t8_ghost_tree_t *) sc_array_index (ghost->ghost_trees,
                                                     it_trees);
    sc_array_reset (&ghost_tree->elements);
  }

  sc_array_destroy (ghost->ghost_trees);
  sc_array_destroy (ghost->remote_processes);
  /* Clean-up the hashtables */
  sc_hash_destroy (ghost->global_tree_to_ghost_tree);
  sc_hash_destroy (ghost->process_offsets);
  /* Clean-up the remote ghost entries */
  for (it = 0; it < ghost->remote_ghosts->a.elem_count; it++) {
    remote_entry = (t8_ghost_remote_t *)
      sc_array_index (&ghost->remote_ghosts->a, it);
    for (it_trees = 0; it_trees < remote_entry->remote_trees.elem_count;
         it_trees++) {
      remote_tree = (t8_ghost_remote_tree_t *)
        sc_array_index (&remote_entry->remote_trees, it_trees);
      sc_array_reset (&remote_tree->elements);
    }
    sc_array_reset (&remote_entry->remote_trees);
  }
  sc_hash_array_destroy (ghost->remote_ghosts);

  /* Clean-up the memory pools for the data inside
   * the hash tables */
  sc_mempool_destroy (ghost->glo_tree_mempool);
  sc_mempool_destroy (ghost->proc_offset_mempool);

  /* Free the ghost */
  T8_FREE (ghost);
  pghost = NULL;
}

void
t8_forest_ghost_ref (t8_forest_ghost_t ghost)
{
  T8_ASSERT (ghost != NULL);

  t8_refcount_ref (&ghost->rc);
}

void
t8_forest_ghost_unref (t8_forest_ghost_t * pghost)
{
  t8_forest_ghost_t   ghost;

  T8_ASSERT (pghost != NULL);
  ghost = *pghost;
  T8_ASSERT (ghost != NULL);

  if (t8_refcount_unref (&ghost->rc)) {
    t8_forest_ghost_reset (pghost);
  }
}

void
t8_forest_ghost_destroy (t8_forest_ghost_t * pghost)
{
  T8_ASSERT (pghost != NULL && *pghost != NULL &&
             t8_refcount_is_last (&(*pghost)->rc));
  t8_forest_ghost_unref (pghost);
  T8_ASSERT (*pghost == NULL);
}

T8_EXTERN_C_END ();
