#include "threadmemory.h"
#include "cacheline.h"
#include "common.h"
#include "model.h"
#include "execution.h"
#include "datarace.h"
#include "analysis.h"
#include "plugins.h"

ThreadMemory::ThreadMemory() :
	storeBuffer(),
	obj_to_last_write(),
	flushBuffer(),
	lastsfence(NULL),
	flushcount(0) {
}

/** Adds CFLUSH or CFLUSHOPT to the store buffer. */

void ThreadMemory::addCacheOp(ModelAction * act) {
	storeBuffer.push_back(act);
	ModelAction *lastWrite = obj_to_last_write.get(getCacheID(act->get_location()));
	act->setLastWrites(model->get_execution()->get_curr_seq_num(), lastWrite);
	if (act->is_clflush()) {
		obj_to_last_write.put(getCacheID(act->get_location()), act);
	}

	model->get_execution()->updateStoreBuffer(1);
	flushcount++;
}

void ThreadMemory::addOp(ModelAction * act) {
	storeBuffer.push_back(act);
	model->get_execution()->updateStoreBuffer(1);
}

void ThreadMemory::addWrite(ModelAction * write) {
	storeBuffer.push_back(write);
	model->get_execution()->updateStoreBuffer(1);
	obj_to_last_write.put(getCacheID(write->get_location()), write);
}

bool ThreadMemory::getLastWriteFromStoreBuffer(ModelAction *read, ModelExecution * exec, SnapVector<Pair<ModelExecution *, ModelAction *> >*writes, uint & numslotsleft) {
	uint size = read->getOpSize();
	uintptr_t rbot = (uintptr_t) read->get_location();
	uintptr_t rtop = rbot + size;

	sllnode<ModelAction *> * rit;
	for (rit = storeBuffer.end();rit != NULL;rit=rit->getPrev()) {
		ModelAction *write = rit->getVal();
		if(write->is_write())
			if (checkOverlap(exec, writes, write, numslotsleft, rbot, rtop, size))
				return true;
	}
	return false;
}

bool ThreadMemory::evictOpFromStoreBuffer(ModelAction *act) {
	ASSERT(act->is_write() || act->is_cache_op() || act->is_sfence());
	bool ignoreOp = false;
	if(act->is_write()) {
		bool applyWrite = evictWrite(act);
		if(!applyWrite) {
			ignoreOp = true;
		}
	} else if (act->is_sfence()) {
		if (emptyFlushBuffer())
			return true;
		lastsfence = act;
		model->get_execution()->remove_action_from_store_buffer(act);
		ModelVector<Analysis*> *analyses = getInstalledAnalyses();
		for(uint i=0;i<analyses->size();i++) {
			(*analyses)[i] -> fenceExecutionAnalysis(model->get_execution(), act);
		}
	} else if (act->is_cache_op()) {
		if (act->is_clflush()) {
			if (model->get_execution()->evictCacheOp(act))
				return true;
			flushcount --;
		} else {
			evictFlushOpt(act);
		}
	} else {
		//There is an operation other write, memory fence, and cache operation in the store buffer!!
		ASSERT(0);
	}
	if(!ignoreOp) {
		ModelVector<Analysis*> *analyses = getInstalledAnalyses();
		for(uint i=0;i<analyses->size();i ++) {
			(*analyses)[i] -> evictStoreBufferAnalysis(model->get_execution(), act);
		}
	}
	return false;
}

void ThreadMemory::evictFlushOpt(ModelAction *act) {
	if (lastsfence != NULL)
		act->set_last_clflush(lastsfence->get_seq_number());
	flushBuffer.push_back(act);
}

bool ThreadMemory::popFromStoreBuffer() {
	if (storeBuffer.size() > 0) {
		ModelAction *head = storeBuffer.front();
		storeBuffer.pop_front();
		model->get_execution()->updateStoreBuffer(-1);
		if (evictOpFromStoreBuffer(head))
			return true;
	}
	return false;
}

bool ThreadMemory::emptyStoreBuffer() {
	uint count =0;
	while(storeBuffer.size() > 0) {
		ModelAction *curr = storeBuffer.pop_front();
		count ++;
		if (evictOpFromStoreBuffer(curr)) {
			model->get_execution()->updateStoreBuffer(-count);
			return true;
		}
	}
	model->get_execution()->updateStoreBuffer(-count);
	return false;
}

bool ThreadMemory::emptyFlushBuffer() {
	while(flushBuffer.size() > 0) {
		ModelAction *curr = flushBuffer.pop_front();
		if (model->get_execution()->evictCacheOp(curr))
			return true;
		flushcount --;
	}
	return false;
}

void ThreadMemory::freeActions() {
	sllnode<ModelAction *> * it;
	for (it = storeBuffer.begin();it != NULL;it=it->getNext()) {
		ModelAction *curr = it->getVal();
		delete curr;
	}
	model->get_execution()->updateStoreBuffer(-storeBuffer.size());
	storeBuffer.clear();

	for (it = flushBuffer.begin();it != NULL;it=it->getNext()) {
		ModelAction *curr = it->getVal();
		delete curr;
	}
	flushcount = 0;
	flushBuffer.clear();
}

bool ThreadMemory::emptyWrites(void * address) {
	sllnode<ModelAction *> * rit;
	for (rit = storeBuffer.end();rit != NULL;rit=rit->getPrev()) {
		ModelAction *curr = rit->getVal();
		if (curr->is_write()) {
			void *loc = curr->get_location();
			if (((uintptr_t)address) >= ((uintptr_t)loc) && ((uintptr_t)address) < (((uintptr_t)loc)+(curr->getOpSize()))) {
				break;
			}
		}
	}
	if (rit != NULL) {
		sllnode<ModelAction *> * it;
		for(it = storeBuffer.begin();it!= NULL;) {
			sllnode<ModelAction *> *next = it->getNext();
			ModelAction *curr = it->getVal();
			storeBuffer.erase(it);

			if (evictOpFromStoreBuffer(curr))
				return false;
			model->get_execution()->updateStoreBuffer(-1);
			if (it == rit)
				return true;
			it = next;
		}
	}
	return false;
}

bool ThreadMemory::hasPendingFlushes() {
	return flushcount != 0;
}

bool ThreadMemory::evictWrite(ModelAction *writeop)
{
	//Initializing the sequence number
	ModelExecution *execution = model->get_execution();
	execution->remove_action_from_store_buffer(writeop);
	return execution->add_write_to_lists(writeop);
}
