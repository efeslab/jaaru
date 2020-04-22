#ifndef CLASSLIST_H
#define CLASSLIST_H
#include <inttypes.h>
#include "stl-model.h"
#include "hashset.h"
#include "modeltypes.h"

class CacheLine;
class ThreadMemory;
class Thread;
class ModelAction;
class ModelExecution;
class Scheduler;

//struct model_snapshot_members;
/*
typedef SnapList<ModelAction *> action_list_t;
typedef SnapList<uint32_t> func_id_list_t;
typedef SnapList<FuncInst *> func_inst_list_t;
typedef HashTable<FuncInst *, ModelAction *, uintptr_t, 0> inst_act_map_t;

typedef HashSet<Predicate *, uintptr_t, 0, model_malloc, model_calloc, model_free> PredSet;
typedef HSIterator<Predicate *, uintptr_t, 0, model_malloc, model_calloc, model_free> PredSetIter;

typedef HashSet<uint64_t, uint64_t, 0, snapshot_malloc, snapshot_calloc, snapshot_free> value_set_t;
typedef HSIterator<uint64_t, uint64_t, 0, snapshot_malloc, snapshot_calloc, snapshot_free> value_set_iter;
typedef HashSet<void *, uintptr_t, 0, snapshot_malloc, snapshot_calloc, snapshot_free> loc_set_t;
typedef HSIterator<void *, uintptr_t, 0, snapshot_malloc, snapshot_calloc, snapshot_free> loc_set_iter;
typedef HashSet<thread_id_t, int, 0> thrd_id_set_t;
typedef HSIterator<thread_id_t, int, 0> thrd_id_set_iter;

extern volatile int modellock;
*/


unsigned int cacheLineHashFunction ( CacheLine * cl);
bool cacheLineEquals(CacheLine *c1, CacheLine *c2);

typedef HashSet<CacheLine *, uintptr_t, 0, snapshot_malloc, snapshot_calloc, snapshot_free, cacheLineHashFunction, cacheLineEquals> CacheLineSet;
typedef HSIterator<CacheLine *, uintptr_t, 0, snapshot_malloc, snapshot_calloc, snapshot_free, cacheLineHashFunction, cacheLineEquals> CacheLineSetIter;
typedef HashSet<uint64_t, uint64_t, 0, snapshot_malloc, snapshot_calloc, snapshot_free> ValueSet;
typedef HSIterator<uint64_t, uint64_t, 0, snapshot_malloc, snapshot_calloc, snapshot_free> ValueSetIter;

#endif
