/**
 *
 * @author Aleksandar Dragojevic aleksandar.dragojevic@epfl.ch
 *
 */

#ifndef WLPDSTM_RW_TRANSACTION_H_
#define WLPDSTM_RW_TRANSACTION_H_

#include <assert.h>
#include <stdio.h>
#include <pthread.h>

#include "../common/word.h"
#include "../common/jmp.h"
#include "../common/cache_aligned_alloc.h"
#include "../common/tid.h"
#include "../common/padded.h"
#include "../common/log.h"
#include "../common/random.h"
#include "../common/timing.h"
#include "../common/sampling.h"

#include "constants.h"
#include "cm.h"
#include "memory.h"
#include "stats.h"

// default is cache line size
// how many successive locations (segment size)
//#define LOCK_EXTENT          LOG_CACHE_LINE_SIZE_BYTES
#define LOCK_EXTENT            (LOG_BYTES_IN_WORD + 2)
#define RWLOCK_TABLE_SIZE (1 << 22)
#define WRITE_LOCKED 1
#define WRITE_LOCK_CLEAR 0
#define TRUE 1
#define FALSE 0

namespace wlpdstm {

	class TransactionNors : public CacheAlignedAlloc {
	protected:
		/////////////////
		// types start //
		/////////////////

		typedef Word WriteLock;
		typedef Word ReadLock;

		struct WriteWordLogEntry {
			Word *address;
			Word value;
			Word mask;
			WriteWordLogEntry *next;
		};

		struct WriteLogEntry {
			WriteWordLogEntry *head;

			WriteLock *write_lock;
			TransactionNors *owner;

			// methods
			void InsertWordLogEntry(Word *address, Word value, Word mask);

			WriteWordLogEntry *FindWordLogEntry(Word *address);

			void ClearWordLogEntries();
		};

		// these will be aligned on cache line boundaries
		struct ReadWriteLock {
			WriteLock write_lock;
			ReadLock read_lock[MAX_THREADS];
		};

		typedef Log<WriteLogEntry> WriteLog;
		typedef Log<WriteWordLogEntry> WriteWordLogMemPool;

		typedef Word Token;
		
		enum RestartCause {
			NO_RESTART = 0,
			RESTART_EXTERNAL,
			RESTART_READ_WRITE,
			RESTART_WRITE_READ,
			RESTART_WRITE_WRITE
		};

#ifdef WAIT_ON_SUCC_ABORTS
		static const unsigned SUCC_ABORTS_THRESHOLD = 1;
		static const unsigned SUCC_ABORTS_MAX = 10;		
		static const unsigned WAIT_CYCLES_MULTIPLICATOR = 8000;
#endif /* WAIT_ON_SUCC_ABORTS */
		
		///////////////
		// types end //
		///////////////
		
		public:
			static void GlobalInit();

			void ThreadInit();

			void TxStart(int lex_tx_id = NO_LEXICAL_TX);

			void TxCommit();

			void TxAbort();

			void TxRestart(RestartCause cause = RESTART_EXTERNAL);		

			Word ReadWord(Word *addr);

			void WriteWord(Word *address, Word val, Word mask = LOG_ENTRY_UNMASKED);

			void *TxMalloc(size_t size);

			void TxFree(void *ptr, size_t size);

			void LockMemoryBlock(void *address, size_t size);

			void ThreadShutdown();

            void StartThreadProfiling();

            void EndThreadProfiling();

			static void GlobalShutdown();

		protected:
			static void PrintStatistics();

			static void PrintConfiguration(FILE *out_file);
		
			static unsigned map_address_to_index(Word *address);

			void AbortCleanup();
			void AbortJump();

			void RestartCleanup(RestartCause cause);
			void RestartJump();

			void Rollback();

#ifdef WAIT_ON_SUCC_ABORTS
			void WaitOnAbort();
#endif /* WAIT_ON_SUCC_ABORTS */

			void LockMemoryStripe(Word *addr, ReadWriteLock *rwlock);

			static Word MaskWord(Word old, Word val, Word mask);

			static Word MaskWord(WriteWordLogEntry *entry);

		/////////////////////////////
		// thread local data start //
		/////////////////////////////

		public:
			// Local, but should be at the start of the descriptor as this is what assembly jump expects.
			CACHE_LINE_ALIGNED union {
				LONG_JMP_BUF start_buf;
				char padding_start_buf[CACHE_LINE_SIZE_BYTES * START_BUF_PADDING_SIZE];
			};

			///////////////////////
			// shared data start //
			///////////////////////

			CACHE_LINE_ALIGNED static ReadWriteLock rwlock_table[RWLOCK_TABLE_SIZE];

			CACHE_LINE_ALIGNED static TransactionNors *transactions[MAX_THREADS];

			// number of application threads running
			CACHE_LINE_ALIGNED static PaddedWord thread_count;

			CACHE_LINE_ALIGNED static Token tokens[MAX_THREADS];

			/////////////////////
			// shared data end //
			/////////////////////

		protected:
			// shared contention manager data
			CACHE_LINE_ALIGNED ContentionManager cm;

			// local? memory manager
			MemoryManager mm;

			// local data that doesn't need to be aligned:

			// local
			Tid tid;

			// local
			ThreadStatistics stats;

			// local
			WriteLog write_log;

			WriteWordLogMemPool write_word_log_mem_pool;

			// local
			Random random;

			TxProfiling profiling;

#ifdef WAIT_ON_SUCC_ABORTS
			// local
			unsigned succ_aborts;
#endif /* WAIT_ON_SUCC_ABORTS */

		///////////////////////////
		// thread local data end //
		///////////////////////////
	};

	typedef TransactionNors TransactionImpl;
}

/*inline void* GC(void *data){
	Word previous_snapshot[wlpdstm::MAX_THREADS], current_snapshot[wlpdstm::MAX_THREADS];

	printf("on gc\n");
	for(Word i=0; i<wlpdstm::MAX_THREADS; i++){
		previous_snapshot[i] = 1;
	}

	while(true){
		//take snapshot of running transactions
		for(Word i=0; i<wlpdstm::TransactionNors::thread_count.val; i++){
			current_snapshot[i] = wlpdstm::TransactionNors::tokens[i].active_tx;
		}

		for(Word i=0; i<RWLOCK_TABLE_SIZE; i++){
			wlpdstm::TransactionNors::rwlock_table[i].onGC = TRUE;

			for(Word j=0; j<wlpdstm::TransactionNors::thread_count.val; j++){
				if(wlpdstm::TransactionNors::rwlock_table[i].read_lock[j] == 0){
					continue;
				}
				if(previous_snapshot[j] <= wlpdstm::TransactionNors::rwlock_table[i].read_lock[j]
				    && wlpdstm::TransactionNors::rwlock_table[i].read_lock[j] < current_snapshot[j]){
					wlpdstm::TransactionNors::rwlock_table[i].read_lock[j] = 0;
				}
			}
			wlpdstm::TransactionNors::rwlock_table[i].onGC = FALSE;
		}
	}

	return NULL;
}*/

inline void wlpdstm::TransactionNors::GlobalInit() {
	PrintConfiguration(stdout);

	// initialize global read and write locks table
	for(unsigned i = 0; i < RWLOCK_TABLE_SIZE; i++) {
		rwlock_table[i].write_lock = WRITE_LOCK_CLEAR;
		//rwlock_table[i].onGC = FALSE;
		for(unsigned j=0; j<MAX_THREADS; j++){
			rwlock_table[i].read_lock[j] = 0;
		}
	}

	//initialize global tokens table
	for(unsigned i=0; i<MAX_THREADS;i++){
		tokens[i] = 0;
	}

	// initialize memory manager
	MemoryManager::GlobalInit();

	// initialize contention manager
	ContentionManager::GlobalInit();

	// init total thread count
	thread_count.val = 0;

/*	pthread_t gc;

	if (pthread_create(&gc, NULL, GC, NULL)) {
	  fprintf(stderr, "Error creating thread\n");
	  exit(1);
	}*/
}

inline void wlpdstm::TransactionNors::PrintConfiguration(FILE *out_file) {
	fprintf(out_file, "\nConfiguration:\n");
	fprintf(out_file, "\tLockExtent: %d\n", LOCK_EXTENT);
	fprintf(out_file, "\tRWLockTableSize: %d\n", RWLOCK_TABLE_SIZE);
	fprintf(out_file, "\tMaxThreads: %d\n", MAX_THREADS);
	//fprintf(out_file, "\tSizeOfRWLock: %u\n", (unsigned)sizeof(ReadWriteLock));
}

inline void wlpdstm::TransactionNors::ThreadInit() {
	// add itself to the transaction array
	transactions[tid.Get()] = this;

	// initialize memory manager
	mm.ThreadInit(tid.Get());

	// initialize contention manager
	cm.ThreadInit(tid.Get());

	// increment count of running threads
	// not safe to use fetch_and_increment on thread_count
	Word my_count = tid.Get() + 1;
	Word curr_count = thread_count.val;
	
	while(my_count > curr_count) {
		if(atomic_cas_no_barrier(&thread_count.val, curr_count, my_count)) {
			break;
		}
		
		curr_count = thread_count.val;
	}

#ifdef WAIT_ON_SUCC_ABORTS
	succ_aborts = 0;
#endif /* WAIT_ON_SUCC_ABORTS */

	profiling.ThreadInit(&random);
}

inline void wlpdstm::TransactionNors::TxStart(int lex_tx_id) {

	// initialize lexical tx id
	stats.lexical_tx_id = lex_tx_id;

	// notify cm of tx start
	cm.TxStart();
	
	// start mm transaction
	mm.TxStart();
}

inline void wlpdstm::TransactionNors::TxCommit() {

	for(WriteLog::iterator curr = write_log.begin();curr.hasNext();curr.next()) {
		WriteLogEntry &entry = *curr;

		*entry.write_lock = WRITE_LOCK_CLEAR;
	}

	write_log.clear();
	write_word_log_mem_pool.clear();

	// notify cm of tx commit
	cm.TxCommit();

	// commit mm transaction
	mm.TxCommit();

	stats.IncrementStatistics(StatisticsType::COMMIT);

#ifdef WAIT_ON_SUCC_ABORTS
	succ_aborts = 0;
#endif /* WAIT_ON_SUCC_ABORTS */

	tokens[tid.Get()]++;
}

inline void wlpdstm::TransactionNors::AbortCleanup() {
	Rollback();
	cm.TxAbort();
}

inline void wlpdstm::TransactionNors::TxAbort() {
	AbortCleanup();
	AbortJump();
}

inline void wlpdstm::TransactionNors::RestartCleanup(RestartCause cause) {
	Rollback();
	cm.TxRestart();
	
#ifdef WAIT_ON_SUCC_ABORTS
	if(cause != RESTART_EXTERNAL) {
		if(++succ_aborts > SUCC_ABORTS_MAX) {
			succ_aborts = SUCC_ABORTS_MAX;
		}
		
		if(succ_aborts >= SUCC_ABORTS_THRESHOLD) {
			WaitOnAbort();
		}
	}
#endif /* WAIT_ON_SUCC_ABORTS */
}

inline void wlpdstm::TransactionNors::TxRestart(RestartCause cause) {
	RestartCleanup(cause);
	RestartJump();	
}

#ifdef WAIT_ON_SUCC_ABORTS
inline void wlpdstm::TransactionNors::WaitOnAbort() {
	uint64_t cycles_to_wait = random.Get() % (succ_aborts * WAIT_CYCLES_MULTIPLICATOR);
	wait_cycles(cycles_to_wait);
	stats.IncrementStatistics(StatisticsType::WAIT_ON_ABORT);
}
#endif /* WAIT_ON_SUCC_ABORTS */


inline void wlpdstm::TransactionNors::Rollback() {
	for(WriteLog::iterator curr = write_log.begin();curr.hasNext();curr.next()) {
		WriteLogEntry &entry = *curr;

		WriteWordLogEntry *word_log_entry = entry.head;

		while(word_log_entry != NULL) {
			*word_log_entry->address = MaskWord(word_log_entry);
			word_log_entry = word_log_entry->next;
		}

		*entry.write_lock = WRITE_LOCK_CLEAR;
	}

	write_log.clear();
	write_word_log_mem_pool.clear();

	// commit mm transaction
	mm.TxAbort();

	stats.IncrementStatistics(StatisticsType::ABORT);

	tokens[tid.Get()]++;
}

inline void wlpdstm::TransactionNors::AbortJump() {
#ifdef WLPDSTM_ICC
	jmp_to_begin_transaction(&start_buf);
#else	
	siglongjmp(start_buf, LONG_JMP_ABORT_FLAG);
#endif /* WLPDSTM_ICC */
}

inline void wlpdstm::TransactionNors::RestartJump() {
#ifdef WLPDSTM_ICC
	jmp_to_begin_transaction(&start_buf);
#else
	siglongjmp(start_buf, LONG_JMP_RESTART_FLAG);
#endif /* WLPDSTM_ICC */
}

inline Word wlpdstm::TransactionNors::ReadWord(Word *address) {
	ReadWriteLock *rwlock = rwlock_table + map_address_to_index(address);

	//atomic_store_full(&rwlock->read_lock[tid.Get()], tokens[tid.Get()]);

	WriteLogEntry *log_entry = (WriteLogEntry *)rwlock->write_lock;

	//if meanwhile a writer has acquired the lock, we abort
	if(log_entry != WRITE_LOCK_CLEAR){
	    if(log_entry->owner != this){
			stats.IncrementStatistics(StatisticsType::ABORT_READ_WRITE);
			TxRestart(RESTART_READ_WRITE);
		}
	}

	return *address;
}

inline void wlpdstm::TransactionNors::WriteWord(Word *addr, Word val, Word mask) {
	ReadWriteLock *rwlock = rwlock_table + map_address_to_index(addr);

	// LockMemoryStripe will restart if it cannot lock
	LockMemoryStripe(addr, rwlock);

	//insert old value into the undo-log
	((WriteLogEntry *)rwlock->write_lock)->InsertWordLogEntry(addr, *addr, mask);

	// then update the value in-place
	*addr = MaskWord(*addr, val, mask);
}

inline void wlpdstm::TransactionNors::LockMemoryStripe(Word *addr, ReadWriteLock *rwlock) {
	WriteLogEntry *log_entry = (WriteLogEntry *)rwlock->write_lock;

	//if locked by me return quickly
	if(log_entry && log_entry->owner == this)
		return;

	log_entry = write_log.get_next();
	log_entry->write_lock = &rwlock->write_lock;
	log_entry->ClearWordLogEntries();
	log_entry->owner = this;

	//There can be only one writer per address at a time
	if(!atomic_cas_no_barrier(&rwlock->write_lock, WRITE_LOCK_CLEAR, log_entry)){
		write_log.delete_last();

		stats.IncrementStatistics(StatisticsType::ABORT_WRITE_WRITE);
		TxRestart(RESTART_WRITE_WRITE);
	}

	//abort if there is a previous running reader
	for(Word i=0; i<thread_count.val; i++){
		if(i != tid.Get() && rwlock->read_lock[i] == tokens[i]){
			write_log.delete_last();

			*log_entry->write_lock = WRITE_LOCK_CLEAR;

			stats.IncrementStatistics(StatisticsType::ABORT_WRITE_READ);
			TxRestart(RESTART_WRITE_READ);
		}
	}
}

// mask contains ones where value bits are valid
inline Word wlpdstm::TransactionNors::MaskWord(Word old, Word val, Word mask) {
	if(mask == LOG_ENTRY_UNMASKED) {
		return val;
	}
	
	return (old & ~mask) | (val & mask);
}

inline Word wlpdstm::TransactionNors::MaskWord(WriteWordLogEntry *entry) {
	return MaskWord(*entry->address, entry->value, entry->mask);
}

inline wlpdstm::TransactionNors::WriteWordLogEntry *wlpdstm::TransactionNors::WriteLogEntry::FindWordLogEntry(Word *address) {
	WriteWordLogEntry *curr = head;

	while(curr != NULL) {
		if(curr->address == address) {
			break;
		}

		curr = curr->next;
	}

	return curr;
}

inline void wlpdstm::TransactionNors::WriteLogEntry::InsertWordLogEntry(Word *address, Word value, Word mask) {
	WriteWordLogEntry *entry = FindWordLogEntry(address);

	// new entry
	if(entry == NULL) {
		entry = owner->write_word_log_mem_pool.get_next();
		entry->address = address;
		entry->next = head;
		entry->value = value;
		entry->mask = mask;
		head = entry;
	}
}

inline void wlpdstm::TransactionNors::WriteLogEntry::ClearWordLogEntries() {
	head = NULL;
}

inline void *wlpdstm::TransactionNors::TxMalloc(size_t size) {
	return mm.TxMalloc(size);
}

inline void wlpdstm::TransactionNors::TxFree(void *ptr, size_t size) {
	LockMemoryBlock(ptr, size);
	mm.TxFree(ptr);
}

inline void wlpdstm::TransactionNors::LockMemoryBlock(void *address, size_t size) {
	uintptr_t start = (uintptr_t)address;
	uintptr_t end = start + size;
	ReadWriteLock *curr, *old = NULL;
	
	for(uintptr_t address = start;address < end;address++) {
		curr = &rwlock_table[map_address_to_index((Word *)address)];
		
		if(curr != old) {
			LockMemoryStripe((Word *)address, curr);
			old = curr;
		}
	}	
}

// TODO think about moving this out of particular STM implementation
inline void wlpdstm::TransactionNors::PrintStatistics() {
#ifdef COLLECT_STATS
	FILE *out_file = stdout;
	fprintf(out_file, "\n");
	fprintf(out_file, "STM internal statistics: \n");
	fprintf(out_file, "========================\n");
	
	// collect stats in a single collection
	ThreadStatisticsCollection stat_collection;
	
	for(unsigned i = 0;i < thread_count.val;i++) {
		// these should all be initialized at this point
		stat_collection.Add(&(transactions[i]->stats));
		fprintf(out_file, "Thread %d: \n", i + 1);
		transactions[i]->stats.Print(out_file, 1);
		fprintf(out_file, "\n");
	}
	
	fprintf(out_file, "Total stats: \n");
	ThreadStatistics total_stats = stat_collection.MergeAll();
	total_stats.Print(out_file, 1);
#endif /* COLLECT_STATS */
}

inline unsigned wlpdstm::TransactionNors::map_address_to_index(Word *address) {
	return ((uintptr_t)address >> (LOCK_EXTENT)) & (RWLOCK_TABLE_SIZE - 1);
}

inline void wlpdstm::TransactionNors::ThreadShutdown() {
	// nothing
}

inline void wlpdstm::TransactionNors::GlobalShutdown() {
	PrintStatistics();
}

inline void wlpdstm::TransactionNors::StartThreadProfiling() {
    // nothing
}

inline void wlpdstm::TransactionNors::EndThreadProfiling() {
    // nothing
}

#endif /* WLPDSTM_RW_TRANSACTION_H_ */
