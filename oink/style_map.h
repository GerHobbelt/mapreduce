#if defined MAP_TASK_STYLE

MapStyle(rmat_generate)

#elif defined MAP_FILE_STYLE

MapStyle(read_edge)
MapStyle(read_words)

#elif defined MAP_STRING_STYLE


#elif defined MAP_MR_STYLE

MapStyle(edge_to_vertices)
MapStyle(edge_upper)
MapStyle(invert)

#else

#include "keyvalue.h"
using namespace MAPREDUCE_NS;

void rmat_generate(int itask, KeyValue *kv, void *ptr);
void read_edge(int itask, char *file, KeyValue *kv, void *ptr);
void read_words(int itask, char *file, KeyValue *kv, void *ptr);
void edge_to_vertices(uint64_t itask, char *key, int keybytes, char *value,
		      int valuebytes, KeyValue *kv, void *ptr);
void edge_upper(uint64_t itask, char *key, int keybytes, char *value,
		int valuebytes, KeyValue *kv, void *ptr);
void invert(uint64_t itask, char *key, int keybytes, char *value,
	    int valuebytes, KeyValue *kv, void *ptr);

#endif
