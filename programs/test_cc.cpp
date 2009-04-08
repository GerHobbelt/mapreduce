// MatVec via MapReduce
// Karen Devine and Steve Plimpton, Sandia Natl Labs
// Nov 2008
//
// Identify connected components in a graph via MapReduce
// algorithm due to Jonathan Cohen.
// The algorithm treats all edges as undirected edges.
// 
// Syntax: concomp switch args switch args ...
// switches:
//   -r N = define N as root vertex, compute all distances from it
//   -o file = output to this file, else no output except screen summary
//   -t style params = input from a test problem
//      style params = ring N = 1d ring with N vertices
//      style params = 2d Nx Ny = 2d grid with Nx by Ny vertices
//      style params = 3d Nx Ny Nz = 3d grid with Nx by Ny by Nz vertices
//      style params = rmat N Nz a b c d frac seed
//        generate an RMAT matrix with 2^N rows, Nz non-zeroes per row,
//        a,b,c,d = RMAT params, frac = RMAT randomize param, seed = RNG seed
//   -f file1 file2 ... = input from list of files containing sparse matrix
//   -p 0/1 = turn random permutation of input data off/on (default = off)

#include "mpi.h"
#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "mapreduce.h"
#include "keyvalue.h"
#include "random_mars.h"
#include "assert.h"

#include "test_cc_common.h"

#include <map>

using namespace std;
using namespace MAPREDUCE_NS;

void reduce1(char *, int, char *, int, int *, KeyValue *, void *);
void reduce2(char *, int, char *, int, int *, KeyValue *, void *);
void reduce3(char *, int, char *, int, int *, KeyValue *, void *);
void reduce4(char *, int, char *, int, int *, KeyValue *, void *);
void output_vtxstats(char *, int, char *, int, int *, KeyValue *, void *);
void output_vtxdetail(char *, int, char *, int, int *, KeyValue *, void *);
void output_zonestats(char *, int, char *, int, int *, KeyValue *, void *);
void output_testdistance(char *, int, char *, int, int *, KeyValue *, void *);
int sort(char *, int, char *, int);

/* ---------------------------------------------------------------------- */

typedef struct {         // vertex state = zone ID, distance from zone seed
  VERTEX vtx;            // vertex ID (redundant?)
  int zone;              // zone this vertex is in = vertex ID of zone root
  int dist;              // distance of this vertex from root
} STATE;

typedef struct {
  float sortdist;        // sorting distance of this edge in KMV
  EDGE e;       
  STATE si;
  STATE sj;
} REDUCE2VALUE;

typedef struct {
  EDGE e;
  STATE s;
} REDUCE3VALUE;

struct SORTINFO {
  char *multivalue;
  int *offsets;
};

SORTINFO sortinfo;    // needed to give qsort() compare fn access to KMV


/* ---------------------------------------------------------------------- */

int main(int narg, char **args)
{
  MPI_Init(&narg,&args);

  int me,nprocs;
  MPI_Comm_rank(MPI_COMM_WORLD,&me);
  MPI_Comm_size(MPI_COMM_WORLD,&nprocs);

  int nVtx, nCC;  // Number of vertices and connected components

  CC cc;
  cc.me = me;
  cc.nprocs = nprocs;

  // parse command-line args

  cc.root = -1;
  cc.input = NOINPUT;
  cc.nfiles = 0;
  cc.permute = 0;
  cc.infiles = NULL;
  cc.outfile = NULL;
  cc.nvtx = 0;

  int iarg = 1;
  while (iarg < narg) {
    if (strcmp(args[iarg],"-r") == 0) {
      if (iarg+2 > narg) error(me,"Bad arguments");
      cc.root = atoi(args[iarg+1]);
      iarg += 2;

    } else if (strcmp(args[iarg],"-o") == 0) {
      if (iarg+2 > narg) error(me,"Bad arguments");
      int n = strlen(args[iarg+1]) + 1;
      cc.outfile = new char[n];
      strcpy(cc.outfile,args[iarg+1]);
      iarg += 2;

    } else if (strcmp(args[iarg],"-t") == 0) {
      if (iarg+2 > narg) error(me,"Bad arguments");
      if (strcmp(args[iarg+1],"ring") == 0) {
        if (iarg+3 > narg) error(me,"Bad arguments");
        cc.input = RING;
        cc.nring = atoi(args[iarg+2]); 
        cc.nvtx = cc.nring;
        iarg += 3;
      } else if (strcmp(args[iarg+1],"grid2d") == 0) {
        if (iarg+4 > narg) error(me,"Bad arguments");
        cc.input = GRID2D;
        cc.nx = atoi(args[iarg+2]); 
        cc.ny = atoi(args[iarg+3]); 
        cc.nvtx = cc.nx * cc.ny;
        iarg += 4;
      } else if (strcmp(args[iarg+1],"grid3d") == 0) {
        if (iarg+5 > narg) error(me,"Bad arguments");
        cc.input = GRID3D;
        cc.nx = atoi(args[iarg+2]); 
        cc.ny = atoi(args[iarg+3]); 
        cc.nz = atoi(args[iarg+4]); 
        cc.nvtx = cc.nx * cc.ny * cc.nz;
        iarg += 5;
      } else if (strcmp(args[iarg+1],"rmat") == 0) {
        if (iarg+10 > narg) error(me,"Bad arguments");
        cc.input = RMAT;
        cc.nlevels = atoi(args[iarg+2]); 
        cc.nnonzero = atoi(args[iarg+3]); 
        cc.a = atof(args[iarg+4]); 
        cc.b = atof(args[iarg+5]); 
        cc.c = atof(args[iarg+6]); 
        cc.d = atof(args[iarg+7]); 
        cc.fraction = atof(args[iarg+8]); 
        cc.seed = atoi(args[iarg+9]); 
	cc.random = new RanMars(cc.seed+me);
        cc.nvtx = 1 << cc.nlevels;
        iarg += 10;
      } else error(me,"Bad arguments");

    } else if (strcmp(args[iarg],"-f") == 0) {
      cc.input = FILES;
      iarg++;
      while (iarg < narg) {
        if (args[iarg][0] == '-') break;
        cc.infiles = 
          (char **) realloc(cc.infiles,(cc.nfiles+1)*sizeof(char *));
        cc.infiles[cc.nfiles] = args[iarg];
        cc.nfiles++;
        iarg++;
      }
    } else if (strcmp(args[iarg],"-p") == 0) {
      if (iarg+2 > narg) error(me,"Bad arguments");
      cc.permute = atoi(args[iarg+1]);
      iarg += 2;
    } else error(me,"Bad arguments");
  }

  if (cc.input == NOINPUT) error(me,"No input specified");

  // find connected components via MapReduce

  MapReduce *mr = new MapReduce(MPI_COMM_WORLD);
  mr->verbosity = 0;

  if (cc.input == FILES) {
    mr->map(nprocs,cc.nfiles,cc.infiles,'\n',80,&file_map1,&cc);
    int tmp = cc.nvtx;
    MPI_Allreduce(&tmp, &cc.nvtx, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

  } else if (cc.input == RMAT) {
    int ntotal = (1 << cc.nlevels) * cc.nnonzero;
    int nremain = ntotal;
    while (nremain) {
      cc.ngenerate = nremain/nprocs;
      if (me < nremain % nprocs) cc.ngenerate++;
      mr->verbosity = 1;
      mr->map(nprocs,&rmat_generate,&cc,1);
      int nunique = mr->collate(NULL);
      if (nunique == ntotal) break;
      mr->reduce(&rmat_cull,&cc);
      nremain = ntotal - nunique;
    }
    mr->reduce(&rmat_map1,&cc);
    mr->verbosity = 0;

  } else if (cc.input == RING)
    mr->map(nprocs,&ring_map1,&cc);
  else if (cc.input == GRID2D)
    mr->map(nprocs,&grid2d_map1,&cc);
  else if (cc.input == GRID3D)
    mr->map(nprocs,&grid3d_map1,&cc);

  // need to mark root vertex if specified, relabel with ID = 0 ??

  if (me == 0) {printf("Input complete\n"); fflush(stdout);}

  MPI_Barrier(MPI_COMM_WORLD);
  double tstart = MPI_Wtime();

  nVtx = mr->collate(NULL);
  int numSingletons = cc.nvtx - nVtx;  // Num vertices with degree zero.

  mr->reduce(&reduce1,&cc);

  int iter = 0;

  if (me == 0) {printf("Beginning iterations\n"); fflush(stdout);}
  while (1) {
    mr->collate(NULL);
    mr->reduce(&reduce2,&cc);

    nCC = mr->collate(NULL);
    iter++;
    if (me == 0) printf("Iteration %d Number of Components = %d\n", iter, nCC);

    cc.doneflag = 1;
    mr->reduce(&reduce3,&cc);

    int alldone;
    MPI_Allreduce(&cc.doneflag,&alldone,1,MPI_INT,MPI_MIN,MPI_COMM_WORLD);
    if (alldone) break;

    mr->collate(NULL);
    mr->reduce(&reduce4,&cc);
  }

  MPI_Barrier(MPI_COMM_WORLD);
  double tstop = MPI_Wtime();

  // Output some results.
  // Data in mr currently is keyed by vertex v
  // multivalue includes every edge containing v, as well as v's state.

  // Compute min/max/avg distances from seed vertices.

  cc.distStats.min = 0;  // Smallest distance is from seed to itself.
  cc.distStats.max = 0;
  cc.distStats.sum = 0;
  cc.distStats.cnt = 0;
  for (int i = 0; i < 10; i++) cc.distStats.histo[i] = 0;

  mr->collate(NULL);  // Collate wasn't done after reduce3 when alldone.
  mr->reduce(&output_vtxstats, &cc);
  mr->collate(NULL);

  STATS gDist;    // global vertex stats
  gDist.min = 0;
  MPI_Allreduce(&cc.distStats.max, &gDist.max, 1, MPI_INT, MPI_MAX,
                MPI_COMM_WORLD);
  MPI_Allreduce(&cc.distStats.sum, &gDist.sum, 1, MPI_INT, MPI_SUM,
                MPI_COMM_WORLD);
  MPI_Allreduce(&cc.distStats.cnt, &gDist.cnt, 1, MPI_INT, MPI_SUM,
                MPI_COMM_WORLD);
  MPI_Allreduce(&cc.distStats.histo, &gDist.histo, 10, MPI_INT, MPI_SUM,
                MPI_COMM_WORLD);

  // Add in degree-zero vertices
  gDist.cnt += numSingletons;
  gDist.histo[0] += numSingletons;

  assert(gDist.cnt == cc.nvtx);
  assert(gDist.min == 0);
  assert(gDist.max < nVtx);

  // Write all vertices with state info to a file.
  // This operation requires all vertices to be on one processor.  
  // Don't do this for big data!

  if (cc.outfile) {
    mr->reduce(&output_vtxdetail, &cc);
    mr->collate(NULL);
  }

  // Compute min/max/avg connected-component size.

  cc.sizeStats.min = (numSingletons ? 1 : nVtx); 
  cc.sizeStats.max = 1;
  cc.sizeStats.sum = 0;
  cc.sizeStats.cnt = 0;
  for (int i = 0; i < 10; i++) cc.sizeStats.histo[i] = 0;

  mr->reduce(&output_zonestats, &cc);

  STATS gCCSize;    // global CC stats
  MPI_Allreduce(&cc.sizeStats.min, &gCCSize.min, 1, MPI_INT, MPI_MIN,
                MPI_COMM_WORLD);
  MPI_Allreduce(&cc.sizeStats.max, &gCCSize.max, 1, MPI_INT, MPI_MAX,
                MPI_COMM_WORLD);
  MPI_Allreduce(&cc.sizeStats.sum, &gCCSize.sum, 1, MPI_INT, MPI_SUM,
                MPI_COMM_WORLD);
  MPI_Allreduce(&cc.sizeStats.cnt, &gCCSize.cnt, 1, MPI_INT, MPI_SUM,
                MPI_COMM_WORLD);
  MPI_Allreduce(&cc.sizeStats.histo, &gCCSize.histo, 10, MPI_INT, MPI_SUM,
                MPI_COMM_WORLD);

  // Add in degree-zero vertices
  gCCSize.sum += numSingletons;
  gCCSize.cnt += numSingletons;
  gCCSize.histo[0] += numSingletons;

  assert(gCCSize.cnt == nCC+numSingletons);
  assert(gCCSize.max <= nVtx);

  if (me == 0) {
    printf("Number of iterations = %d\n", iter);
    printf("Number of vertices = %d\n", cc.nvtx);
    printf("Number of Connected Components = %d\n", gCCSize.cnt);
    printf("Number of Singleton Vertices = %d\n", numSingletons);
    printf("Distance from Seed (Min, Max, Avg):  %d  %d  %f\n", 
           gDist.min, gDist.max, (float) gDist.sum / (float) cc.nvtx);
    printf("Distance Histogram:  ");
    for (int i = 0; i < 10; i++) printf("%d ", gDist.histo[i]);
    printf("\n");
    printf("Size of Connected Components (Min, Max, Avg):  %d  %d  %f\n", 
           gCCSize.min, gCCSize.max, (float) gCCSize.sum / (float) gCCSize.cnt);
    printf("Size Histogram:  ");
    for (int i = 0; i < 10; i++) printf("%d ", gCCSize.histo[i]);
    printf("\n");
  }

  // accuracy check on vertex distances if test problem was used

  if (cc.input != FILES && cc.input != RMAT) {
    mr->reduce(&output_testdistance, &cc);
    int badflag;
    MPI_Allreduce(&cc.badflag, &badflag, 1, MPI_INT, MPI_SUM,
		  MPI_COMM_WORLD);
    if (me == 0) printf("# of Vertices with a Bad distance = %d\n",badflag);
  }

  // final timing

  if (me == 0)
    printf("Time to compute CC on %d procs = %g (secs)\n",
	   nprocs,tstop-tstart);

  // clean up

  delete mr;
  delete [] cc.outfile;
  free(cc.infiles);

  MPI_Finalize();
}


/* ----------------------------------------------------------------------
   reduce1 function
   Input:  One KMV per vertex; MV lists all edges incident to the vertex.
   Output:  One KV per edge: key = edge e_ij; value = initial state_i
   Initial state of a vertex k is zone=k, dist=0.
------------------------------------------------------------------------- */
#ifdef NOISY
#define PRINT_REDUCE1(v, e, s) \
    printf("reduce1:  Vertex %d  Key (%d %d) Value (%d %d %d)\n", \
            v, e->vi, e->vj, s.vtx, s.zone, s.dist);  
#define HELLO_REDUCE1(v, n) \
    printf("HELLO REDUCE1 Vertex %d Nvalues %d\n", *v, nvalues);
#else
#define PRINT_REDUCE1(v, e, s)
#define HELLO_REDUCE1(v, n)
#endif

void reduce1(char *key, int keybytes, char *multivalue,
              int nvalues, int *valuebytes, KeyValue *kv, void *ptr) 
{
  VERTEX *v = (VERTEX *) key;
  EDGE *e = (EDGE *) multivalue;
  STATE s;

  HELLO_REDUCE1(v, nvalues);

  s.vtx = *v;
  s.zone = *v;
  s.dist = 0;
  for (int n = 0; n < nvalues; n++, e++) {
    kv->add((char *) e, sizeof(EDGE), (char *) &s, sizeof(STATE));
    PRINT_REDUCE1(*v, e, s);
  }
}

/* ----------------------------------------------------------------------
   reduce2 function
   Input:  One KMV per edge; MV lists state_i, state_j of v_i, v_j in edge e_ij.
   Output:  Up to three KV based on state_i, state_j of v_i, v_j in edge e_ij.
------------------------------------------------------------------------- */
#ifdef NOISY
#define PRINT_REDUCE2(key, rout) \
    printf("reduce2:  Key %d Value [%f (%d %d) (%d %d %d) (%d %d %d)]\n", \
           key, rout.sortdist, rout.e.vi, rout.e.vj, \
           rout.si.vtx, rout.si.zone, rout.si.dist, \
           rout.sj.vtx, rout.sj.zone, rout.sj.dist);  
#define HELLO_REDUCE2(key, nvalues) \
   printf("HELLO REDUCE2  (%d %d) nvalues %d\n", \
          ((EDGE *)key)->vi, ((EDGE *)key)->vj, nvalues);
#else
#define PRINT_REDUCE2(key, rout) 
#define HELLO_REDUCE2(key, nvalues) 
#endif


void reduce2(char *key, int keybytes, char *multivalue,
              int nvalues, int *valuebytes, KeyValue *kv, void *ptr) 
{
  HELLO_REDUCE2(key, nvalues);

  assert(nvalues == 2);  // For graphs, each edge has two vertices, so 
                         // the multivalue should have at most two states.

  STATE *si = (STATE *) multivalue; 
  STATE *sj = (STATE *) (multivalue + valuebytes[0]);
  
  float dmin = MIN(si->dist, sj->dist);
  float dmax = MAX(si->dist, sj->dist);
  int zmax = MAX(si->zone, sj->zone);

  REDUCE2VALUE rout;

  rout.e = *((EDGE *) key);
  // Order of states s_i and s_j in multivalue is not necessarily the
  // same as in edge; make sure we get them correctly ordered here.
  if (rout.e.vi != si->vtx) {
    STATE *tmp = si;
    si = sj;
    sj = tmp;
  }
  rout.si = *si;
  rout.sj = *sj;

  if (si->zone == sj->zone) {
    rout.sortdist = dmin;
    kv->add((char *) &(si->zone), sizeof(si->zone), 
            (char *) &rout, sizeof(REDUCE2VALUE));
    PRINT_REDUCE2(si->zone, rout);

    rout.sortdist = -(dmax + (dmax - dmin) / (dmax + 1));
    kv->add((char *) &(si->zone), sizeof(si->zone), 
            (char *) &rout, sizeof(REDUCE2VALUE));
    PRINT_REDUCE2(si->zone, rout);
  }
  else {
#define KDD_BUGFIX
#ifdef KDD_BUGFIX
    rout.sortdist = si->dist;  // KDD_BUGFIX
#else
    rout.sortdist = dmin;
#endif
    kv->add((char *) &(si->zone), sizeof(si->zone), 
            (char *) &rout, sizeof(REDUCE2VALUE));
    PRINT_REDUCE2(si->zone, rout);

#ifdef KDD_BUGFIX
    rout.sortdist = sj->dist;  // KDD_BUGFIX
#else
    rout.sortdist = dmin;
#endif
    kv->add((char *) &(sj->zone), sizeof(sj->zone), 
            (char *) &rout, sizeof(REDUCE2VALUE));
    PRINT_REDUCE2(sj->zone, rout);

    rout.sortdist = -BIGVAL;
    kv->add((char *) &zmax, sizeof(zmax), 
            (char *) &rout, sizeof(REDUCE2VALUE));
    PRINT_REDUCE2(zmax, rout);
  }
}

/* ----------------------------------------------------------------------
   comparison function for qsort() in reduce3
   used to compare Bi and Bj sorting criteria of 2 edge values
------------------------------------------------------------------------- */

int sort_compare(const void *iptr, const void *jptr)
{
  REDUCE2VALUE *valuei, *valuej;

  int i = *((int *) iptr);
  valuei = (REDUCE2VALUE *) &sortinfo.multivalue[sortinfo.offsets[i]];
  float *bi = &valuei->sortdist;
  int j = *((int *) jptr);
  valuej = (REDUCE2VALUE *) &sortinfo.multivalue[sortinfo.offsets[j]];
  float *bj = &valuej->sortdist;

  if (*bi < *bj) return -1;
  else if (*bi > *bj) return 1;
#undef STABLE_SORT  // Stable sort is needed only to make parallel runs match
                    // serially runs identically.  It is not needed for
                    // correctness of the algorithm, so it is not the default.
#ifndef STABLE_SORT
  return 0;
#else
  else {
    // Criteria for breaking ties -- look at edge.
    EDGE *ei = &valuei->e;
    EDGE *ej = &valuej->e;
    if (ei->vi < ej->vi) return -1;
    else if (ei->vi > ej->vi) return 1;
    else return (ei->vj < ej->vj ? -1 : 1);
  }
#endif
}

/* ----------------------------------------------------------------------
   comparison function for reduce3 ehash map
   used to compare Ei and Ej edges by comparing vertices in edge
------------------------------------------------------------------------- */

struct key_compare {
  bool operator()(EDGE ei, EDGE ej) {
    if (ei.vi < ej.vi) return true;
    else if (ei.vi == ej.vi && ei.vj < ej.vj) return true;
    return false;
  }
};

/* ----------------------------------------------------------------------
   reduce3 function
   input KMV = all edges in zone, stored twice with different D values
   one value in multi-value = B, Eij, Si, Sj
     B = sorting criterion, Eij = (Vi,Vj), Si = (Zi,dist), Sj = (Zj,dist)
   output KV = vertices with updated state
     key = Vi, value = (Eij,Si)
------------------------------------------------------------------------- */
#ifdef NOISY
#define PRINT_REDUCE3(key, value) \
    printf("reduce3:  Key %d Value [(%d %d) (%d %d %d)]\n", \
           key, value.e.vi, value.e.vj, \
           value.s.vtx, value.s.zone, value.s.dist)
#define HELLO_REDUCE3(key, nvalues) \
   printf("HELLO REDUCE3  %d  nvalues %d\n", key,  nvalues)
#else
#define PRINT_REDUCE3(key, value) 
#define HELLO_REDUCE3(key, nvalues) 
#endif


void reduce3(char *key, int keybytes, char *multivalue,
              int nvalues, int *valuebytes, KeyValue *kv, void *ptr) 
{
  CC *cc = (CC *) ptr;
  
  // create hash table for states of all vertices of zone edges
  // key = vertex ID
  // value = vertex state = Si = (Zi,dist) where Zi = zone ID
  // all copies of a vertex state should be identical, so hash it once
  // also create offsets array at same time
  // offsets[i] = offset into multivalue for start of Ith value

  HELLO_REDUCE3((*((int *)key)), nvalues);

  REDUCE2VALUE *value;
  map<int,STATE> vhash;
  int *offsets = new int[nvalues];

  int offset = 0;
  for (int i = 0; i < nvalues; i++) {
    value = (REDUCE2VALUE *) &multivalue[offset];
    int vi = value->e.vi;
    int vj = value->e.vj;
    if (vhash.find(vi) == vhash.end())
      vhash.insert(make_pair(vi,value->si));
    if (vhash.find(vj) == vhash.end()) 
      vhash.insert(make_pair(vj,value->sj));
    offsets[i] = offset;
    offset += valuebytes[i];
  }

  // sort zone multi-values by B = sorting criterion
  // order = index vector of sorted order
  // store pointers to KMV data in sortinfo so sort_compare fn can access it

  sortinfo.multivalue = multivalue;
  sortinfo.offsets = offsets;

  int *order = new int[nvalues];
  for (int i = 0; i < nvalues; i++) order[i] = i;

  qsort(order,nvalues,sizeof(int),sort_compare);

  // sanity check on sorted ordering

  REDUCE2VALUE *ivalue,*jvalue;

  for (int i = 0; i < nvalues-1; i++) {
    ivalue = (REDUCE2VALUE *) &multivalue[offsets[order[i]]];
    jvalue = (REDUCE2VALUE *) &multivalue[offsets[order[i+1]]];
    if (ivalue->sortdist > jvalue->sortdist) {
      int izone = *((int *) key);
      char str[32];
      sprintf(str,"Bad sorted order for zone %d\n",izone);
      errorone(str);
    }
  }

  // loop over edges of zone in sorted order
  // extract Si and Sj for Eij from hash table
  // Zmin = min(Zi,Zj)
  // Dmin = lowest dist of vertex whose S has Zmin
  // if Si or Sj is already (Zmin,Dmin), don't change it
  // if Si or Sj is not (Zmin,Dmin), change it to Snew = (Zmin,Dmin+1)
  // if Si or Sj changes, put it back in hash table and set CC doneflag = 0

  map<int,STATE>::iterator vloc, viloc, vjloc;
  int vi,vj,zmin,dmin;
  STATE si,sj;

  for (int i = 0; i < nvalues; i++) {
    value = (REDUCE2VALUE *) &multivalue[offsets[order[i]]];
    vi = value->e.vi;
    vj = value->e.vj;
    viloc = vhash.find(vi);
    si = viloc->second;
    vjloc = vhash.find(vj);
    sj = vjloc->second;
    zmin = MIN(si.zone,sj.zone);
    dmin = IBIGVAL;
    if (si.zone == zmin) dmin = si.dist;
    if (sj.zone == zmin) dmin = MIN(dmin,sj.dist);
    if (si.zone != zmin || si.dist > dmin+1) {
      si.zone = zmin;
      si.dist = dmin+1;
      viloc->second = si;
      cc->doneflag = 0;
    }
    if (sj.zone != zmin || sj.dist > dmin+1) {
      sj.zone = zmin;
      sj.dist = dmin+1;
      vjloc->second = sj;
      cc->doneflag = 0;
    }
  }

  // emit 2 KV per unique edge in MV
  // Key = Vi, Val = Eij Si
  // Key = Vj, Val = Eij Sj
  // Si,Sj are extracted from vertex state hash table
  // use edge hash table to identify unique edges
  // skip edge if already in hash, else insert edge in hash and emit KVs

  map<pair<int,int>,int> ehash;
  REDUCE3VALUE value3;

  for (int i = 0; i < nvalues; i++) {
    value = (REDUCE2VALUE *) &multivalue[offsets[order[i]]];
    vi = value->e.vi;
    vj = value->e.vj;
    if (ehash.find(make_pair(vi,vj)) == ehash.end()) {
      ehash.insert(make_pair(make_pair(vi,vj),0));
      vloc = vhash.find(vi);
      si = vloc->second;
      vloc = vhash.find(vj);
      sj = vloc->second;

      value3.e = value->e;
      value3.s = si;
      kv->add((char *) &vi,sizeof(VERTEX), 
              (char *) &value3,sizeof(REDUCE3VALUE));
      PRINT_REDUCE3(vi, value3);

      value3.s = sj;
      kv->add((char *) &vj,sizeof(VERTEX), 
              (char *) &value3,sizeof(REDUCE3VALUE));
      PRINT_REDUCE3(vj, value3);
    }
  }

  // delete temporary storage

  delete [] order;
  delete [] offsets;
}

/* ----------------------------------------------------------------------
   reduce4 function
   Input:  One KMV per vertex; MV is (e_ij, state_i) for all edges incident
           to v_i.
   Output:  One KV for each edge incident to v_i, with updated state_i.
           key = e_ij; value = new state_i
------------------------------------------------------------------------- */

#ifdef NOISY
#define PRINT_REDUCE4(v, e, s) \
    printf("reduce4:  Vertex %d  Key (%d %d) Value (%d %d %d)\n", \
            v, e.vi, e.vj, s.vtx, s.zone, s.dist);  
#define HELLO_REDUCE4(key, nvalues) \
    printf("HELLO REDUCE4 Vertex %d Nvalues %d\n", *((VERTEX *)key), nvalues);
#else
#define PRINT_REDUCE4(v, e, s)
#define HELLO_REDUCE4(key, nvalues)
#endif

void reduce4(char *key, int keybytes, char *multivalue,
              int nvalues, int *valuebytes, KeyValue *kv, void *ptr) 
{
  HELLO_REDUCE4(key, nvalues);

  // Compute best state for this vertex.
  // Best state has min zone, then min dist.
  REDUCE3VALUE *r = (REDUCE3VALUE *) multivalue;
  STATE best;

  best.vtx  = *((VERTEX *) key);
  best.zone = r->s.zone;
  best.dist = r->s.dist;

  r++;  // Processed 0th entry already.  Move on.
  for (int n = 1; n < nvalues; n++, r++) {
    if (r->s.zone < best.zone) {
      best.zone = r->s.zone;
      best.dist = r->s.dist;
    }
    else if (r->s.zone == best.zone)
      best.dist = MIN(r->s.dist, best.dist);
  }

  // Emit edges with updated state for vertex key.
  r = (REDUCE3VALUE *) multivalue;
  map<pair<int,int>,int> ehash;

  for (int n = 0; n < nvalues; n++, r++) {
    // Emit for unique edges -- no duplicates.  
    // KDD:  Replace this map with a true hash table for better performance.
    if (ehash.find(make_pair(r->e.vi, r->e.vj)) == ehash.end()) {
      ehash.insert(make_pair(make_pair(r->e.vi, r->e.vj),0));
      kv->add((char *) &(r->e), sizeof(EDGE), (char *) &best, sizeof(STATE));
      PRINT_REDUCE4(*((VERTEX *) key), r->e, best);
    }
  }
}

/* ----------------------------------------------------------------------
   output_vtxstats function
   Input:  One KMV per vertex; MV is (e_ij, state_i) for all edges incident
           to v_i.
   Output: Two options:  
           if (cc.outfile) Emit (0, state_i) to allow printing of vertex info
           else Emit (zone, state_i) to allow collecting zone stats.
------------------------------------------------------------------------- */

void output_vtxstats(char *key, int keybytes, char *multivalue,
                     int nvalues, int *valuebytes, KeyValue *kv, void *ptr) 
{
  CC *cc = (CC *) ptr;
  REDUCE3VALUE *mv = (REDUCE3VALUE *) multivalue;

  // Gather some stats:  Min, Max and Avg distances.

  // Since we have presumably reached convergence, state_i is the same in
  // all multivalue entries.  We need to check only one.

  if (mv->s.dist > cc->distStats.max) cc->distStats.max = mv->s.dist;
  cc->distStats.sum += mv->s.dist;
  cc->distStats.cnt++;
  int bin = (10 * mv->s.dist) / cc->nvtx;
  cc->distStats.histo[bin]++;

  if (cc->outfile) {
    // Emit for gather to one processor for file output.
    const int zero=0;
    kv->add((char *) &zero, sizeof(zero), (char *) &(mv->s), sizeof(STATE));
  }
  else {
    // Emit for reorg by zones to collect zone stats.
    kv->add((char *) &(mv->s.zone), sizeof(mv->s.zone), 
            (char *) &(mv->s), sizeof(STATE));
  }
}

/* ----------------------------------------------------------------------
   output_vtxdetail function
   Input:  One KMV; key = 0; MV is state_i for all vertices v_i.
   Output: Emit (zone, state_i) to allow collecting zone stats.
------------------------------------------------------------------------- */

void output_vtxdetail(char *key, int keybytes, char *multivalue,
                     int nvalues, int *valuebytes, KeyValue *kv, void *ptr) 
{
  FILE *fp = fopen(((CC*)ptr)->outfile, "w");
  STATE *s = (STATE *) multivalue;
  fprintf(fp, "Vtx\tZone\tDistance\n");
  for (int i = 0; i < nvalues; i++, s++) {
    fprintf(fp, "%d\t%d\t%d\n", s->vtx, s->zone, s->dist);

    // Emit for reorg by zones to collect zone stats.
    kv->add((char *) &(s->zone), sizeof(s->zone), (char *) s, sizeof(STATE));
  }
  fclose(fp);
}

/* ----------------------------------------------------------------------
   output_zonestats function
   Input:  One KMV per zone; MV is (state_i) for all vertices v_i in zone.
   Output: None yet.
------------------------------------------------------------------------- */

void output_zonestats(char *key, int keybytes, char *multivalue,
                      int nvalues, int *valuebytes, KeyValue *kv, void *ptr) 
{
  CC *cc = (CC *) ptr;
  
  // Compute min/max/avg component size.
  if (nvalues > cc->sizeStats.max) cc->sizeStats.max = nvalues;
  if (nvalues < cc->sizeStats.min) cc->sizeStats.min = nvalues;
  cc->sizeStats.sum += nvalues;
  cc->sizeStats.cnt++;
  int bin = (10 * nvalues) / cc->nvtx;
  if (bin == 10) bin--;
  cc->sizeStats.histo[bin]++;
}

/* ----------------------------------------------------------------------
   output_testdistance function
   Input:  One KMV per zone; MV is (state_i) for all vertices v_i in zone.
   Output: None.
------------------------------------------------------------------------- */

void output_testdistance(char *key, int keybytes, char *multivalue,
			 int nvalues, int *valuebytes, KeyValue *kv,
			 void *ptr) 
{
  CC *cc = (CC *) ptr;

  // check distance in state of each vertex from seed
  // check against what distance should be based on vertex ID
  // know correct answer for ring, grid2d, grid3d

  int id,correct;
  STATE *s = (STATE *) multivalue;

  cc->badflag = 0;
  for (int i = 0; i < nvalues; i++, s++) {
    id = s->vtx;

    if (cc->input == RING) {
      if (id-1 <= cc->nring/2) correct = id - 1;
      else correct = cc->nring + 1 - id;
    } else if (cc->input == GRID2D) {
      int ii = (id-1) % cc->nx;
      int jj = (id-1) / cc->nx;
      correct = ii + jj;
    } else if (cc->input == GRID3D) {
      int ii = (id-1) % cc->nx;
      int jj = ((id-1) / cc->nx) % cc->ny;
      int kk = (id-1) / cc->ny / cc->nx;
      correct = ii + jj + kk;
    }

    printf("AAA %d %d %d\n",id,s->dist,correct);

    if (s->dist != correct) cc->badflag++;
  }
}

/* ----------------------------------------------------------------------
   sort function for sorting KMV values
------------------------------------------------------------------------- */

int sort(char *p1, int len1, char *p2, int len2)
{
  int i1 = *(int *) p1;
  int i2 = *(int *) p2;
  if (i1 < i2) return -1;
  else if (i1 > i2) return 1;
  else return 0;
}