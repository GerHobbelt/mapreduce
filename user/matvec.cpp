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
//  $Author: kddevin $
//  $Date: 2008-05-14 11:02:04 -0600 (Wed, 14 May 2008) $
//  $Revision: 61 $

#include <iostream>
#include <list>
#include "mpi.h"
#include "mapreduce.h"
#include "keyvalue.h"

using namespace MAPREDUCE_NS;
using namespace std;

//  These should probably be in the include file for MapReduce library.
typedef void MAPFUNCTION(int, KeyValue *, void *);
typedef void REDUCEFUNCTION(char *, int, char **, KeyValue *, void *);
typedef int COMPAREFUNCTION(char *, char *);

//  MAP FUNCTIONS
MAPFUNCTION mm_readfiles;
MAPFUNCTION initialize_xvec;

//  REDUCE FUNCTIONS
REDUCEFUNCTION terms;
REDUCEFUNCTION rowsum;
REDUCEFUNCTION output;

// COMPARISON FUNCTIONS
COMPAREFUNCTION compare;

////////////////////////////////////////////////////////////////////////////
//  Typedefs for values.

#define XVECVALUE -1

typedef struct INTDOUBLE {
  int i;         // When INTDOUBLE is used as a nonzero A_ij in the matrix,
                 // i is the row index of the nonzero.
                 // When INTDOUBLE is used as an entry of vector x, i < 0;
                 // this flag is needed to allow x_j and column j values to
                 // be identified after being reduced.
  double d;      // A_ij or x_j, depending on use above.
};

typedef struct MatrixEntry {
  int j;            // column index
  INTDOUBLE value;  // emitted value:  row index + nonzero value.
};

// Macro for debugging
#define ADDINGINTDOUBLE(key,value,str) printf("%d ADDING %s: (%d, [%d,%lf])\n",Me, str, key, value.i, value.d);
#define REDUCINGINTDOUBLE(key,value,nvalue,str) printf("%d REDUCING %s: (%d, ",Me, str, *(int*)key); for (int k = 1; k <= nvalue; k++) printf("[%d,%lf] ", value[k].i, value[k].d); printf("\n");

////////////////////////////////////////////////////////////////////////////
// Global variables.

int Me, Np;      // Processor rank and number of processors.

////////////////////////////////////////////////////////////////////////////
int main(int narg, char **args)
{
  MPI_Init(&narg,&args);

  MPI_Comm_rank(MPI_COMM_WORLD,&Me);
  MPI_Comm_size(MPI_COMM_WORLD,&Np);

  if (narg != 5) {
    if (Me == 0) printf("Syntax: matvec file.mtx numfiles N M\n");
    MPI_Abort(MPI_COMM_WORLD,1);
  }
  int N = atoi(args[3]);  // Number of rows in matrix.
  int M = atoi(args[4]);  // Number of cols in matrix.

  MPI_Barrier(MPI_COMM_WORLD);
  double tstart = MPI_Wtime();

  MapReduce *mr = new MapReduce(MPI_COMM_WORLD);
  mr->verbosity = 0;

  // Read matrix-market files.
  int nnz = mr->map(atoi(args[2]), &mm_readfiles, args[1]);
  cout << "First Map Done:  nnz = " << nnz << endl;

  // Initialize x vector; add its entries to current map.
  int xcol = mr->map(M, &initialize_xvec, &M, 1);
  cout << "Second Map Done:  xcol = " << xcol << endl;

  // Gather matrix column j and x_j to same processors.
  mr->collate(NULL);

  // Compute terms x_j * A_ij.
  int nterms = mr->reduce(&terms, NULL);
  cout << "Second Reduce Done:  nterms = " << nterms << endl;

  // Gather matrix now by rows.
  mr->collate(NULL);

  // Compute sum of terms over rows.
  int nrow = mr->reduce(&rowsum, NULL);
  cout << "Third Reduce Done:  nrow = " << nrow << endl;

  // Output results:  Gather results to proc 0, sort and print.
  int nkeys = mr->gather(1);
  cout << "Gather done:  nkeys = " << nkeys << endl;
  nkeys = mr->sort_keys(&compare);
  cout << "Sort done:  nkeys = " << nkeys << endl;
  
  // Print results.
  nkeys = mr->convert();
  cout << "Convert done:  nkeys = " << nkeys << endl;
  nkeys = mr->reduce(&output, NULL);
  cout << "Output done:  nkeys = " << nkeys << endl;

  delete mr;

  MPI_Barrier(MPI_COMM_WORLD);
  double tstop = MPI_Wtime();

  if (Me == 0) {
    printf("Time to matvec %s (%d) on %d procs = %g (secs)\n",
	   args[1], nrow, Np, tstop-tstart);
  }

  MPI_Finalize();
}

/////////////////////////////////////////////////////////////////////////////

// mm_readfiles map() function
// For each non-zero in matrix-market file, emit (key,value) = (j,[A_ij,i]).
// Assume matrix-market file is split into itask chunks.
// To allow re-use of the matrix without re-reading the files, 
// store the nonzeros and just re-emit them to re-use the matrix.

void mm_readfiles(int itask, KeyValue *kv, void *ptr)
{
  static list<MatrixEntry> Amat;

  if (ptr == NULL) {
    // No file name provided; assume previously read the matrix into Amat.
    list<MatrixEntry>::iterator nz;
    for (nz=Amat.begin(); nz!=Amat.end(); nz++) {
      kv->add((char *)&((*nz).j), sizeof((*nz).j), 
              (char *)&((*nz).value), sizeof((*nz).value));
      ADDINGINTDOUBLE((*nz).j, (*nz).value, "mm_readfiles");
    }
  }
  else {
    // Read the matrix from a file.
    // First, clear Amat if not already clear.
    char *basefile = (char *) ptr;                  // Matrix-market file name.
    char filename[81];
    sprintf(filename, "%s.%04d", basefile, itask);  // Filename for this task.
    FILE *fp = fopen(filename,"r");
    if (fp == NULL) {
      cout << "File not found:  " << filename << endl;
      exit(-1);
    }
    
    // Values emitted are pair [A_ij,i].
    struct MatrixEntry nz;
  
    // Read matrix market file.  Emit (key, value) = (j, [A_ij,i]).
    while (fscanf(fp, "%d %d %lf", &nz.value.i, &nz.j, &nz.value.d) == 3)  {
      Amat.push_front(nz);
      kv->add((char *)&(nz.j), sizeof(nz.j),
              (char *)&(nz.value),sizeof(nz.value));
      ADDINGINTDOUBLE(nz.j, nz.value, "mm_readfiles");
    }
  
    fclose(fp);
  }
}

/////////////////////////////////////////////////////////////////////////////
// initialize_xvec map() function
// For each vector entry x_j in vector x, emit (j, x_j).
// Note:  Include flag in INTDOUBLE indicating that this emission is 
// from vector x, not column j.

void initialize_xvec(int itask, KeyValue *kv, void *ptr)
{
// KDD For now, just emit uniform input vector.
// Assume ptr = number of columns in matrix = M = ncol.
// Assume initialize_vec is issued once for each column.
  int ncol = *(int *)ptr;
  INTDOUBLE value;

  value.i = XVECVALUE;
  value.d = 1. / (float) ncol; // KDD For now, uniform x_j; later, need input.

  itask++;  // Matrix-market is one-based; the vector should be, too.
  kv->add((char *)&itask, sizeof(itask), (char *) &value, sizeof(value));
  ADDINGINTDOUBLE(itask, value, "initialize_xvec");
}

/////////////////////////////////////////////////////////////////////////////
// terms reduce() function
// input:  key = column index; multivalue = {x_j, of A_ij for
//         all i with nonzero A_ij.}
// output:  key = row index i; value = x_j * A_ij.

void terms(char *key, int nvalues, char **multivalue, KeyValue *kv, void *ptr)
{
INTDOUBLE *xptr;
INTDOUBLE *aptr;

  if (nvalues == 1) 
    // No nonzeros in this column; skip it.
    return;

  // Find the x_j value
  for (int k = 0; k < nvalues; k++) {
    xptr = (INTDOUBLE *) multivalue[k];
    if (xptr->i < 0)  // found it
      break;
  }
  double x_j = xptr->d;

  for (int k = 0; k < nvalues; k++) {
    aptr = (INTDOUBLE *) multivalue[k];
    if (aptr == xptr) continue;  // don't add in x_j * x_j.

    double product = x_j * aptr->d;
    kv->add((char *) &aptr->i, sizeof(aptr->i), 
            (char *) &product, sizeof(product));
    printf("%d REDUCING terms: (%d, %lf)\n", Me, aptr->i, product);
  }
}

/////////////////////////////////////////////////////////////////////////////
// rowsum reduce() function
// input:  key = row index i; 
//         multivalue = {x_j*A_ij for all j with nonzero A_ij}.
// output: key = row index i; value = sum over j [x_j * A_ij] = y_i.
void rowsum(char *key, int nvalues, char **multivalue, KeyValue *kv, void *ptr)
{
  double sum = 0;
  int row = *(int*) key;
  for (int k = 0; k < nvalues; k++) 
    sum += *(double *) multivalue[k];
  kv->add((char *) &row, sizeof(row), (char *) &sum, sizeof(sum));
  printf("%d REDUCING rowsum: (%d, %lf)\n", Me, row, sum);
}

/////////////////////////////////////////////////////////////////////////////
// rowsum reduce() function
// input:  key = row index i; 
//         multivalue = {x_j*A_ij for all j with nonzero A_ij}.
// output: print (i, y_i).

void output(char *key, int nvalues, char **multivalue, KeyValue *kv, void *ptr)
{
  assert(nvalues == 1);
  cout << *(int*) key <<  "    " << *(double*)multivalue[0] << endl;
}

/////////////////////////////////////////////////////////////////////////////
// compare comparison function.
// Compares two integer keys a and b; returns -1, 0, 1 if a<b, a==b, a>b,
// respectively.
int compare(char *a, char *b)
{
int ia = *(int*)a;
int ib = *(int*)b;
  if (ia < ib) return -1;
  if (ia > ib) return  1;
  return 0;
}

