/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * filter.h - Heap TIDs of an equality filter, read from a btree
 */
#pragma once

#include <postgres.h>

#include <storage/itemptr.h>
#include <utils/rel.h>
#include <utils/snapshot.h>

#include "types/query.h"

typedef enum TpFilterRead
{
	TP_FILTER_READ_OK,		 /* *tids_out holds the matching TIDs */
	TP_FILTER_READ_ALL,		 /* every btree entry matches: no filter */
	TP_FILTER_READ_OVERFLOW, /* more than max_rows TIDs (or than palloc takes)
							  */
} TpFilterRead;

/*
 * Read the heap TIDs whose btree entries match hint's clause, sorted and
 * unique, into *tids_out (palloc'd in the current context, NULL when
 * none).  The btree is opened and locked by the caller.
 */
extern TpFilterRead tp_filter_read_tids(
		Relation				 heap,
		Relation				 btree,
		const TpQueryFilterHint *hint,
		Datum					 value,
		Snapshot				 snapshot,
		int						 max_rows,
		ItemPointer				*tids_out,
		int						*count_out);
