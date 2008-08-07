// MatVec via MapReduce
// Karen Devine, 1416
// June 2008
//
// Performs matrix-vector multiplication  A*x=y.
//
// Syntax: matvec basefilename #_of_files N M
//
// Assumes matrix file format as follows:
//     row_i  col_j  nonzero_value    (one line for each local nonzero)
// The number of these files is given by the #_of_files argument
// on the command line.  These files will be read in parallel if
// #_of_files > 1.
//
// The files should be named basefilename.0000, basefilename.0001, etc.
// up to the #_of_files-1.
//
// The dimensions of the matrix A are given by N and M (N rows, M columns).
// Ideally, we would store this info in the files, but I haven't yet
// figured out how to do the broadcast necessary to get this info from
// the files to the processors.
//
// Values of the resulting vector y are written to stdout in sorted order:
//     row_i  y_i
//
// SVN Information:
//  $Author:$
//  $Date:$
//  $Revision:$

#ifndef MRALL_H__
#define MRALL_H__

#include <iostream>
#include <list>
#include "mpi.h"
#include "mapreduce.h"
#include "keyvalue.h"

using namespace MAPREDUCE_NS;
using namespace std;

/////////////////////////////////////////////////////////////////////////////
//  These should probably be in the include file for MapReduce library.
typedef void MAPFUNCTION(int, KeyValue *, void *);
typedef void REDUCEFUNCTION(char *, int, char *, int, int*, KeyValue *, void *);
typedef int COMPAREFUNCTION(char *, int, char *, int);


/////////////////////////////////////////////////////////////////////////////
//  Data type for values emitted.
#define XVECVALUE -1
typedef struct INTDOUBLE {
  int i;         // When INTDOUBLE is used as a nonzero A_ij in the matrix,
                 // i is the row index of the nonzero.
                 // When INTDOUBLE is used as an entry of vector x, i < 0;
                 // this flag is needed to allow x_j and column j values to
                 // be identified after being reduced.
  double d;      // A_ij or x_j, depending on use above.
};

/////////////////////////////////////////////////////////////////////////////
#define MIN(a,b) ((a) < (b) ? (a) : (b))

/////////////////////////////////////////////////////////////////////////////
int MRGlobalSum(MapReduce *, int);
#endif
