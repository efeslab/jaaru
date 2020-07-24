/**
 * File: threadmemory.h
 * This is the shadow memory for simulating TSO memory semantics for X86 processor.
 * This memory tracks the effects of memory operations on the memory to validate
 * the program crash consistency property.
 */

#ifndef THREADMEMORY_H
#define THREADMEMORY_H
#include "stl-model.h"
#include "hashtable.h"
#include "action.h"
#include "actionlist.h"

class ThreadMemory {
public:
	ThreadMemory();
	void getWritesFromStoreBuffer(void *address, SnapVector<ModelAction *> * rf_set);
	ModelAction * getLastWriteFromStoreBuffer(void *address);
	void addOp(ModelAction *act);
	void applyFence();
	ModelAction *popFromStoreBuffer();
	void writeToCacheLine(ModelAction *write);

	SNAPSHOTALLOC;
private:
	void evictOpFromStoreBuffer(ModelAction *op);
	void evictWrite(ModelAction *write);
	void evictNonAtomicWrite(ModelAction *na_write);
	void executeWriteOperation(ModelAction *write);
	void emptyStoreBuffer();

	SnapList<ModelAction*> storeBuffer;
};

#endif
