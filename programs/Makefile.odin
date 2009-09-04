# Odin/MPI Makefile for MapReduce programs

CC =		mpic++ -m64
CCFLAGS =	-O2 -I../src 
LINK =		mpic++ -m64
LINKFLAGS =	-O2
USRLIB =	-L../src -lmrmpi
SYSLIB =	
LIB =		../src/libmrmpi.a
DEPFLAGS =      -M

include Makefile.common

