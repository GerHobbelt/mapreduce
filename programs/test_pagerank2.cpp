// PageRank via MapReduce
// Karen Devine, 1416
// April 2010
//
// Performs PageRank on a square matrix A using MapReduce library.
//
// Syntax: pagerank basefilename N
//
// Assumes matrix file format as follows:
//     row_i  col_j  nonzero_value    (one line for each local nonzero)
// The number of these files is given by the #_of_files argument
// on the command line.  These files will be read in parallel if
// #_of_files > 1.
//
// The dimensions of the matrix A are given by N (N rows, N columns).
// Ideally, we would store this info in the files, but I haven't yet
// figured out how to do the broadcast necessary to get this info from
// the files to the processors.
//
// Values of the resulting vector y are written to stdout in sorted order:
//     row_i  y_i
//
#include <iostream>
#include <list>
#include <math.h>
#include <assert.h>
#include <unistd.h>
#include <mpi.h>
#include "mapreduce.h"
#include "keyvalue.h"
#include "mrvector2.hpp"
#include "mrmatrix2.hpp"
#include "blockmacros.hpp"
#include "localdisks.hpp"
#include "read_fb_data.hpp"
#include "read_mm_data.hpp"
#include "rmat.hpp"

using namespace MAPREDUCE_NS;
using namespace std;

////////////////////////////////////////////////////////////////////////////
// Global ordinal type for matrix row/column numbers.
#define IDXTYPE int64_t
#define MPI_IDXTYPE MPI_LONG

// Input file types
#define FBFILE 0
#define MMFILE 1
#define RMAT 2

#define ABS(a) ((a) >= 0. ? (a) : -1*(a));

////////////////////////////////////////////////////////////////////////////
// Compute local contribution to adjustment for allzero rows.
//
// allzero_contribution reduce() function
// Performs a dot product of emptyRows and vector x.
// Re-emits emptyRows for next iteration.
// Input:  vector entries (i, v_i) + indices of allzero rows (i, 0)
// Output:  sum of v_j for each allzero row j; stored in ptr.
void allzero_contribution(char *key, int keybytes, char *multivalue, 
                          int nvalues, int *valuebytes,
                          KeyValue *kv, void *ptr)
{
  if (nvalues == 2) {
    // This is an allzero row; 
    // multivalue has 0 (from emptyRows) and v_j (from x).
    const double zero = 0.;
    double *sum = (double *) ptr;
    double *values = (double *) multivalue;
    *sum += (values[0] + values[1]);
    kv->add(key, keybytes, (char *) &zero, sizeof(double));
  }
}

double compute_local_allzero_adj(
  MRMatrix<IDXTYPE> *A,
  MRVector<IDXTYPE> *x,
  double alpha
)
{
  double sum = 0.;

  MapReduce *emr = A->emptyRows;

  emr->add(x->mr);
  emr->compress(allzero_contribution, &sum);

  return (alpha * sum / x->GlobalLen());
}

////////////////////////////////////////////////////////////////////////////
// Reduce function:  compute_lmax_residual
// Multivalues received are the x and y values for a given matrix entry.
// Compute the difference and collect the max.
// input:  key = vector index j; multivalue = {x_j, y_j}
//         ptr = local max residual lresid.
// output: new max lresid
    
void compute_lmax_residual(char *key, int keybytes, 
                           char *multivalue, int nvalues, int *valuebytes,
                           KeyValue *kv, void *ptr)
{
  assert(nvalues == 2);
  double *lmax = (double *) ptr;
  double *values = (double *) multivalue;
  double diff = ABS(values[0] - values[1]);
  if (diff > *lmax) *lmax = diff;
}

////////////////////////////////////////////////////////////////////////////
MRVector<IDXTYPE> *pagerank(
  MRMatrix<IDXTYPE> *A,
  double alpha,
  double tolerance
)
{
  int me = A->mr->my_proc();
  if (me == 0) {cout << "Initializing vectors..." << endl; flush(cout);}
  MRVector<IDXTYPE> *x = new MRVector<IDXTYPE>(A->NumRows(),
                                             A->mr->memsize, A->mr->fpath);
  MRVector<IDXTYPE> *y = new MRVector<IDXTYPE>(x->GlobalLen(), 
                                             A->mr->memsize, A->mr->fpath);
  MRVector<IDXTYPE> *zerovec = new MRVector<IDXTYPE>(x->GlobalLen(), 
                                             A->mr->memsize, A->mr->fpath);

  double randomlink = (1.-alpha)/(double)(x->GlobalLen());
  int iter; 
  int maxniter = (int) ceil(log10(tolerance) / log10(alpha));

  // Scale matrix A.
  A->Scale(alpha);
  x->PutScalar(1./x->GlobalLen());

  MPI_Barrier(MPI_COMM_WORLD);
  double tstart = MPI_Wtime();

  if (me == 0) {cout << "Beginning iterations..." << endl; flush(cout);}
  // PageRank iteration
  for (iter = 0; iter < maxniter; iter++) {

    // Compute adjustment for irreducibility (1-alpha)/n
    double ladj = 0.;
    double gadj = 0.;
    ladj = randomlink * x->LocalSum();

    // Compute local adjustment for all-zero rows.
    double allzeroadj = 0.;
    if (A->nEmptyRows)
      allzeroadj = compute_local_allzero_adj(A, x, alpha);
    ladj += allzeroadj;

    // Compute global adjustment via all-reduce-like operation.
    // Cheating here!  Should be done through MapReduce.
    MPI_Allreduce(&ladj, &gadj, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

    // Compute global adjustment.
    A->MatVec(x, y, zerovec);

    // Add adjustment to product vector in mr.
    y->AddScalar(gadj);

    // Compute max-norm of vector in mr.
    double gmax = y->GlobalMax();

    // Scale vector in mr by 1/maxnorm.
    y->Scale(1./gmax);

    // Compute local residual; this also empties x->mr.
    double lresid = 0.;
    x->mr->add(y->mr);
    x->mr->compress(compute_lmax_residual, &lresid);

    // Compute global max residual.
    double gresid;
    MPI_Allreduce(&lresid, &gresid, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

    //  Move result y to x for next iteration.
    MRVector<IDXTYPE> *tmp = x;
    x = y;
    y = tmp;

    if (me == 0) {
      cout << "iteration " << iter+1 << " resid " << gresid << endl; 
      flush(cout);
    }

    if (gresid < tolerance) 
      break;  // Iterations are done.
  }
  MPI_Barrier(MPI_COMM_WORLD);
  double tstop = MPI_Wtime();

  static double time_sum = 0.;
  time_sum += (tstop - tstart);
  static int time_cnt = 0;
  time_cnt++;

  if (me == 0) {
    cout << " Number of iterations " << iter+1 << " Iteration Time "
         << tstop-tstart << endl;
    cout << " Average time for " << time_cnt << " pagerank computations "
         << time_sum / time_cnt << endl;
  }
  delete y;
  double gsum = x->GlobalSum();
  x->Scale(1./gsum);
  A->Scale(1./alpha);  // Return A to original condition
  return x;
}

////////////////////////////////////////////////////////////////////////////
// Print some simple stats.
static void simple_stats(MRMatrix<IDXTYPE> *A, MRVector<IDXTYPE> *x)
{
int me = A->mr->my_proc();
int np;  MPI_Comm_size(MPI_COMM_WORLD, &np);
IDXTYPE lnentry, maxnentry, minnentry, sumnentry;

  lnentry = A->mr->kv->nkv;
  MPI_Allreduce(&lnentry, &maxnentry, 1, MPI_IDXTYPE, MPI_MAX, MPI_COMM_WORLD);
  MPI_Allreduce(&lnentry, &minnentry, 1, MPI_IDXTYPE, MPI_MIN, MPI_COMM_WORLD);
  MPI_Allreduce(&lnentry, &sumnentry, 1, MPI_IDXTYPE, MPI_SUM, MPI_COMM_WORLD);
  if (me == 0) 
    cout << "Matrix Stats:  nonzeros/proc (max, min, avg):  "
         << maxnentry << " " << minnentry << " " <<  sumnentry/np << endl;

  lnentry = x->mr->kv->nkv;
  MPI_Allreduce(&lnentry, &maxnentry, 1, MPI_IDXTYPE, MPI_MAX, MPI_COMM_WORLD);
  MPI_Allreduce(&lnentry, &minnentry, 1, MPI_IDXTYPE, MPI_MIN, MPI_COMM_WORLD);
  MPI_Allreduce(&lnentry, &sumnentry, 1, MPI_IDXTYPE, MPI_SUM, MPI_COMM_WORLD);
  if (me == 0) 
    cout << "Vector Stats:  entries/proc (max, min, avg):  "
         << maxnentry << " " <<  minnentry << " " <<  sumnentry/np << endl;
}

/////////////////////////////////////////////////////////////////////////////
// compare comparison function.
// Compares two integer keys a and b; returns -1, 0, 1 if a<b, a==b, a>b,
// respectively.
int compare(char *a, int lena, char *b, int lenb)
{
IDXTYPE ia = *(IDXTYPE*)a;
IDXTYPE ib = *(IDXTYPE*)b;
  if (ia < ib) return -1;
  if (ia > ib) return  1;
  return 0;
}

/////////////////////////////////////////////////////////////////////////////
// output reduce() function
// input:  key = row index i; 
//         multivalue = {x_j*A_ij for all j with nonzero A_ij}.
// output: print (i, y_i).

void output(char *key, int keybytes, 
            char *multivalue, int nvalues, int *valuebytes,
            KeyValue *kv, void *ptr)
{
  assert(nvalues == 1);
  double *dptr = (double *) multivalue;
  cout << *(IDXTYPE*) key <<  "    " << dptr[0] << endl;
}

////////////////////////////////////////////////////////////////////////////
int main(int narg, char **args)
{
  int me, np;      // Processor rank and number of processors.
  MPI_Init(&narg,&args);

  MPI_Comm_rank(MPI_COMM_WORLD,&me);
  MPI_Comm_size(MPI_COMM_WORLD,&np);
  if (me == 0) {cout << "Here we go..." << endl; flush(cout);}

  // Default parameters
  double alpha = 0.8;
  double tolerance = 0.00001;
  int NumberOfPageranks = 1;
  int pagesize = 64;
  int filetype = RMAT;

  if (me == 0) 
    cout << "Syntax: pagerank "
         << "[-a alpha] [-t tolerance] "
         << "[-n NumberOfPageranks] [-p pagesize] "
         << "[-r|-m|-k] [filetype parameters]" << endl;

  // Parse the command line.
  int iarg = 0;
  while (iarg < narg) {

    if (strcmp(args[iarg], "-a") == 0) {
      // Pagerank relaxation param
      alpha = atof(args[iarg+1]);
      iarg += 2;
    } else if (strcmp(args[iarg], "-t") == 0) {
      // Pagerank tolerance
      tolerance = atof(args[iarg+1]);
      iarg += 2;
    } else if (strcmp(args[iarg], "-n") == 0) {
      // Number of times to do pagerank
      NumberOfPageranks = atoi(args[iarg+1]);
      iarg += 2;
    } else if (strcmp(args[iarg], "-p") == 0) {
      // Memsize value for out-of-core MapReduce.
      pagesize = atoi(args[iarg+1]);
      iarg += 2;
    } else if (strcmp(args[iarg], "-r") == 0) {
      // Generate rmat input; must also include parameters for rmat.hpp.
      filetype = RMAT;
      iarg++;
    } else if (strcmp(args[iarg], "-m") == 0) {
      filetype = MMFILE;
      iarg++;
    } else if (strcmp(args[iarg], "-k") == 0) {
      // Read Karl's files; must include parameters for read_fb_data.hpp.
      filetype = FBFILE;
      iarg++;
    } else {
      if (me ==0) 
        cout << "Passing option " << args[iarg] << " to file reader" << endl;
      iarg++;
    }
  }
 
  MPI_Barrier(MPI_COMM_WORLD);
  double tstart = MPI_Wtime();

  MapReduce *mrvert = NULL;
  MapReduce *mredge = NULL;
  uint64_t nverts;    // Number of unique non-zero vertices
  uint64_t nrawedges; // Number of edges in input files.
  uint64_t nedges;    // Number of unique edges in input files.
  int vertexsize;

  if (filetype == FBFILE) { // FB files
    // Not yet supported.  Still have assumption that vertices are named
    // 1, 2, ..., nverts as in MatrixMarket or RMAT.  Cannot handle 
    // non-consecutive hashkeys yet.  It wouldn't be too hard, I think;
    // In MRVector, need to replace 1, 2, ..., nverts with hash-key IDs.
    // Need to pass mrvert to MRVector constructor.
    // Or need to renumber as in link2graph.
    if (me == 0) {
      cout << "FBFILE not yet supported." << endl;
      MPI_Abort(MPI_COMM_WORLD, -1);
    }
    ReadFBData readFB(narg, args, true);
    readFB.run(&mrvert, &mredge, &nverts, &nrawedges, &nedges);
    vertexsize = readFB.vertexsize;
  }
  else if (filetype == MMFILE) { // MM file
    ReadMMData readMM(narg, args, true);
    readMM.run(&mrvert, &mredge, &nverts, &nrawedges, &nedges);
    vertexsize = readMM.vertexsize;
  }
  else { // Generate RMAT
    GenerateRMAT rmat(narg, args);
    rmat.run(&mrvert, &mredge, &nverts, &nrawedges, &nedges);
    vertexsize = 8;
  }

  // Row IDs are currently int64_t; cannot support 128-bit keys yet.
  if (vertexsize != 8 && me == 0) {
    cout << "Vertexsize != 8 not yet supported.  Use -e1 option." << endl;
    MPI_Abort(MPI_COMM_WORLD, -1);
  }

  // Persistent storage of the matrix. Will be loaded from files initially.
  // Store the transposed matrix, since it is needed in pagerank's MatVecs.
  if (me == 0) {cout << "Loading matrix..." << endl; flush(cout);}
  MRMatrix<IDXTYPE> A(nverts, nverts, mrvert, mredge, true, 
                      pagesize, MYLOCALDISK);

  delete mredge;
  delete mrvert;  // Will need mrvert for MRVector constructor for FBFILE.

  MPI_Barrier(MPI_COMM_WORLD);
  double tstop = MPI_Wtime();

  if (me == 0) {
    cout << "Time to read/generate/transpose matrix " << tstop - tstart << endl;
    flush(cout);
  }

  // Call PageRank function.
  for (int npr = 0; npr < NumberOfPageranks; npr++) {

    if (me == 0) {cout << "Calling pagerank..." << endl; flush(cout);}

    MRVector<IDXTYPE> *x = pagerank(&A, alpha, tolerance);  

    if (me == 0) {cout << "Pagerank done..." << endl; flush(cout);}

    // Output results:  Gather results to proc 0, sort and print.
    simple_stats(&A, x);
    double xmin = x->GlobalMin();    
    double xmax = x->GlobalMax();       
    double xavg = x->GlobalSum() / x->GlobalLen();
    if (x->GlobalLen() < 40) {
      if (me == 0) printf("PageRank Vector:\n");
      MapReduce *xmr = x->mr;
      xmr->gather(1);
      xmr->sort_keys(&compare);
      xmr->convert();
      xmr->reduce(&output, NULL);
    }
    if (me == 0) {
      cout << "Page Rank Stats:  " << endl;
      cout << "      Max Value:  " << xmax << endl;
      cout << "      Min Value:  " << xmin << endl;
      cout << "      Avg Value:  " << xavg << endl;
    }
    delete x;
  }

  MPI_Finalize();
}
