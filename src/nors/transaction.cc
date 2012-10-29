/**
 * @author Aleksandar Dragojevic aleksandar.dragojevic@epfl.ch
 *
 */

#include "transaction.h"

CACHE_LINE_ALIGNED wlpdstm::TransactionNors::ReadWriteLock wlpdstm::TransactionNors::rwlock_table[RWLOCK_TABLE_SIZE];

CACHE_LINE_ALIGNED wlpdstm::TransactionNors *wlpdstm::TransactionNors::transactions[MAX_THREADS];

CACHE_LINE_ALIGNED wlpdstm::PaddedWord wlpdstm::TransactionNors::thread_count;

CACHE_LINE_ALIGNED wlpdstm::TransactionNors::Token wlpdstm::TransactionNors::tokens[MAX_THREADS];
