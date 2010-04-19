// Single-source shortest paths via MapReduce
// Input:   A directed graph, provided by Karl's files or matrix-market file.
// Output:  For each vertex Vi, the shortest weighted distance from a randomly
//          selected source vertex S to Vi, along with the predecessor vertex 
//          of Vi in the shortest weighted path from S to Vi.
//
// This implementation uses a BFS-like algorithm.  See sssp.txt for details.
// This implementation uses local mapreduce objects for edges and sends only
// updated distances through the global mapreduce object.

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <float.h>
#include <vector>
#include "mapreduce.h"
#include "keyvalue.h"
#include "blockmacros.hpp"
#include "read_fb_data.hpp"
#include "read_mm_data.hpp"
#include "shared.hpp"
#include "rmat.hpp"

using namespace std;
using namespace MAPREDUCE_NS;

#define MAX_NUM_EXPERIMENTS 60

#define FBFILE 0
#define MMFILE 1
#define RMAT 2

uint64_t NVtxLabeled = 0;  // Global variable (yuk!) counting number of 
                           // vertices labeled on this processor.
                           // Used as a diagnostic.
uint64_t GNVtxLabeled;     // Global variable (yuk!) counting number of
                           // vertices labeled across all processors.
                           // Used as a diagnostic.

/////////////////////////////////////////////////////////////////////////////
// Class used to pass distance information through the MapReduce system.
template <typename VERTEX, typename EDGE>
class DISTANCE {
public:
  DISTANCE(){
    memset(&(e.v.v), 0, sizeof(VERTEX));
    e.wt = FLT_MAX;
    current = true;
  };
  ~DISTANCE(){};
  EDGE e;        // Edge describing the distance of a vtx from S; 
                 // e.v is predecessor vtx; e.wt is distance from S through e.v.
  bool current;  // Flag indicating that this distance is the current state
                 // for the vtx (the currently accepted best distance).
                 // Needed so we can know when to stop (when no vtx distances
                 // change in an iteration).
  friend bool operator!=(const DISTANCE<VERTEX,EDGE>& lhs,
                         const DISTANCE<VERTEX,EDGE>& rhs) {
    if (lhs.e != rhs.e) return true;
    return false;
  };
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
  d.current = false;
  kv->add((char *) v, sizeof(VERTEX),
          (char *) &d, sizeof(DISTANCE<VERTEX, EDGE>));
//  cout << "KDDKDD add_source " << *v << endl;
}

/////////////////////////////////////////////////////////////////////////////
// move_to_new_mr:  Move KV from existing MR to new MR provided in ptr.
// Map:    Input:   KVs in exisitng MR object, new MR object in ptr.
//         Output:  No KVs in existing MR object; they are all added to new MR.
void move_to_new_mr(uint64_t itask, char *key, int keybytes, 
                    char *value, int valuebytes, 
                    KeyValue *kv, void *ptr)
{
  MapReduce *mr = (MapReduce *) ptr;
  mr->kv->add(key, keybytes, value, valuebytes);
//  cout << "KDDKDD Move_to_new_mr " << *((uint64_t *) key) << endl;
}

/////////////////////////////////////////////////////////////////////////////
// initialize_vertex_distances:  Add initial distance to all vertices.
// Map:    Input:   KV  key = Vtx ID; value = NULL
//         Output:  KV  key = Vtx ID; value = initial distance
template <typename VERTEX, typename EDGE>
void initialize_vertex_distances(uint64_t itask, char *key, int keybytes, 
                                 char *value, int valuebytes, 
                                 KeyValue *kv, void *ptr)
{
  DISTANCE<VERTEX, EDGE> d;
  kv->add(key, keybytes, (char *) &d, sizeof(DISTANCE<VERTEX, EDGE>));
//  cout << "KDDKDD initialize_vertex_distances " << *((uint64_t *) key) << " current " << d.current << endl;
}


/////////////////////////////////////////////////////////////////////////////
// pick_shortest_distances:  For each vertex, pick the best distance.
// Emit the winning distance.  Also, emit any changed vertex distances
// to mrpath (in the ptr argument).
template <typename VERTEX, typename EDGE>
void pick_shortest_distances(char *key, int keybytes, char *multivalue,
                        int nvalues, int *valuebytes, KeyValue *kv, void *ptr)
{
  MapReduce *mrpath = (MapReduce *) ptr;

// int me;
// MPI_Comm_rank(MPI_COMM_WORLD, &me);

  uint64_t total_nvalues;
  CHECK_FOR_BLOCKS(multivalue, valuebytes, nvalues, total_nvalues)

  DISTANCE<VERTEX, EDGE> shortest; // Shortest path so far to Vi.
  DISTANCE<VERTEX, EDGE> previous; // Previous best answer.
  bool modified = false;

// cout << " KDDKDD PICK_SHORTEST_DISTANCES " << *((uint64_t *) key) << " total_nvalues = " << total_nvalues << endl;
  if (total_nvalues > 1) {
    // Need to find the shortest distance to Vi.

    BEGIN_BLOCK_LOOP(multivalue, valuebytes, nvalues)

    uint64_t offset = 0;
    for (int j = 0; j < nvalues; j++) {
      DISTANCE<VERTEX, EDGE> *d = (DISTANCE<VERTEX, EDGE>*) (multivalue+offset);
// cout << "      KDDKDD COMPARE DISTANCES " << *((uint64_t *) key) << " D " << d->e.wt << " current " << d->current << " S " << shortest.e.wt << endl;
      if (d->e.wt < shortest.e.wt) {
        shortest = *d;   // shortest path so far.
      }
      if (d->current) previous = *d;
      offset += valuebytes[j];
    }

    END_BLOCK_LOOP
  }
  else {
    DISTANCE<VERTEX, EDGE> *d = (DISTANCE<VERTEX, EDGE>*) multivalue;
    shortest = *d;
    previous = *d;
  } 


  // Did we change the vertex's distance?
  if (previous != shortest) modified = true;

//cout << "    KDDKDD RESULT " << *((uint64_t *) key) << " DISTANCE " << shortest.e.wt << " PREVIOUS " << previous.e.wt <<  " MODIFIED " << modified << endl;

  // Emit vertex with updated distance back into mrvert.
  shortest.current = true;
  kv->add(key, keybytes, (char *) &shortest, sizeof(DISTANCE<VERTEX, EDGE>));
  if (shortest.e.wt < FLT_MAX) NVtxLabeled++;

  // If changes were made, emit the new distance into mrpath.
  if (modified) {
    mrpath->kv->add(key, keybytes,
                   (char *) &shortest, sizeof(DISTANCE<VERTEX, EDGE>));
// cout << "KDDKDD VTXUPDATE ADD " << *((uint64_t *) key) << " DISTANCE " << shortest.e.wt << endl;
  }

}

/////////////////////////////////////////////////////////////////////////////
// update_adjacent_distances:  For each vertex whose distance has changed, 
// emit into mrpath a possible updated distance to each of its adjacencies.
// Also emit the adjacency list back into mredge.
template <typename VERTEX, typename EDGE>
void update_adjacent_distances(char *key, int keybytes, char *multivalue,
                        int nvalues, int *valuebytes, KeyValue *kv, void *ptr)
{
  MapReduce *mrpath = (MapReduce *) ptr;
  VERTEX *vi = (VERTEX *) key;
  bool found = false;
  DISTANCE<VERTEX, EDGE> shortest;

  uint64_t total_nvalues;
  CHECK_FOR_BLOCKS(multivalue, valuebytes, nvalues, total_nvalues)

  // Find the updated distance, if any.
  // Also, re-emit the edges into mredge.
  BEGIN_BLOCK_LOOP(multivalue, valuebytes, nvalues)

  uint64_t offset = 0;
  for (int j = 0; j < nvalues; j++) {
    // Multivalues are either edges or distances.  Distances use more bytes.
    if (valuebytes[j] == sizeof(DISTANCE<VERTEX, EDGE>)) {
      // This is a distance value.
      DISTANCE<VERTEX, EDGE> *d = (DISTANCE<VERTEX, EDGE>*) (multivalue+offset);
      found = true;
      if (d->e.wt < shortest.e.wt)  shortest = *d;   // shortest path so far.
    }
    else {
      // This is an edge value.  Re-emit it into mredge.
      kv->add(key, keybytes, multivalue+offset, valuebytes[j]);
    }
    offset += valuebytes[j];
  }

  END_BLOCK_LOOP

  // If an updated distance was found, need to update distances for 
  // outward adjacencies.  Add these updates to mrpath.
// cout << "KDDKDD FOUND = " << found << endl;
  if (found) {
    BEGIN_BLOCK_LOOP(multivalue, valuebytes, nvalues)

// cout << "KDDKDD HEY nvalues = " << nvalues << endl;
    uint64_t offset = 0;
    for (int j = 0; j < nvalues; j++) {
      // Multivalues are either edges or distances.  Distances use more bytes.
      if (valuebytes[j] == sizeof(EDGE)) {
        // This is an edge value.  Emit the updated distance.
        EDGE *e = (EDGE *) (multivalue+offset);

// cout << "     KDDKDD EDGE = " << *e << " V " << *vi << " SHORTEST " << shortest.e << endl;
        // with all wt > 0, don't follow (1) loops back to predecessor or
        // (2) self-loops.
        if ((shortest.e.v != e->v) && (e->v != *vi)) {
          DISTANCE<VERTEX, EDGE> dist;
          dist.e.v = *vi;    // Predecessor of Vj along the path.
          dist.e.wt = shortest.e.wt + e->wt;
          dist.current = false;
          mrpath->kv->add((char *) &(e->v), sizeof(VERTEX),
                          (char *) &dist, sizeof(DISTANCE<VERTEX, EDGE>));
// cout << "KDDKDD ADJ ADD " << e->v << " DISTANCE " << dist.e.wt << endl;
        }
      }
      offset += valuebytes[j];
    }
    END_BLOCK_LOOP
  }
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
//  FILE *fp = (FILE *) ptr;
  DISTANCE<VERTEX,EDGE> *d = (DISTANCE<VERTEX,EDGE> *) multivalue;
  EDGE *e = &(d->e);
//  VERTEX *vi = (VERTEX *) key;

  if (nvalues > 1) {
    cout << "Sanity check failed in output_distances:  nvalues = " 
         << nvalues << endl;
    MPI_Abort(MPI_COMM_WORLD,-1);
  }
  
// FOR GREG
//  if (e->wt < FLT_MAX-1.)
//    *fp << *((VERTEX *)key) << "   " << e->wt << endl;
//  else
//    *fp << *((VERTEX *)key) << "   " << -1. << endl;

   *fp << *((VERTEX *)key) << "   " << *e << endl;

//  if (keybytes == 16)
//    fprintf(fp, "%lld %lld    %lld %lld  %ld\n",
//            vi->v[0], vi->v[1], edge->v.v[0], edge->v.v[1], edge->wt);
//  else if (keybytes == 8) 
//    fprintf(fp, "%lld    %lld   %ld\n",
//            vi->v[0], edge->v.v[0], edge->wt);
//  else
//    fprintf(fp, "Invalid vertex size %d\n", keybytes);
}


/////////////////////////////////////////////////////////////////////////////
template <typename VERTEX, typename EDGE>
class SSSP {
public:
  SSSP(int, char **, uint64_t, MapReduce *, uint64_t, MapReduce *);
  ~SSSP()
  {
    if (sourcefp) fclose(sourcefp);
    sourcemap.clear();
    sourcelist.clear();
  };

  bool run();
  bool get_next_source(VERTEX *);
  double tcompute;  // Compute time
  double twrite;    // Write time
  uint64_t tnlabeled;  // Total number of vtx labeled in all experiments.
  vector<VERTEX> sourcelist;    // For generated RMAT inputs,
                                // create a list of sources with
                                // outdegree > 1% * nverts.  Want
                                // to avoid singletons as sources
                                // as they skew the output for 
                                // different numbers of processors.
  uint64_t sourcelimit;         // 1% * nverts.
private:
  int me;
  int np;
  uint64_t nverts;
  MapReduce *mrvert;
  uint64_t nedges;
  MapReduce *mredge;
  FILE *sourcefp;               // Pointer to source-vtx file; set only on 
                                // proc 0.
  map<VERTEX, char> sourcemap;  // unique vertices previously used as sources;
                                // populated only on proc 0.
  int filetype;                 // Flag indicating source of the input 
                                // (FBFILE, MMFILE, or RMAT).
  bool write_files;             // Flag indicating whether to write files
                                // after computing SSSP.
  uint64_t counter;             // Count how many times the SSSP is run().
};

/////////////////////////////////////////////////////////////////////////////
// SSSP constructor.
// Sets options from command line.
// Builds MapReduce object with outdegree of all unique vertices.
// Modifies MapReduce edge list to include vertex outdegree in keys.

template <typename VERTEX, typename EDGE>
void get_good_sources(char *key, int keybytes, char *multivalue,
                      int nvalues, int *valuebytes, KeyValue *kv, void *ptr)
{
SSSP<VERTEX,EDGE> *sssp = (SSSP<VERTEX,EDGE> *) ptr;

  // Check whether already have enough sources.
  if (sssp->sourcelist.size() >= MAX_NUM_EXPERIMENTS) return;

  uint64_t total_nvalues;
  CHECK_FOR_BLOCKS(multivalue, valuebytes, nvalues, total_nvalues)


  // Don't include a source if it has low outdegree.
  if (total_nvalues < sssp->sourcelimit) return;

  sssp->sourcelist.push_back(*((VERTEX *) key));
}



template <typename VERTEX, typename EDGE>
SSSP<VERTEX, EDGE>::SSSP(
  int narg, 
  char **args, 
  uint64_t nverts_,
  MapReduce *mrvert_, 
  uint64_t nedges_,
  MapReduce *mredge_
) :
  tcompute(0.),
  twrite(0.),
  tnlabeled(0),
  nverts(nverts_),
  mrvert(mrvert_),
  nedges(nedges_),
  mredge(mredge_),
  sourcefp(NULL),
  filetype(FBFILE),
  write_files(false),
  counter(0)
{
  MPI_Comm_rank(MPI_COMM_WORLD, &me); 
  MPI_Comm_size(MPI_COMM_WORLD, &np); 

  // Process input options.  Open the source vertex file on proc 0.
  int iarg = 1;
  while (iarg < narg) {
    if (strcmp(args[iarg], "-s") == 0) {
      iarg++;
      if (me == 0) {
        if (filetype == MMFILE) {
          sourcefp = fopen(args[iarg], "r");
          // Skip comment lines
          char ch;
          while ((ch = getc(sourcefp)) == '%' || (ch == '#')) 
            while (getc(sourcefp) != '\n');
          // Skip header line
          while (getc(sourcefp) != '\n');
        }
        else if (filetype == FBFILE) {
          sourcefp = fopen(args[iarg], "rb");
        }
        else {
          cout << "Error:  -s is invalid option when generating RMAT; " << endl;
          MPI_Abort(MPI_COMM_WORLD, -1);
        }
        if (!sourcefp) {
          cout << "Unable to open source file " << args[iarg] << endl;
          MPI_Abort(MPI_COMM_WORLD, -1);          
        }
      }
    }
    else if (strcmp(args[iarg], "-mmfile") == 0) {
      // Indicate whether source and graph files are matrix-market format.
      // Must be specified before -s, -f and -ff arguments.
      filetype = MMFILE;
    }
    else if (strcmp(args[iarg], "-rmat") == 0) {
      // Automatically generate RMAT input.
      // Sources will be randomly selected vertices.
      filetype = RMAT;
      
      // Generate a list of valid sources.  Disallow a source vertex
      // if its outdegree is less than 1% of the number of vertices.
      // Trying to make the test results more fair when generating
      // rmat on different numbers of processors; if one run picks lots
      // of singletons for sources, it will not compare well with other
      // runs.  Ideally, total number of iterations and vertices 
      // visited should be close to the same.
      sourcelimit = MAX(1, 0.01 * nverts);
      MapReduce *mrlist = mredge->copy();  // edges are already aggregated.
      mrlist->compress(get_good_sources<VERTEX, EDGE>, this);
      delete mrlist;
    }
    else if (strcmp(args[iarg], "-o") == 0) {
      write_files = true;
    }
    iarg++;
  }
  if ((me == 0) && (filetype != RMAT) && !sourcefp) {
    cout << "Source-vertex file missing; hard-coded source will be used."
         << endl
         << "Use -s to specify source-vertex file."
         << endl
         << "(Remember to keep -f or -ff arguments last on command line.)"
         << endl;
  }
}

/////////////////////////////////////////////////////////////////////////////
// Routine to read source vertex from a file, determine whether it has been
// used as a source vertex previously, and if not, return it to the application.
// The source file is specified on the command line, with "-s sourcefile".
// It should be in Karl's format.

template <typename VERTEX, typename EDGE>
bool SSSP<VERTEX, EDGE>::get_next_source(
  VERTEX *source
)
{
  source->reset();
  if (me == 0) {
    if ((filetype == MMFILE) && sourcefp) {
      // Read source vertices from text file; keep reading until reach EOF or
      // until find a source vertex that we haven't used before.
      // Keep track of used source vtxs in a map (hash table would be better).
      int i, j;
      float v;
      while (1) {
        int nwords = fscanf(sourcefp, "%d %d %f",  &i, &j, &v);
        if (nwords == 0 or nwords == EOF) break;  // return an invalid source.
  
        source->v[0] = i;
        if (sourcemap.find(*source) == sourcemap.end()) {
          // Found a source we haven't used before
          sourcemap[*source] = '1';
          break;  // Stop reading and return this source vertex.
        }
        else
          source->reset();
      }
    }
    else if ((filetype == FBFILE) && sourcefp) {
      // Read source vertices from file; keep reading until reach EOF or
      // until find a source vertex that we haven't used before.
      // Keep track of used source vtxs in a map (hash table would be better).
      const int RECORDSIZE=32;
      uint64_t buf[4];
      while (1) {
        int nrecords = fread(buf, RECORDSIZE, 1, sourcefp);
        if (nrecords == 0) break;  // EOF; return an invalid source.
  
        if (buf[0] != 0)  { // Non-zero vertex
          *source = *((VERTEX *) &buf);
          if (sourcemap.find(*source) == sourcemap.end()) {
            // Found a source we haven't used before
            sourcemap[*source] = '1';
            break;  // Stop reading and return this source vertex.
          }
          else
            source->reset();
        }
      }
    }
    else if (filetype == RMAT) {
      //static uint64_t vv = 1;
      //if (vv <= nverts) source->v[0] = vv;
      //else source->reset();
      //vv++;
      // Randomly select a vertex in range [0..nverts-1].
      // source->v[0] = ((uint64_t) (drand48() * nverts)) + 1;
      if (counter < sourcelist.size())
        *source = sourcelist[counter];
      else 
        source->reset();
    }

    else {
      static bool firsttime = true;
      if (firsttime) {
        firsttime = false;
        if (me == 0) {
          source->v[0] = 2415554029276017988lu;   // host-to-host
          if (sizeof(VERTEX) == sizeof(VERTEX16)) // path-to-path
            source->v[1] = 5818840024467251242lu;
        }
      }
    }
  }
  
  MPI_Bcast(source, sizeof(VERTEX), MPI_BYTE, 0, MPI_COMM_WORLD);
  return(source->valid());
}


//////////////////////////////////////////////////////////////////////////////
// Routine to run the SSSP algorithm.
template <typename VERTEX, typename EDGE>
bool SSSP<VERTEX, EDGE>::run() 
{
  // Create a new MapReduce object, Paths.
  // Select a source vertex.  
  //       Processor 0 selects random number S in range [1:N] for N vertices.
  //       Processor 0 emits into Paths key-value pair [S, {-1, 0}], 
  //       signifying that vertex S has distance zero from itself, with no
  //       predecessor.

  MPI_Barrier(MPI_COMM_WORLD);
  double tstart = MPI_Wtime();

  VERTEX source;

  if (counter >= MAX_NUM_EXPERIMENTS) 
   return false;   // Limit the number of experiments for sanity's sake. :)

  if (!get_next_source(&source))
    return false;  // no unique source remains; quit execution and return.

  // Initialize vertex distances.
  mrvert->map(mrvert, initialize_vertex_distances<VERTEX, EDGE>, (void *) NULL, 0);
// cout << "    KDDKDD MRVERT->NKV " << mrvert->kv->nkv << endl;

  MapReduce *mrpath = new MapReduce(MPI_COMM_WORLD);
  mrpath->set_fpath(MYLOCALDISK); 
  mrpath->memsize = MRMEMSIZE;

  if (me == 0) cout << counter << ": BEGINNING SOURCE " << source << endl;

  mrpath->map(1, add_source<VERTEX,EDGE>, &source);
// printf("    KDDKDD MRVERT %d   MRPATH %d\n", mrvert->kv->nkv, mrpath->kv->nkv);

  //  Perform a BFS from S, editing distances as visit vertices.
  int done = 0;
  int iter = 0;
  while (!done) {
    done = 1;
 
    // Add Edges to Paths.
    // Collate Paths by vertex; collects edges and any distances 
    // computed so far.
//    mrpath->add(mredge);

    // First, determine which, if any, vertex distances have changed.
    // Add updated distances existing distances.
    mrpath->aggregate(NULL);
// printf("    KDDKDD2 MRVERT %d   MRPATH %d\n", mrvert->kv->nkv, mrpath->kv->nkv);

// cout << " KDDKDD MOVING TO MRVERT " << endl;
    mrvert->kv->append();
    mrpath->map(mrpath, move_to_new_mr, mrvert);
    mrvert->kv->complete();

// printf("    KDDKDD3 MRVERT %d   MRPATH %d\n", mrvert->kv->nkv, mrpath->kv->nkv);
    // Pick best distances.  For vertices with changed distances,
    // emit new distances into mrpath.
    uint64_t tmp_nv = 0, tmp_ne = 0;
    NVtxLabeled = 0;
    mrpath->kv->append();
    tmp_nv = mrvert->compress(pick_shortest_distances<VERTEX, EDGE>, mrpath);
    mrpath->kv->complete();
// printf("    KDDKDD4 MRVERT %d   MRPATH %d\n", mrvert->kv->nkv, mrpath->kv->nkv);

    uint64_t nchanged;
    MPI_Allreduce(&(mrpath->kv->nkv), &nchanged, 1, MPI_UNSIGNED_LONG,
                  MPI_SUM, MPI_COMM_WORLD);
    if (nchanged) {
      // Some vtxs' distances changed; 
      // need to emit new distances for adjacent vtxs.
      done = 0;
// cout << " KDDKDD MOVING TO MREDGE " << endl;
      mredge->kv->append();
      mrpath->map(mrpath, move_to_new_mr, mredge);
      mredge->kv->complete();
// printf("    KDDKDD5 MREDGE %d   MRPATH %d\n", mredge->kv->nkv, mrpath->kv->nkv);

      mrpath->kv->append();
      tmp_ne = mredge->compress(update_adjacent_distances<VERTEX, EDGE>, mrpath);
      mrpath->kv->complete();
// printf("    KDDKDD6 MREDGE %d   MRPATH %d\n", mredge->kv->nkv, mrpath->kv->nkv);
    }
    else done = 1;

    int alldone;
    MPI_Allreduce(&done, &alldone, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
    done = alldone;

    if (me == 0)
      cout << "   Iteration " << iter << " MRPath size " << mrpath->kv->nkv
           << " MRVert size " << mrvert->kv->nkv 
           << " MREdge size " << mredge->kv->nkv
           << endl;
    iter++;
  }

  MPI_Barrier(MPI_COMM_WORLD);
  double tstop = MPI_Wtime();
  tcompute += (tstop - tstart);

  MPI_Allreduce(&NVtxLabeled, &GNVtxLabeled, 1, MPI_UNSIGNED_LONG, 
                MPI_SUM, MPI_COMM_WORLD);
  if (me == 0) {
    cout << counter << ":  Source = " << source
                    << "; Iterations = " << iter 
                    << "; Num Vtx Labeled = " << GNVtxLabeled << endl;
    cout << counter << ":  Source = " << source
                    << "; Iterations = " << iter 
                    << "; Compute Time = " << (tstop-tstart) << endl;
  }
  tnlabeled += GNVtxLabeled;
  counter++;

  // Now mrpath contains one key-value per vertex Vi:
  // Key = Vi
  // Value = {Vd, D}:  the predecessor vtx Vd, the distance D from 
  //                   S to Vi, and an extraneous flag that we could
  //                   remove.

  // Output results.

  if (write_files) {
    char filename[254];
    MapReduce *mrtmp = mrvert->copy();
#define KEEP_OUTPUT
#ifdef KEEP_OUTPUT
    // Custom filenames for each source -- lots of big files.
    // All files written to NFS.
    if (sizeof(VERTEX) == sizeof(VERTEX16))
      sprintf(filename, "distance_from_%llu_%llu.%03d",
                         source.v[0], source.v[1], me);
    else
      sprintf(filename, "distance_from_%llu.%03d", source.v[0], me);

#else
    //  Single filename per processor; will be rewritten for each source, 
    //  so it is useful only for timings.
#ifdef LOCALDISK
    sprintf(filename, "%s/distance.%03d", MYLOCALDISK, me);
#else
    sprintf(filename, "distance.%03d", me);
#endif
#endif

    ofstream fp;
    fp.open(filename);
  
    mrtmp->clone();
    mrtmp->reduce(output_distances<VERTEX,EDGE>, (void *) &fp);

    fp.close();
    delete mrtmp;
  }

  MPI_Barrier(MPI_COMM_WORLD);
  twrite += (MPI_Wtime() - tstop);
   
  delete mrpath;

  return true;  // Keep going.
}

/////////////////////////////////////////////////////////////////////////////
int main(int narg, char **args)
{
  MPI_Init(&narg, &args);
  int me, np;
  int filetype = FBFILE;
  MPI_Comm_size(MPI_COMM_WORLD, &np);
  MPI_Comm_rank(MPI_COMM_WORLD, &me);

  if (np < 100) greetings();
#ifdef LOCALDISK
  // Test the file system for writing; some nodes seem to have 
  // trouble writing to local disk.
  test_local_disks();
#endif

  for (int i = 0; i < narg; i++) 
    if (strcmp(args[i], "-mmfile") == 0) {
      filetype = MMFILE;
      break;
    } else if (strcmp(args[i], "-rmat") == 0) {
      filetype = RMAT;
      break;
    }



  // Create a new MapReduce object, Edges.
  // Map(Edges):  Input graph from files as in link2graph_weighted.
  //              Output:  Key-values representing edges Vi->Vj with weight Wij
  //                       Key = Vi    
  //                       Value = {Vj, Wij} 

  double tstart;

  MapReduce *mrvert = NULL;
  MapReduce *mredge = NULL;
  uint64_t nverts;    // Number of unique non-zero vertices
  uint64_t nrawedges; // Number of edges in input files.
  uint64_t nedges;    // Number of unique edges in input files.
  int vertexsize;
  if (filetype == FBFILE) { // FB files
    ReadFBData readFB(narg, args, true);

    MPI_Barrier(MPI_COMM_WORLD);
    tstart = MPI_Wtime();

    readFB.run(&mrvert, &mredge, &nverts, &nrawedges, &nedges);
    vertexsize = readFB.vertexsize;
  }
  else if (filetype == MMFILE) { // MM file
    ReadMMData readMM(narg, args, true);

    MPI_Barrier(MPI_COMM_WORLD);
    tstart = MPI_Wtime();

    readMM.run(&mrvert, &mredge, &nverts, &nrawedges, &nedges);
    vertexsize = readMM.vertexsize;
  }
  else { // Generate RMAT
    GenerateRMAT rmat(narg, args);

    MPI_Barrier(MPI_COMM_WORLD);
    tstart = MPI_Wtime();

    rmat.run(&mrvert, &mredge, &nverts, &nrawedges, &nedges);
    vertexsize = 8;
  }

  // Aggregate mredge and mrvert by key.  No need to convert yet.
  // These then are, essentially, local mapreduce objects.
  mredge->aggregate(NULL);
  mrvert->aggregate(NULL);

  MPI_Barrier(MPI_COMM_WORLD);
  double tmap = MPI_Wtime();

  srand48(1l);
  if (vertexsize == 16) {
    SSSP<VERTEX16, EDGE16> sssp(narg, args, nverts, mrvert, nedges, mredge);
    if (me == 0) cout << "Beginning sssp with 16-byte keys." << endl;
    while (sssp.run());
    if (me == 0) {
      cout << "Experiment Time (Compute): " << sssp.tcompute << endl;
      cout << "Experiment Time (Write):   " << sssp.twrite << endl;
      cout << "Total # Vtx Labeled:       " << sssp.tnlabeled << endl;
    }
  }
  else if (vertexsize == 8) {
    SSSP<VERTEX08, EDGE08> sssp(narg, args, nverts, mrvert, nedges, mredge);
    if (me == 0) cout << "Beginning sssp with 8-byte keys." << endl;
    while (sssp.run());
    if (me == 0) {
      cout << "Experiment Time (Compute): " << sssp.tcompute << endl;
      cout << "Experiment Time (Write):   " << sssp.twrite << endl;
      cout << "Total # Vtx Labeled:       " << sssp.tnlabeled << endl;
    }
  }
  else {
    cout << "Invalid vertex size " << vertexsize << endl;
    MPI_Abort(MPI_COMM_WORLD, -1);
  }

  delete mrvert;
  delete mredge;

  MPI_Barrier(MPI_COMM_WORLD);
  double tstop = MPI_Wtime();

  if (me == 0) {
    cout << "Time (Map):         " << tmap - tstart << endl;
    cout << "Time (Experiments): " << tstop - tmap << endl;
    cout << "Time (Total):       " << tstop - tstart << endl;
  }

  MPI_Finalize();
}
