#!/usr/local/bin/python

# ----------------------------------------------------------------------
#   MR-MPI = MapReduce-MPI library
#   http://www.cs.sandia.gov/~sjplimp/mapreduce.html
#   Steve Plimpton, sjplimp@sandia.gov, Sandia National Laboratories
#
#   Copyright (2009) Sandia Corporation.  Under the terms of Contract
#   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
#   certain rights in this software.  This software is distributed under 
#   the modified Berkeley Software Distribution (BSD) License.
#
#   See the README file in the top-level MapReduce directory.
# -------------------------------------------------------------------------

# MapReduce word frequency example in Python
# Syntax: wordfreq.py file1 file2 ...
# (1) reads all files, parses into words separated by whitespace
# (2) counts occurrence of each word in all files
# (3) prints top 10 words

import sys
import pypar
from mrmpi import mrmpi

# fileread map() function
# for each word in file, emit key = word, value = NULL

def fileread(itask,mr,dummy):
  text = open(files[itask]).read()
  words = text.split()
  for word in words: mr.add(word,None)

# sum reduce() function
# emit key = word, value = # of multi-values

def sum(key,mvalue,mr):
  mr.add(key,len(mvalue))

# ncompare compare() function
# order values by count, largest first

def ncompare(key1,key2):
  if key1 < key2: return 1
  elif key1 > key2: return -1
  else: return 0

# output reduce() function
# depending on flag, emit KV or print it, up to limit

def output(key,mvalue,mr):
  count[0] += 1
  if count[0] > count[1]: return
  if count[2]: print mvalue[0],key
  else: mr.add(key,mvalue[0])

# main program

nprocs = pypar.size()
me = pypar.rank()

if len(sys.argv) < 2:
  print "Syntax: wordfreq.py file1 file2 ..."
  sys.exit()
files = sys.argv[1:]

mr = mrmpi()

pypar.barrier()
tstart = pypar.time()

nwords = mr.map(len(files),fileread)
mr.collate()
nunique = mr.reduce(sum)

pypar.barrier()
tstop = pypar.time()

mr.sort_values(ncompare)
mr.clone()
count = [0,10,0]
mr.reduce(output)

mr.gather(1)
mr.sort_values(ncompare)
mr.clone()
count = [0,10,1]
mr.reduce(output)

mr.destroy()

# output

if me == 0:
  print "%d total words, %d unique words" % (nwords,nunique)
  print "Time to process %d files on %d procs = %g (secs)" % \
        (len(files),nprocs,tstop-tstart);
  
pypar.finalize()
