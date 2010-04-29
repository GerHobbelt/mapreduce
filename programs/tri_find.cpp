/* ----------------------------------------------------------------------
   enumerate triangles in a graph via J Cohen algorithm
   run() inputs:
     mre = one KV per edge = (Eij,NULL), with all Vi < Vj
     mrt = copy of mre
   run() outputs:
     mre is unchanged
     mrt = one KV per triangle = ((Vi,Vj,Vk),NULL)
     ntri = # of triangles in mrt
     return elapsed time
 ------------------------------------------------------------------------- */

#include "mpi.h"
#include "math.h"
#include "stdint.h"
#include "stdlib.h"
#include "blockmacros.h"
#include "tri_find.h"
#include "mapreduce.h"
#include "keyvalue.h"

using MAPREDUCE_NS::MapReduce;
using MAPREDUCE_NS::KeyValue;

#define INTMAX 0x7FFFFFFF

/* ---------------------------------------------------------------------- */

double TriFind::run(MapReduce *mre, MapReduce *mrt, uint64_t &ntri)
{
  MPI_Barrier(MPI_COMM_WORLD);
  double tstart = MPI_Wtime();

  // augment edges with degree of each vertex
  // mrt = (Eij,(Di,Dj))

  mrt->map(mrt,map_edge_vert,NULL);
  mrt->collate(NULL);
  mrt->reduce(reduce_first_degree,NULL);
  mrt->collate(NULL);
  mrt->reduce(reduce_second_degree,NULL);

  // find triangles in degree-augmented graph
  // nsq_angles creates angles = triangles missing an edge
  // add in edges from original graph
  // emit_triangles finds completed triangles
  // mrt = ((Vi,Vj,Vk),NULL)

  mrt->map(mrt,map_low_degree,NULL);
  mrt->collate(NULL);
  mrt->reduce(reduce_nsq_angles,NULL);
  mrt->add(mre);
  mrt->collate(NULL);
  ntri = mrt->reduce(reduce_emit_triangles,NULL);

  MPI_Barrier(MPI_COMM_WORLD);
  double tstop = MPI_Wtime();

  return tstop-tstart;
}

/* ---------------------------------------------------------------------- */

void TriFind::map_edge_vert(uint64_t itask, char *key, int keybytes, 
			    char *value, int valuebytes, 
			    KeyValue *kv, void *ptr)
{
  EDGE *edge = (EDGE *) key;
  kv->add((char *) &edge->vi,sizeof(VERTEX),(char *) &edge->vj,sizeof(VERTEX));
  kv->add((char *) &edge->vj,sizeof(VERTEX),(char *) &edge->vi,sizeof(VERTEX));
}

/* ---------------------------------------------------------------------- */

void TriFind::reduce_first_degree(char *key, int keybytes,
				  char *multivalue, int nvalues,
				  int *valuebytes, KeyValue *kv, void *ptr)
{
  int i;
  char *value;
  VERTEX vi,vj;
  EDGE edge;
  DEGREE degree;

  uint64_t nvalues_total;
  CHECK_FOR_BLOCKS(multivalue,valuebytes,nvalues,nvalues_total)
  if (nvalues_total > INTMAX) {
    printf("Too many edges for one vertex in reduce first_degree");
    MPI_Abort(MPI_COMM_WORLD,1);
  }
  int ndegree = nvalues_total;

  vi = *(VERTEX *) key;

  BEGIN_BLOCK_LOOP(multivalue,valuebytes,nvalues)

  value = multivalue;
  for (i = 0; i < nvalues; i++) {
    vj = *(VERTEX *) value;
    if (vi < vj) {
      edge.vi = vi;
      edge.vj = vj;
      degree.di = ndegree;
      degree.dj = 0;
      kv->add((char *) &edge,sizeof(EDGE),(char *) &degree,sizeof(DEGREE));
    } else {
      edge.vi = vj;
      edge.vj = vi;
      degree.di = 0;
      degree.dj = ndegree;
      kv->add((char *) &edge,sizeof(EDGE),(char *) &degree,sizeof(DEGREE));
    }
    value += valuebytes[i];
  }

  END_BLOCK_LOOP
}

/* ---------------------------------------------------------------------- */

void TriFind::reduce_second_degree(char *key, int keybytes,
				   char *multivalue, int nvalues, 
				   int *valuebytes, KeyValue *kv, void *ptr)
{
  DEGREE *one = (DEGREE *) multivalue;
  DEGREE *two = (DEGREE *) &multivalue[valuebytes[0]];
  DEGREE degree;

  if (one->di) {
    degree.di = one->di;
    degree.dj = two->dj;
    kv->add(key,keybytes,(char *) &degree,sizeof(DEGREE));
  } else {
    degree.di = two->di;
    degree.dj = one->dj;
    kv->add(key,keybytes,(char *) &degree,sizeof(DEGREE));
  }
}

/* ---------------------------------------------------------------------- */

void TriFind::map_low_degree(uint64_t itask, char *key, int keybytes, 
			     char *value, int valuebytes,
			     KeyValue *kv, void *ptr)
{
  EDGE *edge = (EDGE *) key;
  DEGREE *degree = (DEGREE *) value;

  if (degree->di < degree->dj)
    kv->add((char *) &edge->vi,sizeof(VERTEX),
	    (char *) &edge->vj,sizeof(VERTEX));
  else if (degree->dj < degree->di)
    kv->add((char *) &edge->vj,sizeof(VERTEX),
	    (char *) &edge->vi,sizeof(VERTEX));
  else if (edge->vi < edge->vj)
    kv->add((char *) &edge->vi,sizeof(VERTEX),
	    (char *) &edge->vj,sizeof(VERTEX));
  else
    kv->add((char *) &edge->vj,sizeof(VERTEX),
	    (char *) &edge->vi,sizeof(VERTEX));
}

/* ---------------------------------------------------------------------- */

void TriFind::reduce_nsq_angles(char *key, int keybytes,
				char *multivalue, int nvalues, int *valuebytes,
				KeyValue *kv, void *ptr)
{
  int j,k,nv,nv2,iblock,jblock;
  VERTEX vj,vk;
  EDGE edge;

  if (nvalues) {
    for (j = 0; j < nvalues-1; j++) {
      vj = *(VERTEX *) &multivalue[j*sizeof(VERTEX)];
      for (k = j+1; k < nvalues; k++) {
	vk = *(VERTEX *) &multivalue[k*sizeof(VERTEX)];
	if (vj < vk) {
	  edge.vi = vj;
	  edge.vj = vk;
	  kv->add((char *) &edge,sizeof(EDGE),key,sizeof(VERTEX));
	} else {
	  edge.vi = vk;
	  edge.vj = vj;
	  kv->add((char *) &edge,sizeof(EDGE),key,sizeof(VERTEX));
	}
      }
    }

  } else {
    MapReduce *mr = (MapReduce *) valuebytes;
    int nblocks;
    mr->multivalue_blocks(nblocks);

    for (iblock = 0; iblock < nblocks; iblock++) { 
      nv = mr->multivalue_block(iblock,&multivalue,&valuebytes);
      for (j = 0; j < nv-1; j++) {
	vj = *(VERTEX *) &multivalue[j*sizeof(VERTEX)];

	for (k = j+1; k < nv; k++) {
	  vk = *(VERTEX *) &multivalue[k*sizeof(VERTEX)];
	  if (vj < vk) {
	    edge.vi = vj;
	    edge.vj = vk;
	    kv->add((char *) &edge,sizeof(EDGE),key,sizeof(VERTEX));
	  } else {
	    edge.vi = vk;
	    edge.vj = vj;
	    kv->add((char *) &edge,sizeof(EDGE),key,sizeof(VERTEX));
	  }
	}

	for (jblock = iblock+1; jblock < nblocks; jblock++) { 
	  nv2 = mr->multivalue_block(jblock,&multivalue,&valuebytes);
	  for (k = 0; k < nv2; k++) {
	    vk = *(VERTEX *) &multivalue[k*sizeof(VERTEX)];
	    if (vj < vk) {
	      edge.vi = vj;
	      edge.vj = vk;
	      kv->add((char *) &edge,sizeof(EDGE),key,sizeof(VERTEX));
	    } else {
	      edge.vi = vk;
	      edge.vj = vj;
	      kv->add((char *) &edge,sizeof(EDGE),key,sizeof(VERTEX));
	    }
	  }
	}

	if (iblock < nblocks)
	  mr->multivalue_block(iblock,&multivalue,&valuebytes);
      }
    } 
  }
}

/* ---------------------------------------------------------------------- */

void TriFind::reduce_emit_triangles(char *key, int keybytes,
				    char *multivalue, int nvalues,
				    int *valuebytes, KeyValue *kv, void *ptr)
{
  int i;
  char *value;

  // loop over values to find a NULL

  int flag = 0;

  uint64_t nvalues_total;
  CHECK_FOR_BLOCKS(multivalue,valuebytes,nvalues,nvalues_total)
  BEGIN_BLOCK_LOOP(multivalue,valuebytes,nvalues)

  for (i = 0; i < nvalues; i++)
    if (valuebytes[i] == 0) {
      flag = 1;
      break;
    }
  if (i < nvalues) break;

  END_BLOCK_LOOP

  if (!flag) return;

  // emit triangle for each vertex

  TRI tri;
  EDGE *edge = (EDGE *) key;
  tri.vj = edge->vi;
  tri.vk = edge->vj;

  BEGIN_BLOCK_LOOP(multivalue,valuebytes,nvalues)

  value = multivalue;
  for (i = 0; i < nvalues; i++) {
    if (valuebytes[i]) {
      tri.vi = *(VERTEX *) value;
      kv->add((char *) &tri,sizeof(TRI),NULL,0);
    }
    value += valuebytes[i];
  }

  END_BLOCK_LOOP
}
