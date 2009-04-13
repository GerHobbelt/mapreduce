/* ----------------------------------------------------------------------
   MR-MPI = MapReduce-MPI library
   http://www.cs.sandia.gov/~sjplimp/mapreduce.html
   Steve Plimpton, sjplimp@sandia.gov, Sandia National Laboratories

   Copyright (2009) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the modified Berkeley Software Distribution (BSD) License.

   See the README file in the top-level MapReduce directory.
------------------------------------------------------------------------- */

/*
MapReduce word frequency example in C
Syntax: cwordfreq file1 file2 ...
(1) reads all files, parses into words separated by whitespace
(2) counts occurrence of each word in all files
(3) prints top 10 words
*/

#include "mpi.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "cmapreduce.h"

void fileread(int, void *, void *);
void sum(char *, int, char *, int, int *, void *, void *);
int ncompare(char *, int, char *, int);
void output(char *, int, char *, int, int*, void *, void *);

struct Count {
  int n,limit,flag;
};

#define FILESIZE 10000000                // should make this infinite

/* ---------------------------------------------------------------------- */

int main(int narg, char **args)
{
  int me,nprocs;
  int nwords,nunique;
  double tstart,tstop;
  Count count;

  MPI_Init(&narg,&args);
  MPI_Comm_rank(MPI_COMM_WORLD,&me);
  MPI_Comm_size(MPI_COMM_WORLD,&nprocs);

  if (narg <= 1) {
    if (me == 0) printf("Syntax: cwordfreq file1 file2 ...\n");
    MPI_Abort(MPI_COMM_WORLD,1);
  }

  void *mr = MR_create(MPI_COMM_WORLD);

  MPI_Barrier(MPI_COMM_WORLD);
  tstart = MPI_Wtime();

  nwords = MR_map(mr,narg-1,&fileread,&args[1]);
  MR_collate(mr,NULL);
  nunique = MR_reduce(mr,&sum,NULL);

  MPI_Barrier(MPI_COMM_WORLD);
  tstop = MPI_Wtime();

  MR_sort_values(mr,&ncompare);
  MR_clone(mr);

  count.n = 0;
  count.limit = 10;
  count.flag = 0;
  MR_reduce(mr,&output,&count);
  
  MR_gather(mr,1);
  MR_sort_values(mr,&ncompare);
  MR_clone(mr);

  count.n = 0;
  count.limit = 10;
  count.flag = 1;
  MR_reduce(mr,&output,&count);

  MR_destroy(mr);

  if (me == 0) {
    printf("%d total words, %d unique words\n",nwords,nunique);
    printf("Time to wordcount %d files on %d procs = %g (secs)\n",
	   narg-1,nprocs,tstop-tstart);
  }

  MPI_Finalize();
}

/* ----------------------------------------------------------------------
   fileread map() function
   for each word in file, emit key = word, value = NULL
------------------------------------------------------------------------- */

void fileread(int itask, void *kv, void *ptr)
{
  char *whitespace = " \t\n\f\r\0";
  char text[FILESIZE];

  char **files = (char **) ptr;
  FILE *fp = fopen(files[itask],"r");
  int nchar = fread(text,1,FILESIZE,fp);
  text[nchar] = '\0';
  fclose(fp);

  char *word = strtok(text,whitespace);
  while (word) {
    MR_kv_add(kv,word,strlen(word)+1,NULL,0);
    word = strtok(NULL,whitespace);
  }
}

/* ----------------------------------------------------------------------
   sum reduce() function
   emit key = word, value = # of multi-values
------------------------------------------------------------------------- */

void sum(char *key, int keybytes, char *multivalue,
	 int nvalues, int *valuebytes, void *kv, void *ptr) 
{
  MR_kv_add(kv,key,keybytes,(char *) &nvalues,sizeof(int));
}

/* ----------------------------------------------------------------------
   ncompare compare() function
   order values by count, largest first
------------------------------------------------------------------------- */

int ncompare(char *p1, int len1, char *p2, int len2)
{
  int i1 = *(int *) p1;
  int i2 = *(int *) p2;
  if (i1 > i2) return -1;
  else if (i1 < i2) return 1;
  else return 0;
}

/* ----------------------------------------------------------------------
   output reduce() function
   depending on flag, emit KV or print it, up to limit
------------------------------------------------------------------------- */

void output(char *key, int keybytes, char *multivalue,
	    int nvalues, int *valuebytes, void *kv, void *ptr)
{
  Count *count = (Count *) ptr;
  count->n++;
  if (count->n > count->limit) return;

  int n = *(int *) multivalue;
  if (count->flag) printf("%d %s\n",n,key);
  else MR_kv_add(kv,key,keybytes,(char *) &n,sizeof(int));
}
