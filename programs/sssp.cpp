// Single-source shortest paths via MapReduce
// Input:   A directed graph, provided by Karl's files.
// Output:  For each vertex Vi, the shortest weighted distance from a randomly
//          selected source vertex S to Vi, along with the predecessor vertex 
//          of Vi in the shortest weighted path from S to Vi.
// 
// Assume:  Vertices are identified by positive whole numbers in range [1:N].
//
// This implementation uses a BFS-like algorithm.  See sssp.txt for details.

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include "mapreduce.h"
#include "keyvalue.h"
#include "blockmacros.hpp"
#include "read_fb_data.hpp"
#include "renumber_graph.hpp"
#include "shared.hpp"

using namespace std;
using namespace MAPREDUCE_NS;

/////////////////////////////////////////////////////////////////////////////
// Class used to pass distance information through the MapReduce system.
template <typename VERTEX, typename EDGE>
class DISTANCE {
public:
  DISTANCE(){
    memset(&(e.v.v), 0, sizeof(VERTEX));
    e.wt = INT_MAX;
    current = false;
  };
  ~DISTANCE(){};
  EDGE e;        // Edge describing the distance of a vtx from S; 
                 // e.v is predecessor vtx; e.wt is distance from S through e.v.
  bool current;  // Flag indicating that this distance is the current state
                 // for the vtx (the currently accepted best distance).
                 // Needed so we can know when to stop (when no vtx distances
                 // change in an iteration).
};


/////////////////////////////////////////////////////////////////////////////
// add_source:  Add the source vertex to the MapReduce object as initial vtx.
// Map:    Input:   randomly selected vertex in [1:N] for source.
//         Output:  One key-value pair for the source.
template <typename VERTEX, typename EDGE>
void add_source(int nmap, KeyValue *kv, void *ptr)
{
  VERTEX *v = (VERTEX *) ptr;
  DISTANCE<VERTEX, EDGE> d;
  d.e.wt = 0;  // Distance from source to itself is zero.
  kv->add((char *) v, sizeof(VERTEX),
          (char *) &d, sizeof(DISTANCE<VERTEX, EDGE>));
}

/////////////////////////////////////////////////////////////////////////////
// bfs_with_distances:  Do breadth-first search, keeping track of shortest
// distance from source.
// Reduce:  Input:   Key-multivalue 
//                   Key = Vi
//                   Multivalue = [{Vj, Wij} for all adj vertices Vj] + 
//                                 (possibly) {Vk, Dk, true/false} representing
//                                 shortest distance from S to Vi through
//                                 preceding vertex Vk.
//                 
//          Compute: If any distances from S to Vi have been computed so far, 
//                   find minimum distance D; keep track of the preceding
//                   vertex Vd giving this best distance.  
//                   If changed the minimum distance, done = 0.
//
//          Output:  Only if a minimum distance was computed, emit one key-value
//                   for each adjacent vertex Vj:
//                   Key = Vj
//                   Value = {Vi, D+Wij, false}
//                   Also emit best distance so far for Vi:
//                   Key = Vi
//                   Value = {Vd, D, true}, where Vd is the preceding vertex
//                           corresponding to the best distance.
template <typename VERTEX, typename EDGE>
void bfs_with_distances(char *key, int keybytes, char *multivalue,
                        int nvalues, int *valuebytes, KeyValue *kv, void *ptr)
{
  int *done = (int *) ptr;
  VERTEX *vi = (VERTEX *) key;

  CHECK_FOR_BLOCKS(multivalue, valuebytes, nvalues)


  // First, find the shortest distance to Vi, if any have been computed yet.
  bool found = false;
  DISTANCE<VERTEX, EDGE> previous; // Best distance for Vi from prev iterations.
  DISTANCE<VERTEX, EDGE> shortest; // Shortest path so far to Vi.

  BEGIN_BLOCK_LOOP(multivalue, valuebytes, nvalues)

  int offset = 0;
  for (int j = 0; j < nvalues; j++) {
    // Multivalues are either edges or distances.  Distances use more bytes.
    if (valuebytes[j] == sizeof(DISTANCE<VERTEX, EDGE>)) {
      // This is a distance value.
      DISTANCE<VERTEX, EDGE> *d = (DISTANCE<VERTEX, EDGE>*) &multivalue[offset];
      found = true;
      if (d->e.wt < shortest.e.wt)  shortest = *d;   // shortest path so far.
      if (d->current) previous = *d;     // currently accepted best distance.
    }
    offset += valuebytes[j];
  }

  END_BLOCK_LOOP

  // if !found, this vtx hasn't been visited along a path from S yet.
  // It is only in mrpath because we added in the entire graph to get the
  // edge lists.  We don't have to emit anything for this vtx.

  if (found) {
    // Emit best distance so far for Vi.
    shortest.current = true;
    kv->add(key, keybytes, (char *) &shortest, sizeof(DISTANCE<VERTEX, EDGE>));

    // Check stopping criterion: not done if (1) this is the first distance
    // computed for Vi, OR (2) the distance for Vi was updated.
    if (!previous.current || 
        (previous.current && (shortest.e.wt != previous.e.wt))) {

      *done = 0;
  
      // Next, augment the path from Vi to each Vj with the weight Wij.
      BEGIN_BLOCK_LOOP(multivalue, valuebytes, nvalues)
  
      int offset = 0;
      for (int j = 0; j < nvalues; j++) {
        if (valuebytes[j] == sizeof(EDGE)) { 
          // This is an adjacency value.
          EDGE *e = (EDGE *) &multivalue[offset];
          DISTANCE<VERTEX, EDGE> dist;
          dist.e.v = *vi;    // Predecessor of Vj along the path.
          dist.e.wt = shortest.e.wt + e->wt; 
          dist.current = false;
          kv->add((char *) &(e->v), sizeof(VERTEX),
                  (char *) &dist, sizeof(DISTANCE<VERTEX, EDGE>));
        }
        offset += valuebytes[j];
      }
  
      END_BLOCK_LOOP
    }
  }
}

/////////////////////////////////////////////////////////////////////////////
//  default_vtx_distance:   Earlier, we didn't emit an initial value of 
//  infinity for every vertex, as we'd have to carry that around throughout 
//  the iterations.  But now we'll add initial values in so that we report
//  (infinite) distances for vertices that are not connected to S.
//
//  Reduce:  Input:   Key-multivalue
//                    Key = Vi hashkey
//                    Multivalue = NULL
//
//
//           Output:  Key = Vi
//                    Value = default shortest distance INT_MAX through vtx -1.
template <typename VERTEX, typename EDGE>
void default_vtx_distance(char *key, int keybytes, char *multivalue,
                          int nvalues, int *valuebytes, KeyValue *kv, void *ptr)
{
  DISTANCE<VERTEX, EDGE> shortest;    // Constructor initializes values.

  kv->add(key, keybytes, (char *) &shortest, sizeof(DISTANCE<VERTEX, EDGE>));
}

/////////////////////////////////////////////////////////////////////////////
//  last_distance_update
//  Reduce:  Input:   Key-multivalue
//                    Key = Vi
//                    Multivalue = [{Vk, Dk, true/false}] 
//                                 representing the shortest
//                                 distance from S to Vi through preceding
//                                 vertex Vk.
//
//           Compute: Find minimum distance D, keeping track of corresponding
//                    preceding vertex Vd.
//
//           Output:  Emit distance from S to Vi:
//                    Key = Vi
//                    Value = {Vd, D}
template <typename VERTEX, typename EDGE>
void last_distance_update(char *key, int keybytes, char *multivalue,
                          int nvalues, int *valuebytes, KeyValue *kv, void *ptr)
{

  CHECK_FOR_BLOCKS(multivalue, valuebytes, nvalues)

  // First, find the shortest distance to Vi, if any have been computed yet.
  DISTANCE<VERTEX, EDGE> shortest;     // The shortest path so far to Vi.

  BEGIN_BLOCK_LOOP(multivalue, valuebytes, nvalues)

  DISTANCE<VERTEX, EDGE> *d = (DISTANCE<VERTEX, EDGE> *) multivalue;
  for (int j = 0; j < nvalues; j++)
    if (d[j].e.wt < shortest.e.wt) shortest = d[j]; // shortest path so far.

  END_BLOCK_LOOP

  // Then emit the best distance from S to Vi.
  // Don't need to emit the DISTANCE structure here, as we don't need
  // the stopping-criterion flag any longer.
  kv->add(key, keybytes, (char *) &shortest.e, sizeof(EDGE));
}

/////////////////////////////////////////////////////////////////////////////
//  output_distances: Write the best distance from S to Vi to a file.
//
//  Reduce:  Input:   Key-multivalue
//                    Key = Vi
//                    Multivalue = {Vk, Dk} representing the 
//                                 shortest distance from S to Vi through 
//                                 preceding vertex Vk.
//                    Note that nvalues should equal one.
//
//           Output:  Write path entries to a file
//                    Vi D Vd
//                    No key-values emitted.
template <typename VERTEX, typename EDGE>
void output_distances(char *key, int keybytes, char *multivalue,
                      int nvalues, int *valuebytes, KeyValue *kv, void *ptr)
{
  ofstream *fp = (ofstream *) ptr;
  EDGE *e = (EDGE *) multivalue;

  if (nvalues > 1) {
    cout << "Sanity check failed in output_distances:  nvalues = " 
         << nvalues << endl;
    MPI_Abort(MPI_COMM_WORLD,-1);
  }
  
  *fp << *((VERTEX *)key) << "   " << *e << endl;
}


/////////////////////////////////////////////////////////////////////////////
template <typename VERTEX, typename EDGE>
class SSSP {
public:
  SSSP(MapReduce *mrvert_, MapReduce *mredge_) : mrvert(mrvert_),
                                                 mredge(mredge_)
  {
    MPI_Comm_rank(MPI_COMM_WORLD, &me); 
    MPI_Comm_size(MPI_COMM_WORLD, &np); 
  };
  ~SSSP(){};
  void run(int);
private:
  int me;
  int np;
  MapReduce *mrvert;
  MapReduce *mredge;
  
};

template <typename VERTEX, typename EDGE>
void SSSP<VERTEX, EDGE>::run(int iteration) 
{
  // Create a new MapReduce object, Paths.
  // Select a source vertex.  
  //       Processor 0 selects random number S in range [1:N] for N vertices.
  //       Processor 0 emits into Paths key-value pair [S, {-1, 0}], 
  //       signifying that vertex S has distance zero from itself, with no
  //       predecessor.

  MapReduce *mrpath = new MapReduce(MPI_COMM_WORLD);
  VERTEX source;
  if (me == 0) cout << "Selecting source vertex..." << endl;

#ifdef NEW_OUT_OF_CORE
  cout << "Random source selection not yet working with out-of-core "
       << "implementation." << endl;
  MPI_Abort(MPI_COMM_WORLD, -1);

#else
  if (me == iteration%np) {
    uint64_t nkey = mrvert->kv->nkey;
    uint64_t idx = drand48() * nkey;
    memcpy(&source, &(mrvert->kv->keydata[mrvert->kv->keys[idx]]), 
           sizeof(VERTEX));
    cout << "Source vertex:  " << source << endl;
  }
#endif
    
  if (me == 0) cout << "Adding source vertex to MRPath..." << endl;
  MPI_Bcast(&source, sizeof(VERTEX), MPI_BYTE, iteration%np, MPI_COMM_WORLD);
  mrpath->map(1, add_source<VERTEX,EDGE>, &source);

  //  Perform a BFS from S, editing distances as visit vertices.
  if (me == 0) cout << "Beginning while loop..." << endl;
  int done = 0;
  while (!done) {
    done = 1;
 
    // Add Edges to Paths.
    // Collate Paths by vertex; collects edges and any distances 
    // computed so far.
#ifdef NEW_OUT_OF_CORE
    mrpath->add(mredge);
#else
    mrpath->kv->add(mredge->kv);
#endif

    mrpath->collate(NULL);
    mrpath->reduce(bfs_with_distances<VERTEX,EDGE>, &done);

    int alldone;
    MPI_Allreduce(&done, &alldone, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
    done = alldone;
  }
  if (me == 0) cout << "Done while loop..." << endl;

  // Finish up:  Want distance from S to all vertices.  Have to add in 
  //             vertices that are not connected to S through any paths.
#ifdef NEW_OUT_OF_CORE
  MapReduce *mrinit = mrvert->copy();
#else
  MapReduce *mrinit = new MapReduce(*mrvert);
#endif
  mrinit->clone();
  mrinit->reduce(default_vtx_distance<VERTEX,EDGE>, NULL);

#ifdef NEW_OUT_OF_CORE
  mrpath->add(mrinit);
#else
  mrpath->kv->add(mrinit->kv);
#endif
  delete mrinit;

  mrpath->collate(NULL);
  mrpath->reduce(last_distance_update<VERTEX,EDGE>, NULL);

  // Now mrpath contains one key-value per vertex Vi:
  // Key = Vi
  // Value = {Vd, D}:  the predecessor vtx Vd, the distance D from 
  //                   S to Vi, and an extraneous flag that we could
  //                   remove.

  // Output results.

  char filename[32];
  sprintf(filename, "distance_iteration_%d.%d", iteration, me);
  ofstream fp;
  fp.open(filename);

  mrpath->clone();
  mrpath->reduce(output_distances<VERTEX,EDGE>, (void *) &fp);

  fp.close();
   
  delete mrpath;
}

/////////////////////////////////////////////////////////////////////////////
int main(int narg, char **args)
{
  MPI_Init(&narg, &args);
  int me, np;
  MPI_Comm_size(MPI_COMM_WORLD, &np);
  MPI_Comm_rank(MPI_COMM_WORLD, &me);

  if (np < 100) greetings();

  // Get input options.
  int nexp = 40;    // Number of experiments to run.
  
  // Create a new MapReduce object, Edges.
  // Map(Edges):  Input graph from files as in link2graph_weighted.
  //              Output:  Key-values representing edges Vi->Vj with weight Wij
  //                       Key = Vi    
  //                       Value = {Vj, Wij} 
  ReadFBData readFB(narg, args);

  MPI_Barrier(MPI_COMM_WORLD);
  double tstart = MPI_Wtime();

  MapReduce *mrvert = NULL;
  MapReduce *mredge = NULL;
  uint64_t nverts;    // Number of unique non-zero vertices
  uint64_t nrawedges; // Number of edges in input files.
  uint64_t nedges;    // Number of unique edges in input files.
  readFB.run(&mrvert, &mredge, &nverts, &nrawedges, &nedges);

  MPI_Barrier(MPI_COMM_WORLD);
  double tmap = MPI_Wtime();

  // update mrvert and mredge so their vertices are unique ints from 1-N,
  // not hash values
  // if (me == 0) cout << "Renumbering graph..." << endl;
  // renumber_graph(readFB.vertexsize, mrvert, mredge);

  srand48(1l);
  if (readFB.vertexsize == 16) {
    SSSP<VERTEX16, EDGE16> sssp(mrvert, mredge);
    if (me == 0) cout << "Beginning sssp with VERTEX16" << endl;
    for (int exp = 0; exp < nexp; exp++) 
      sssp.run(exp);
  }
  else if (readFB.vertexsize == 8) {
    SSSP<VERTEX08, EDGE08> sssp(mrvert, mredge);
    if (me == 0) cout << "Beginning sssp with VERTEX08" << endl;
    for (int exp = 0; exp < nexp; exp++) 
      sssp.run(exp);
  }
  else {
    cout << "Invalid vertex size " << readFB.vertexsize << endl;
    MPI_Abort(MPI_COMM_WORLD, -1);
  }

  MPI_Barrier(MPI_COMM_WORLD);
  double tstop = MPI_Wtime();

  if (me == 0) {
    cout << "Time (Map):         " << tmap - tstart << endl;
    cout << "Time (Iterations):  " << tstop - tmap << endl;
    cout << "Time (Total):       " << tstop - tstart << endl;
  }

  MPI_Finalize();
}
