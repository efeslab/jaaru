#include <stdio.h>
#include <algorithm>
#include <new>
#include <stdarg.h>

#include "model.h"
#include "execution.h"
#include "action.h"
#include "schedule.h"
#include "common.h"
#include "clockvector.h"
#include "threads-model.h"
#include "reporter.h"
#include "fuzzer.h"
#include "datarace.h"
#include "mutex.h"
#include "threadmemory.h"
#include "cacheline.h"
#include "varrange.h"

#define INITIAL_THREAD_ID       0

/**
 * Structure for holding small ModelChecker members that should be snapshotted
 */
struct model_snapshot_members {
	model_snapshot_members() :
		/* First thread created will have id INITIAL_THREAD_ID */
		next_thread_id(INITIAL_THREAD_ID),
		used_sequence_numbers(0),
		bugs(),
		warnings(),
		asserted(false)
	{ }

	~model_snapshot_members() {
		for (unsigned int i = 0;i < bugs.size();i++)
			delete bugs[i];
		bugs.clear();
		for (unsigned int i = 0;i < warnings.size();i++)
			delete warnings[i];
		warnings.clear();
	}

	unsigned int next_thread_id;
	modelclock_t used_sequence_numbers;
	SnapVector<bug_message *> bugs;
	SnapVector<bug_message *> warnings;
	/** @brief Incorrectly-ordered synchronization was made */
	bool asserted;

	SNAPSHOTALLOC
};

/** @brief Constructor */
ModelExecution::ModelExecution(ModelChecker *m, Scheduler *scheduler) :
	model(m),
	params(NULL),
	scheduler(scheduler),
	thread_map(2),	/* We'll always need at least 2 threads */
	pthread_map(0),
	pthread_counter(2),
	action_trace(),
	obj_map(),
	condvar_waiters_map(),
	obj_thrd_map(),
	obj_wr_thrd_map(),
	mutex_map(),
	cond_map(),
	thrd_last_action(1),
	priv(new struct model_snapshot_members ()),
	fuzzer(new Fuzzer()),
	isfinished(false)
{
	/* Initialize a model-checker thread, for special ModelActions */
	model_thread = new Thread(get_next_id());
	add_thread(model_thread);
	fuzzer->register_engine(m, this);
	scheduler->register_engine(this);
#ifdef TLS
	pthread_key_create(&pthreadkey, tlsdestructor);
#endif
}

/** @brief Destructor */
ModelExecution::~ModelExecution()
{
	for (unsigned int i = 0;i < get_num_threads();i++)
		delete get_thread(int_to_id(i));

	delete priv;
}


int ModelExecution::get_execution_number() const
{
	return model->get_execution_number();
}

static SnapVector<action_list_t> * get_safe_ptr_vect_action(HashTable<const void *, SnapVector<action_list_t> *, uintptr_t, 2> * hash, void * ptr)
{
	SnapVector<action_list_t> *tmp = hash->get(ptr);
	if (tmp == NULL) {
		tmp = new SnapVector<action_list_t>();
		hash->put(ptr, tmp);
	}
	return tmp;
}

static simple_action_list_t * get_safe_ptr_action(HashTable<const void *, simple_action_list_t *, uintptr_t, 2> * hash, void * ptr)
{
	simple_action_list_t *tmp = hash->get(ptr);
	if (tmp == NULL) {
		tmp = new simple_action_list_t();
		hash->put(ptr, tmp);
	}
	return tmp;
}

static SnapVector<simple_action_list_t> * get_safe_ptr_vect_action(HashTable<const void *, SnapVector<simple_action_list_t> *, uintptr_t, 2> * hash, void * ptr)
{
	SnapVector<simple_action_list_t> *tmp = hash->get(ptr);
	if (tmp == NULL) {
		tmp = new SnapVector<simple_action_list_t>();
		hash->put(ptr, tmp);
	}
	return tmp;
}

/**
 * When vectors of action lists are reallocated due to resize, the root address of
 * action lists may change. Hence we need to fix the parent pointer of the children
 * of root.
 */
static void fixup_action_list (SnapVector<action_list_t> * vec)
{
	for (uint i = 0;i < vec->size();i++) {
		action_list_t * list = &(*vec)[i];
		if (list != NULL)
			list->fixupParent();
	}
}

/** @return a thread ID for a new Thread */
thread_id_t ModelExecution::get_next_id()
{
	return priv->next_thread_id++;
}

/** @return the number of user threads created during this execution */
unsigned int ModelExecution::get_num_threads() const
{
	return priv->next_thread_id;
}

/** @return a sequence number for a new ModelAction */
modelclock_t ModelExecution::get_next_seq_num()
{
	return ++priv->used_sequence_numbers;
}

/** @return a sequence number for a new ModelAction */
modelclock_t ModelExecution::get_curr_seq_num()
{
	return priv->used_sequence_numbers;
}

/** Restore the last used sequence number when actions of a thread are postponed by Fuzzer */
void ModelExecution::restore_last_seq_num()
{
	priv->used_sequence_numbers--;
}

/**
 * @brief Should the current action wake up a given thread?
 *
 * @param curr The current action
 * @param thread The thread that we might wake up
 * @return True, if we should wake up the sleeping thread; false otherwise
 */
bool ModelExecution::should_wake_up(const ModelAction *curr, const Thread *thread) const
{
	const ModelAction *asleep = thread->get_pending();
	/* Synchronizing actions may have been backtracked */
	if (asleep->could_synchronize_with(curr))
		return true;
	/* The sleep is literally sleeping */
	if (asleep->is_sleep()) {
		if (fuzzer->shouldWake(asleep))
			return true;
	}

	return false;
}

void ModelExecution::wake_up_sleeping_actions(ModelAction *curr)
{
	for (unsigned int i = 0;i < get_num_threads();i++) {
		Thread *thr = get_thread(int_to_id(i));
		if (scheduler->is_sleep_set(thr)) {
			if (should_wake_up(curr, thr)) {
				/* Remove this thread from sleep set */
				scheduler->remove_sleep(thr);
				if (thr->get_pending()->is_sleep())
					thr->set_wakeup_state(true);
			}
		}
	}
}

void ModelExecution::add_warning(const char *msg, ...) {
	char str[1024];

	va_list ap;
	va_start(ap, msg);
	vsnprintf(str, sizeof(str), msg, ap);
	va_end(ap);
	priv->warnings.push_back(new bug_message(msg, false));
}

void ModelExecution::assert_bug(const char *msg)
{
	priv->bugs.push_back(new bug_message(msg));
	set_assert();
}

/** @return True, if any bugs have been reported for this execution */
bool ModelExecution::have_bug_reports() const
{
	return priv->bugs.size() != 0;
}

SnapVector<bug_message *> * ModelExecution::get_warnings() const
{
	return &priv->warnings;
}

SnapVector<bug_message *> * ModelExecution::get_bugs() const
{
	return &priv->bugs;
}

/**
 * Check whether the current trace has triggered an assertion which should halt
 * its execution.
 *
 * @return True, if the execution should be aborted; false otherwise
 */
bool ModelExecution::has_asserted() const
{
	return priv->asserted;
}

/**
 * Trigger a trace assertion which should cause this execution to be halted.
 * This can be due to a detected bug or due to an infeasibility that should
 * halt ASAP.
 */
void ModelExecution::set_assert()
{
	priv->asserted = true;
}

/**
 * Check if we are in a deadlock. Should only be called at the end of an
 * execution, although it should not give false positives in the middle of an
 * execution (there should be some ENABLED thread).
 *
 * @return True if program is in a deadlock; false otherwise
 */
bool ModelExecution::is_deadlocked() const
{
	bool blocking_threads = false;
	for (unsigned int i = 0;i < get_num_threads();i++) {
		thread_id_t tid = int_to_id(i);
		if (is_enabled(tid))
			return false;
		Thread *t = get_thread(tid);
		if (!t->is_model_thread() && t->get_pending())
			blocking_threads = true;
	}
	return blocking_threads;
}

/**
 * Check if this is a complete execution. That is, have all thread completed
 * execution (rather than exiting because sleep sets have forced a redundant
 * execution).
 *
 * @return True if the execution is complete.
 */
bool ModelExecution::is_complete_execution() const
{
	for (unsigned int i = 0;i < get_num_threads();i++)
		if (is_enabled(int_to_id(i)))
			return false;
	return true;
}

ModelAction * ModelExecution::convertNonAtomicStore(void * location) {
	ASSERT(0);
	uint64_t value = *((const uint64_t *) location);
	modelclock_t storeclock;
	thread_id_t storethread;
	getStoreThreadAndClock(location, &storethread, &storeclock);
	setAtomicStoreFlag(location);
	ModelAction * act = new ModelAction(NONATOMIC_WRITE, memory_order_relaxed, location, value, get_thread(storethread));
	act->set_seq_number(storeclock);
	add_normal_write_to_lists(act);
	add_write_to_lists(act);
	return act;
}

/**
 * Processes a read model action.
 * @param curr is the read model action to process.
 * @param rf_set is the set of model actions we can possibly read from
 * @return True if the read can be pruned from the thread map list.
 */
void ModelExecution::process_read(ModelAction *curr, SnapVector<ModelAction *> * rf_set)
{
	ASSERT(curr->is_read());
	bool hasnonatomicstore = hasNonAtomicStore(curr->get_location());
	if (hasnonatomicstore) {
		ASSERT(0);
		ModelAction * nonatomicstore = convertNonAtomicStore(curr->get_location());
		rf_set->push_back(nonatomicstore);
	}
	ASSERT(rf_set->size() > 0);
	int index = fuzzer->selectWrite(curr, rf_set);
	ModelAction *rf = (*rf_set)[index];
	ASSERT(rf);
	read_from(curr, rf);
	get_thread(curr)->set_return_value(curr->get_return_value());
	//Update acquire fence clock vector
	ClockVector * hbcv = get_hb_from_write(curr->get_reads_from());
	if (hbcv != NULL)
		get_thread(curr)->get_acq_fence_cv()->merge(hbcv);
}

/**
 * Processes a lock, trylock, or unlock model action.  @param curr is
 * the read model action to process.
 *
 * The try lock operation checks whether the lock is taken.  If not,
 * it falls to the normal lock operation case.  If so, it returns
 * fail.
 *
 * The lock operation has already been checked that it is enabled, so
 * it just grabs the lock and synchronizes with the previous unlock.
 *
 * The unlock operation has to re-enable all of the threads that are
 * waiting on the lock.
 *
 * @return True if synchronization was updated; false otherwise
 */
bool ModelExecution::process_mutex(ModelAction *curr)
{
	ASSERT(0);
	pmc::mutex *mutex = curr->get_mutex();
	struct pmc::mutex_state *state = NULL;

	if (mutex)
		state = mutex->get_state();

	switch (curr->get_type()) {
	case ATOMIC_TRYLOCK: {
		bool success = !state->locked;
		curr->set_try_lock(success);
		if (!success) {
			get_thread(curr)->set_return_value(0);
			break;
		}
		get_thread(curr)->set_return_value(1);
	}
	//otherwise fall into the lock case
	case ATOMIC_LOCK: {
		//TODO: FIND SOME BETTER WAY TO CHECK LOCK INITIALIZED OR NOT
		//if (curr->get_cv()->getClock(state->alloc_tid) <= state->alloc_clock)
		//	assert_bug("Lock access before initialization");
		// TODO: lock count for recursive mutexes
		state->locked = get_thread(curr);
		ModelAction *unlock = get_last_unlock(curr);
		//synchronize with the previous unlock statement
		if (unlock != NULL) {
			synchronize(unlock, curr);
			return true;
		}
		break;
	}
	case ATOMIC_WAIT: {
		//TODO: DOESN'T REALLY IMPLEMENT SPURIOUS WAKEUPS CORRECTLY
		if (fuzzer->shouldWait(curr)) {
			/* wake up the other threads */
			for (unsigned int i = 0;i < get_num_threads();i++) {
				Thread *t = get_thread(int_to_id(i));
				Thread *curr_thrd = get_thread(curr);
				if (t->waiting_on() == curr_thrd && t->get_pending()->is_lock())
					scheduler->wake(t);
			}

			/* unlock the lock - after checking who was waiting on it */
			state->locked = NULL;
			/* remove old wait action and disable this thread */
			simple_action_list_t * waiters = get_safe_ptr_action(&condvar_waiters_map, curr->get_location());
			for (sllnode<ModelAction *> * it = waiters->begin();it != NULL;it = it->getNext()) {
				ModelAction * wait = it->getVal();
				if (wait->get_tid() == curr->get_tid()) {
					waiters->erase(it);
					break;
				}
			}

			waiters->push_back(curr);
			scheduler->sleep(get_thread(curr));
		}

		break;
	}
	case ATOMIC_TIMEDWAIT:
	case ATOMIC_UNLOCK: {
		//TODO: FIX WAIT SITUATION...WAITS CAN SPURIOUSLY
		//FAIL...TIMED WAITS SHOULD PROBABLY JUST BE THE SAME
		//AS NORMAL WAITS...THINK ABOUT PROBABILITIES
		//THOUGH....AS IN TIMED WAIT MUST FAIL TO GUARANTEE
		//PROGRESS...NORMAL WAIT MAY FAIL...SO NEED NORMAL
		//WAIT TO WORK CORRECTLY IN THE CASE IT SPURIOUSLY
		//FAILS AND IN THE CASE IT DOESN'T...  TIMED WAITS
		//MUST EVENMTUALLY RELEASE...

		// TODO: lock count for recursive mutexes
		/* wake up the other threads */
		for (unsigned int i = 0;i < get_num_threads();i++) {
			Thread *t = get_thread(int_to_id(i));
			Thread *curr_thrd = get_thread(curr);
			if (t->waiting_on() == curr_thrd && t->get_pending()->is_lock())
				scheduler->wake(t);
		}

		/* unlock the lock - after checking who was waiting on it */
		state->locked = NULL;
		break;
	}
	case ATOMIC_NOTIFY_ALL: {
		simple_action_list_t *waiters = get_safe_ptr_action(&condvar_waiters_map, curr->get_location());
		//activate all the waiting threads
		for (sllnode<ModelAction *> * rit = waiters->begin();rit != NULL;rit=rit->getNext()) {
			scheduler->wake(get_thread(rit->getVal()));
		}
		waiters->clear();
		break;
	}
	case ATOMIC_NOTIFY_ONE: {
		simple_action_list_t *waiters = get_safe_ptr_action(&condvar_waiters_map, curr->get_location());
		if (waiters->size() != 0) {
			Thread * thread = fuzzer->selectNotify(waiters);
			scheduler->wake(thread);
		}
		break;
	}

	default:
		ASSERT(0);
	}
	return false;
}

/**
 * Process a write ModelAction
 * @param curr The ModelAction to process
 * @return True if the mo_graph was updated or promises were resolved
 */
void ModelExecution::process_write(ModelAction *curr)
{
	get_thread(curr)->getMemory()->addWrite(curr);
	if(!curr->is_rmw()){
		get_thread(curr)->set_return_value(VALUE_NONE);
	}
	
}

/**
 * Process a cache operation including be CLWB, CLFLUSH, CLFLUSHOPT
 * @param curr: the cache operation that need to be processed
 * @return Nothing is returned
 */
void ModelExecution::process_cache_op(ModelAction *curr)
{
	ASSERT(curr->is_cache_op());
	get_thread(curr)->getMemory()->addCacheOp(curr);
	get_thread(curr)->set_return_value(VALUE_NONE);
}

/**
 * Process a memory fence including SFENCE, MFENCE
 * @param curr: the fence operation
 * @return Nothing is returned
 */
void ModelExecution::process_memory_fence(ModelAction *curr)
{
	get_thread(curr)->getMemory()->applyFence(curr);
	get_thread(curr)->set_return_value(VALUE_NONE);
}

/**
 * @brief Process the current action for thread-related activity
 *
 * Performs current-action processing for a THREAD_* ModelAction. Proccesses
 * may include setting Thread status, completing THREAD_FINISH/THREAD_JOIN
 * synchronization, etc.  This function is a no-op for non-THREAD actions
 * (e.g., ATOMIC_{READ,WRITE,RMW,LOCK}, etc.)
 *
 * @param curr The current action
 * @return True if synchronization was updated or a thread completed
 */
void ModelExecution::process_thread_action(ModelAction *curr)
{
	switch (curr->get_type()) {
	case THREAD_CREATE: {
		thrd_t *thrd = (thrd_t *)curr->get_location();
		struct thread_params *params = (struct thread_params *)curr->get_value();
		Thread *th = new Thread(get_next_id(), thrd, params->func, params->arg, get_thread(curr));
		curr->set_thread_operand(th);
		add_thread(th);
		th->set_creation(curr);
		break;
	}
	case PTHREAD_CREATE: {
		(*(uint32_t *)curr->get_location()) = pthread_counter++;

		struct pthread_params *params = (struct pthread_params *)curr->get_value();
		Thread *th = new Thread(get_next_id(), NULL, params->func, params->arg, get_thread(curr));
		curr->set_thread_operand(th);
		add_thread(th);
		th->set_creation(curr);

		if ( pthread_map.size() < pthread_counter )
			pthread_map.resize( pthread_counter );
		pthread_map[ pthread_counter-1 ] = th;

		break;
	}
	case THREAD_JOIN: {
		Thread *blocking = curr->get_thread_operand();
		ModelAction *act = get_last_action(blocking->get_id());
		synchronize(act, curr);
		break;
	}
	case PTHREAD_JOIN: {
		Thread *blocking = curr->get_thread_operand();
		ModelAction *act = get_last_action(blocking->get_id());
		synchronize(act, curr);
		break;	// WL: to be add (modified)
	}

	case THREADONLY_FINISH:
	case THREAD_FINISH: {
		Thread *th = get_thread(curr);
		if (curr->get_type() == THREAD_FINISH &&
				th == model->getInitThread()) {
			th->complete();
			setFinished();
			break;
		}

		/* Wake up any joining threads */
		for (unsigned int i = 0;i < get_num_threads();i++) {
			Thread *waiting = get_thread(int_to_id(i));
			if (waiting->waiting_on() == th &&
					waiting->get_pending()->is_thread_join())
				scheduler->wake(waiting);
		}
		th->complete();
		break;
	}
	case THREAD_START: {
		break;
	}
	case THREAD_SLEEP: {
		Thread *th = get_thread(curr);
		th->set_pending(curr);
		scheduler->add_sleep(th);
		break;
	}
	default:
		break;
	}
}

/**
 * Initialize the current action by performing one or more of the following
 * actions, as appropriate: merging RMWR and RMWC/RMW actions,
 * manipulating backtracking sets, allocating and
 * initializing clock vectors, and computing the promises to fulfill.
 *
 * @param curr The current action, as passed from the user context; may be
 * freed/invalidated after the execution of this function, with a different
 * action "returned" its place (pass-by-reference)
 * @return True if curr is a newly-explored action; false otherwise
 */
void ModelExecution::initialize_curr_action(ModelAction *curr)
{
		curr->set_seq_number(get_next_seq_num());
		/* Always compute new clock vector */
		curr->create_cv(get_parent_action(curr->get_tid()));
}
/**
 * @brief Establish reads-from relation between two actions
 *
 * Perform basic operations involved with establishing a concrete rf relation,
 * including setting the ModelAction data and checking for release sequences.
 *
 * @param act The action that is reading (must be a read)
 * @param rf The action from which we are reading (must be a write)
 *
 * @return True if this read established synchronization
 */

void ModelExecution::read_from(ModelAction *act, ModelAction *rf)
{
	ASSERT(rf);
	ASSERT(rf->is_write());
	ASSERT(act->is_read());
	ThreadMemory *readMem = get_thread( act->get_tid() )->getMemory();
	ThreadMemory *writeMem = get_thread( act->get_tid() )->getMemory();
	if(readMem == writeMem){
		//Reading from a write in the same thread.
		readMem->persistUntil(rf->get_seq_number());
	} else{
		//Reading from the write in another thread.
		readMem->persistUntil(act->get_seq_number());
		writeMem->persistUntil(rf->get_seq_number());
	}
	//UPDATE read from set
	act->set_read_from(rf);
	ClockVector *cv = get_hb_from_write(rf);
	if (cv != NULL){
		act->get_cv()->merge(cv);
	}
}

/**
 * @brief Synchronizes two actions
 *
 * When A synchronizes with B (or A --sw-> B), B inherits A's clock vector.
 * This function performs the synchronization as well as providing other hooks
 * for other checks along with synchronization.
 *
 * @param first The left-hand side of the synchronizes-with relation
 * @param second The right-hand side of the synchronizes-with relation
 * @return True if the synchronization was successful (i.e., was consistent
 * with the execution order); false otherwise
 */
bool ModelExecution::synchronize(const ModelAction *first, ModelAction *second)
{
	if (*second < *first) {
		ASSERT(0);	//This should not happend
		return false;
	}
	return second->synchronize_with(first);
}

/**
 * @brief Check whether a model action is enabled.
 *
 * Checks whether an operation would be successful (i.e., is a lock already
 * locked, or is the joined thread already complete).
 *
 * For yield-blocking, yields are never enabled.
 *
 * @param curr is the ModelAction to check whether it is enabled.
 * @return a bool that indicates whether the action is enabled.
 */
bool ModelExecution::check_action_enabled(ModelAction *curr) {
	if (curr->is_lock()) {
		pmc::mutex *lock = curr->get_mutex();
		struct pmc::mutex_state *state = lock->get_state();
		if (state->locked) {
			Thread *lock_owner = (Thread *)state->locked;
			Thread *curr_thread = get_thread(curr);
			if (lock_owner == curr_thread && state->type == PTHREAD_MUTEX_RECURSIVE) {
				return true;
			}
			return false;
		}
	} else if (curr->is_thread_join()) {
		Thread *blocking = curr->get_thread_operand();
		if (!blocking->is_complete()) {
			return false;
		}
	} else if (curr->is_sleep()) {
		if (!fuzzer->shouldSleep(curr))
			return false;
	}

	return true;
}

/**
 * This is the heart of the model checker routine. It performs model-checking
 * actions corresponding to a given "current action." Among other processes, it
 * calculates reads-from relationships, updates synchronization clock vectors,
 * forms a memory_order constraints graph, and handles replay/backtrack
 * execution when running permutations of previously-observed executions.
 *
 * @param curr The current action to process
 * @return The ModelAction that is actually executed; may be different than
 * curr
 */
ModelAction * ModelExecution::check_current_action(ModelAction *curr)
{
	ASSERT(curr);
	initialize_curr_action(curr);
	if(curr->is_rmw()) {
		get_thread(curr)->getMemory()->applyRMW(curr);
	}
	DBG();

	wake_up_sleeping_actions(curr);

	SnapVector<ModelAction *> * rf_set = NULL;
	/* Build may_read_from set for newly-created actions */
	if (curr->is_read())
		rf_set = build_may_read_from(curr);

	if (curr->is_read()) {
		process_read(curr, rf_set);
		delete rf_set;
	} else
		ASSERT(rf_set == NULL);

	/* Add the action to lists */
	add_action_to_lists(curr);

	if (curr->is_write())
		add_write_to_lists(curr);

	process_thread_action(curr);

	if (curr->is_write()) {
		process_write(curr);
	}

	if (curr->is_cache_op()) {
		process_cache_op(curr);
	}

	if (curr->is_memory_fence()) {
		process_memory_fence(curr);
	}

	if (curr->is_mutex_op()) {
		process_mutex(curr);
	}

	return curr;
}

/**
 * Computes the clock vector that happens before propagates from this write.
 *
 * @param rf The action that might be part of a release sequence. Must be a
 * write.
 * @return ClockVector of happens before relation.
 */

ClockVector * ModelExecution::get_hb_from_write(ModelAction *rf) const {
	SnapVector<ModelAction *> * processset = NULL;
	for ( ;rf != NULL;rf = rf->get_reads_from()) {
		ASSERT(rf->is_write());
		if (!rf->is_rmw() || rf->get_rfcv() != NULL)
			break;
		if (processset == NULL)
			processset = new SnapVector<ModelAction *>();
		processset->push_back(rf);
	}

	int i = (processset == NULL) ? 0 : processset->size();

	ClockVector * vec = NULL;
	while(true) {
		if (rf->get_rfcv() != NULL) {
			vec = rf->get_rfcv();
		} else if (rf->is_write()) {
			(vec = new ClockVector(vec, NULL))->merge(rf->get_cv());
			rf->set_rfcv(vec);
		} else {
			ASSERT(0);
		}
		i--;
		if (i >= 0) {
			rf = (*processset)[i];
		} else
			break;
	}
	if (processset != NULL)
		delete processset;
	return vec;
}

/**
 * Performs various bookkeeping operations for the current ModelAction. For
 * instance, adds action to the per-object, per-thread action vector and to the
 * action trace list of all thread actions.
 *
 * @param act is the ModelAction to add.
 */
void ModelExecution::add_action_to_lists(ModelAction *act)
{
	int tid = id_to_int(act->get_tid());
	if (act->is_seqcst() || act->is_unlock()) {
		simple_action_list_t *list = get_safe_ptr_action(&obj_map, act->get_location());
		act->setActionRef(list->add_back(act));
	}	
	
	// Update action trace, a total order of all actions
	action_trace.addAction(act);


	// Update obj_thrd_map, a per location, per thread, order of actions
	SnapVector<action_list_t> *vec = get_safe_ptr_vect_action(&obj_thrd_map, act->get_location());
	if ((int)vec->size() <= tid) {
		uint oldsize = vec->size();
		vec->resize(priv->next_thread_id);
		for(uint i = oldsize;i < priv->next_thread_id;i++)
			new (&(*vec)[i]) action_list_t();

		fixup_action_list(vec);
	}
	if (act->is_read() || act->is_write())
		(*vec)[tid].addAction(act);

	// Update thrd_last_action, the last action taken by each thread
	if ((int)thrd_last_action.size() <= tid)
		thrd_last_action.resize(get_num_threads());
	thrd_last_action[tid] = act;

	if (act->is_wait()) {
		void *mutex_loc = (void *) act->get_value();
		act->setActionRef(get_safe_ptr_action(&obj_map, mutex_loc)->add_back(act));
	}
}

void insertIntoActionList(action_list_t *list, ModelAction *act) {
	list->addAction(act);
}
/**
 * Performs various bookkeeping operations for a normal write.  The
 * complication is that we are typically inserting a normal write
 * lazily, so we need to insert it into the middle of lists.
 *
 * @param act is the ModelAction to add.
 */

void ModelExecution::add_normal_write_to_lists(ModelAction *act)
{
	int tid = id_to_int(act->get_tid());
	action_trace.addAction(act);

	// Update obj_thrd_map, a per location, per thread, order of actions
	SnapVector<action_list_t> *vec = get_safe_ptr_vect_action(&obj_thrd_map, act->get_location());
	if (tid >= (int)vec->size()) {
		uint oldsize =vec->size();
		vec->resize(priv->next_thread_id);
		for(uint i=oldsize;i<priv->next_thread_id;i++)
			new (&(*vec)[i]) action_list_t();

		fixup_action_list(vec);
	}
	insertIntoActionList(&(*vec)[tid],act);

	ModelAction * lastact = thrd_last_action[tid];
	// Update thrd_last_action, the last action taken by each thrad
	if (lastact == NULL || lastact->get_seq_number() == act->get_seq_number())
		thrd_last_action[tid] = act;
}


void ModelExecution::add_write_to_lists(ModelAction *write) {
	SnapVector<simple_action_list_t> *vec = get_safe_ptr_vect_action(&obj_wr_thrd_map, write->get_location());
	int tid = id_to_int(write->get_tid());
	if (tid >= (int)vec->size()) {
		uint oldsize =vec->size();
		vec->resize(priv->next_thread_id);
		for(uint i=oldsize;i<priv->next_thread_id;i++)
			new (&(*vec)[i]) simple_action_list_t();
	}
	write->setActionRef((*vec)[tid].add_back(write));
}

/**
 * @brief Get the last action performed by a particular Thread
 * @param tid The thread ID of the Thread in question
 * @return The last action in the thread
 */
ModelAction * ModelExecution::get_last_action(thread_id_t tid) const
{
	int threadid = id_to_int(tid);
	if (threadid < (int)thrd_last_action.size())
		return thrd_last_action[id_to_int(tid)];
	else
		return NULL;
}

/**
 * Gets the last unlock operation performed on a particular mutex (i.e., memory
 * location). This function identifies the mutex according to the current
 * action, which is presumed to perform on the same mutex.
 * @param curr The current ModelAction; also denotes the object location to
 * check
 * @return The last unlock operation
 */
ModelAction * ModelExecution::get_last_unlock(ModelAction *curr) const
{
	void *location = curr->get_location();

	simple_action_list_t *list = obj_map.get(location);
	if (list == NULL)
		return NULL;

	/* Find: max({i in dom(S) | isUnlock(t_i) && samevar(t_i, t)}) */
	sllnode<ModelAction*>* rit;
	for (rit = list->end();rit != NULL;rit=rit->getPrev())
		if (rit->getVal()->is_unlock() || rit->getVal()->is_wait())
			return rit->getVal();
	return NULL;
}

ModelAction * ModelExecution::get_parent_action(thread_id_t tid) const
{
	ModelAction *parent = get_last_action(tid);
	if (!parent)
		parent = get_thread(tid)->get_creation();
	return parent;
}

/**
 * Returns the clock vector for a given thread.
 * @param tid The thread whose clock vector we want
 * @return Desired clock vector
 */
ClockVector * ModelExecution::get_cv(thread_id_t tid) const
{
	ModelAction *firstaction=get_parent_action(tid);
	return firstaction != NULL ? firstaction->get_cv() : NULL;
}

bool valequals(uint64_t val1, uint64_t val2, int size) {
	switch(size) {
	case 1:
		return ((uint8_t)val1) == ((uint8_t)val2);
	case 2:
		return ((uint16_t)val1) == ((uint16_t)val2);
	case 4:
		return ((uint32_t)val1) == ((uint32_t)val2);
	case 8:
		return val1==val2;
	default:
		ASSERT(0);
		return false;
	}
}

/**
 * Finds the last write operation in the current thread that happened
 * before operation 'op'.
 * @param op operation
 */
ModelAction * ModelExecution::get_last_write_before_op(void *writeAddress, ModelAction *op)
{
	ASSERT(writeAddress);
	ASSERT(op->is_cache_op());
	uint threadIndex = (uint)id_to_int(op->get_tid());
	ASSERT(obj_wr_thrd_map.get(writeAddress) != NULL);
	ASSERT(obj_wr_thrd_map.get(writeAddress)->size() > threadIndex );
	simple_action_list_t *writelist = &(*obj_wr_thrd_map.get(writeAddress))[threadIndex];
	ASSERT(writelist);
	sllnode<ModelAction *> * rit;
	for (rit = writelist->end();rit != NULL;rit=rit->getPrev()) {
		ModelAction *before = rit->getVal();
		ASSERT(before->is_write());
		if(before->get_seq_number() < op->get_seq_number()) {
			return before;
		}
	}
	return NULL;
}

/**
 * Build up an initial set of all past writes that this 'read' action may read
 * from, as well as any previously-observed future values that must still be valid.
 *
 * @param curr is the current ModelAction that we are exploring; it must be a
 * 'read' operation.
 */
SnapVector<ModelAction *> *  ModelExecution::build_may_read_from(ModelAction *curr)
{
	SnapVector<simple_action_list_t> *thrd_lists = obj_wr_thrd_map.get(curr->get_location());
	unsigned int i;
	ASSERT(curr->is_read());

	SnapVector<ModelAction *> * rf_set = new SnapVector<ModelAction *>();
	ModelAction *lastWrite = get_thread(curr->get_tid())->getMemory()->getLastWriteFromSoreBuffer(curr->get_location());
	if(lastWrite != NULL){
		//There is a write in the current thread that the read is going to read from that
		rf_set->push_back(lastWrite);
		return rf_set;
	}
	/* Iterate over all threads */
	if (thrd_lists != NULL)
		for (i = 0;i < thrd_lists->size();i++) {
			/* Iterate over actions in thread, starting from most recent */
			ThreadMemory * threadMem = get_thread(int_to_id(i))->getMemory();
			simple_action_list_t *list = &(*thrd_lists)[i];
			if(int_to_id(i) != curr->get_tid()){
				//The current thread already checked and it didn't have any writesin its storeBuffer.
				threadMem->getWritesFromStoreBuffer(curr, rf_set);	
			}
			VarRange *variable = threadMem->getVarRange(curr->get_location());
			if(variable == NULL){
				//This thread haven't executed any write on the given address
				continue;
			}
			modelclock_t beginR = variable->getBeginRange();
			sllnode<ModelAction *> * rit;
			model_print("*************\n");
			for (rit = list->end();rit != NULL;rit=rit->getPrev()) {
				ModelAction *act = rit->getVal();
				model_print("[%p] = %u, ", act->get_location(), act->get_value());
				if (act == curr)
					continue;
				ASSERT(act->is_write());
				/* Stop when the write happens before the valid range*/
				if (act->get_seq_number() < beginR) {
					break;
				}
				rf_set->push_back(act);
			}
			model_print("\n\n");
		}

	if (DBG_ENABLED()) {
		model_print("Reached read action:\n");
		curr->print();
		model_print("End printing read_from_past\n");
	}
	return rf_set;
}

static void print_list(action_list_t *list)
{
	sllnode<ModelAction*> *it;

	model_print("------------------------------------------------------------------------------------\n");
	model_print("#    t    Action type     MO       Location         Value               Rf  CV\n");
	model_print("------------------------------------------------------------------------------------\n");

	unsigned int hash = 0;

	for (it = list->begin();it != NULL;it=it->getNext()) {
		const ModelAction *act = it->getVal();
		if (act->get_seq_number() > 0)
			act->print();
		hash = hash^(hash<<3)^(it->getVal()->hash());
	}
	model_print("HASH %u\n", hash);
	model_print("------------------------------------------------------------------------------------\n");
}

/** @brief Prints an execution trace summary. */
void ModelExecution::print_summary()
{
	model_print("Execution trace %d:", get_execution_number());
	if (scheduler->all_threads_sleeping())
		model_print(" SLEEP-SET REDUNDANT");
	if (have_bug_reports())
		model_print(" DETECTED BUG(S)");

	model_print("\n");

	print_list(&action_trace);
	model_print("\n");

}

void ModelExecution::print_tail()
{
	model_print("Execution trace %d:\n", get_execution_number());

	sllnode<ModelAction*> *it;

	model_print("------------------------------------------------------------------------------------\n");
	model_print("#    t    Action type     MO       Location         Value               Rf  CV\n");
	model_print("------------------------------------------------------------------------------------\n");

	unsigned int hash = 0;

	int length = 25;
	int counter = 0;
	SnapList<ModelAction *> list;
	for (it = action_trace.end();it != NULL;it = it->getPrev()) {
		if (counter > length)
			break;

		ModelAction * act = it->getVal();
		list.push_front(act);
		counter++;
	}

	for (it = list.begin();it != NULL;it=it->getNext()) {
		const ModelAction *act = it->getVal();
		if (act->get_seq_number() > 0)
			act->print();
		hash = hash^(hash<<3)^(it->getVal()->hash());
	}
	model_print("HASH %u\n", hash);
	model_print("------------------------------------------------------------------------------------\n");
}

/**
 * Add a Thread to the system for the first time. Should only be called once
 * per thread.
 * @param t The Thread to add
 */
void ModelExecution::add_thread(Thread *t)
{
	unsigned int i = id_to_int(t->get_id());
	if (i >= thread_map.size())
		thread_map.resize(i + 1);
	thread_map[i] = t;
	if (!t->is_model_thread())
		scheduler->add_thread(t);
}

/**
 * @brief Get a Thread reference by its ID
 * @param tid The Thread's ID
 * @return A Thread reference
 */
Thread * ModelExecution::get_thread(thread_id_t tid) const
{
	unsigned int i = id_to_int(tid);
	if (i < thread_map.size())
		return thread_map[i];
	return NULL;
}

/**
 * @brief Get a reference to the Thread in which a ModelAction was executed
 * @param act The ModelAction
 * @return A Thread reference
 */
Thread * ModelExecution::get_thread(const ModelAction *act) const
{
	return get_thread(act->get_tid());
}

/**
 * @brief Get a Thread reference by its pthread ID
 * @param index The pthread's ID
 * @return A Thread reference
 */
Thread * ModelExecution::get_pthread(pthread_t pid) {
	// pid 1 is reserved for the main thread, pthread ids should start from 2
	if (pid == 1)
		return get_thread(pid);
	union {
		pthread_t p;
		uint32_t v;
	} x;
	x.p = pid;
	uint32_t thread_id = x.v;
	if (thread_id < pthread_counter + 1)
		return pthread_map[thread_id];
	else
		return NULL;
}

/**
 * @brief Check if a Thread is currently enabled
 * @param t The Thread to check
 * @return True if the Thread is currently enabled
 */
bool ModelExecution::is_enabled(Thread *t) const
{
	return scheduler->is_enabled(t);
}

/**
 * @brief Check if a Thread is currently enabled
 * @param tid The ID of the Thread to check
 * @return True if the Thread is currently enabled
 */
bool ModelExecution::is_enabled(thread_id_t tid) const
{
	return scheduler->is_enabled(tid);
}

/**
 * @brief Select the next thread to execute based on the curren action
 *
 * RMW actions occur in two parts, and we cannot split them. And THREAD_CREATE
 * actions should be followed by the execution of their child thread. In either
 * case, the current action should determine the next thread schedule.
 *
 * @param curr The current action
 * @return The next thread to run, if the current action will determine this
 * selection; otherwise NULL
 */
Thread * ModelExecution::action_select_next_thread(const ModelAction *curr) const
{
	/* Follow CREATE with the created thread */
	/* which is not needed, because model.cc takes care of this */
	if (curr->get_type() == THREAD_CREATE)
		return curr->get_thread_operand();
	if (curr->get_type() == PTHREAD_CREATE) {
		return curr->get_thread_operand();
	}
	return NULL;
}

/**
 * Takes the next step in the execution, if possible.
 * @param curr The current step to take
 * @return Returns the next Thread to run, if any; NULL if this execution
 * should terminate
 */
Thread * ModelExecution::take_step(ModelAction *curr)
{
	Thread *curr_thrd = get_thread(curr);
	ASSERT(curr_thrd->get_state() == THREAD_READY);

	ASSERT(check_action_enabled(curr));	/* May have side effects? */
	curr = check_current_action(curr);
	ASSERT(curr);

	/* Process this action in ModelHistory for records */
	if (curr_thrd->is_blocked() || curr_thrd->is_complete())
		scheduler->remove_thread(curr_thrd);

	return action_select_next_thread(curr);
}

/** Computes clock vector that all running threads have already synchronized to.  */

ClockVector * ModelExecution::computeMinimalCV() {
	ClockVector *cvmin = NULL;
	//Thread 0 isn't a real thread, so skip it..
	for(unsigned int i = 1;i < thread_map.size();i++) {
		Thread * t = thread_map[i];
		if (t->get_state() == THREAD_COMPLETED)
			continue;
		thread_id_t tid = int_to_id(i);
		ClockVector * cv = get_cv(tid);
		if (cvmin == NULL)
			cvmin = new ClockVector(cv, NULL);
		else
			cvmin->minmerge(cv);
	}
	return cvmin;
}

Fuzzer * ModelExecution::getFuzzer() {
	return fuzzer;
}

