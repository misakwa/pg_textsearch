/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * filter.c - Heap TIDs of an equality filter, read from a btree
 *
 * A filtered BM25 top-k scan needs every heap TID that the filter's
 * btree lists for one value, sorted in CTID order, the order of each
 * segment's doc_ids.  The btree is read at its leaf level directly:
 * one descent to the first entry with the value, then the leaf pages to
 * the right, copying TIDs (posting lists whole) until an entry differs.
 * That costs a few nanoseconds per TID where index_getnext_tid costs
 * about a hundred.  Entries of one value are ordered by the btree's
 * other key columns, not by TID, so the TIDs are sorted afterwards.
 *
 * Readers follow nbtree's protocol for scans: one share-locked leaf at
 * a time, stepping right by the sibling link read under the lock, and
 * past deleted or half-dead pages.  The TIDs are a superset of the
 * visible matching rows (dead entries are kept), as from any index
 * scan; the executor's Filter rechecks every row returned.
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/htup_details.h>
#include <access/nbtree.h>
#include <access/relscan.h>
#include <access/skey.h>
#include <access/xact.h>
#include <catalog/pg_index.h>
#include <miscadmin.h>
#include <pgstat.h>
#include <port/pg_bitutils.h>
#include <storage/bufmgr.h>
#include <utils/memutils.h>
#include <utils/rel.h>

#include "access/filter.h"

#define TP_FILTER_RADIX_BITS 11
#define TP_FILTER_RADIX_MIN	 256

/* Packed TIDs, (block << 16) | offset, in arrival order. */
typedef struct TidKeys
{
	uint64 *keys;
	int		n;
	int		alloced;
	int		cap;
} TidKeys;

static inline uint64
tid_key(const ItemPointerData *tid)
{
	return ((uint64)ItemPointerGetBlockNumberNoCheck(tid) << 16) |
		   ItemPointerGetOffsetNumberNoCheck(tid);
}

/* Append n TIDs; false when that would pass the cap. */
static bool
keys_add(TidKeys *k, const ItemPointerData *tids, int n)
{
	if (n > k->cap - k->n)
		return false;
	if (k->n + n > k->alloced)
	{
		int want = Min(Max(Max(k->alloced * 2, k->n + n), 1024), k->cap);

		k->keys	   = k->keys == NULL ? palloc(want * sizeof(uint64))
									 : repalloc(k->keys, want * sizeof(uint64));
		k->alloced = want;
	}
	for (int i = 0; i < n; i++)
		k->keys[k->n++] = tid_key(&tids[i]);
	return true;
}

#define ST_SORT			 tp_sort_tid_keys
#define ST_ELEMENT_TYPE	 uint64
#define ST_COMPARE(a, b) (*(a) < *(b) ? -1 : *(a) > *(b))
#define ST_SCOPE		 static
#define ST_DECLARE
#define ST_DEFINE
#include <lib/sort_template.h>

/*
 * Sort packed TIDs: no work when they already ascend, a comparison sort
 * when few, else a least-significant-digit radix sort over the bits the
 * keys use (about 30 for a table of a few GB: three passes).
 */
static void
sort_keys(uint64 *keys, int n)
{
	uint64	used   = 0;
	bool	sorted = true;
	uint64 *src	   = keys;
	uint64 *dst;
	int		bits;

	for (int i = 0; i < n; i++)
	{
		used |= keys[i];
		if (i > 0 && keys[i] < keys[i - 1])
			sorted = false;
	}
	if (sorted)
		return;
	if (n < TP_FILTER_RADIX_MIN)
	{
		tp_sort_tid_keys(keys, n);
		return;
	}

	bits = pg_leftmost_one_pos64(used) + 1;
	dst	 = palloc(n * sizeof(uint64));
	for (int shift = 0; shift < bits; shift += TP_FILTER_RADIX_BITS)
	{
		uint32	count[1 << TP_FILTER_RADIX_BITS] = {0};
		uint32	pos								 = 0;
		uint64 *swap;

		for (int i = 0; i < n; i++)
			count[(src[i] >> shift) & ((1 << TP_FILTER_RADIX_BITS) - 1)]++;
		for (int d = 0; d < (1 << TP_FILTER_RADIX_BITS); d++)
		{
			uint32 c = count[d];

			count[d] = pos;
			pos += c;
		}
		for (int i = 0; i < n; i++)
			dst
					[count[(src[i] >> shift) &
						   ((1 << TP_FILTER_RADIX_BITS) - 1)]++] = src[i];
		swap = src;
		src	 = dst;
		dst	 = swap;
	}
	if (src != keys)
	{
		memcpy(keys, src, n * sizeof(uint64));
		pfree(src);
	}
	else
		pfree(dst);
}

/*
 * The btree's insertion scankey for its first column equal to value,
 * with the opclass's ordering function, as _bt_first builds one.
 */
static BTScanInsert
filter_insertion_key(Relation btree, Datum value)
{
	BTScanInsert key = _bt_mkscankey(btree, NULL);

	_bt_metaversion(btree, &key->heapkeyspace, &key->allequalimage);
	key->keysz		 = 1;
	key->anynullkeys = false;
	key->scankeys[0].sk_flags &= ~SK_ISNULL;
	key->scankeys[0].sk_argument = value;
	return key;
}

/* First offset in [low, high] whose item is >= key (> key if after). */
static OffsetNumber
leaf_bound(
		Relation	 btree,
		BTScanInsert key,
		Page		 page,
		OffsetNumber low,
		OffsetNumber high,
		bool		 after)
{
	high++;
	while (low < high)
	{
		OffsetNumber mid = low + (high - low) / 2;
		int32		 cmp = _bt_compare(btree, key, page, mid);

		if (cmp > 0 || (after && cmp == 0))
			low = mid + 1;
		else
			high = mid;
	}
	return low;
}

/*
 * Copy the heap TIDs of leaf items [first, last]; false past the cap.
 * Items killed as dead to every snapshot are left out unless the
 * transaction began in recovery, as nbtree's own scans do.
 */
static bool
leaf_add_items(
		TidKeys		*k,
		Page		 page,
		OffsetNumber first,
		OffsetNumber last,
		bool		 include_dead)
{
	for (OffsetNumber off = first; off <= last; off++)
	{
		ItemId	   iid	= PageGetItemId(page, off);
		IndexTuple itup = (IndexTuple)PageGetItem(page, iid);

		if (!include_dead && ItemIdIsDead(iid))
			continue;

		if (BTreeTupleIsPosting(itup))
		{
			if (!keys_add(
						k,
						BTreeTupleGetPosting(itup),
						BTreeTupleGetNPosting(itup)))
				return false;
		}
		else if (!keys_add(k, &itup->t_tid, 1))
			return false;
	}
	return true;
}

/*
 * Whether the first entry of the leftmost leaf and the last entry of the
 * rightmost leaf both equal key: then so does every entry between them,
 * and the filter removes no row.  An empty end page answers no.
 */
static bool
btree_all_equal(Relation btree, BTScanInsert key)
{
	Buffer		 buf;
	Page		 page;
	BTPageOpaque opaque;
	OffsetNumber off;
	bool		 equal;

	buf = _bt_get_endpoint(btree, 0, false);
	if (!BufferIsValid(buf))
		return false;
	page   = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);
	off	   = P_FIRSTDATAKEY(opaque);
	equal  = off <= PageGetMaxOffsetNumber(page) &&
			_bt_compare(btree, key, page, off) == 0;
	_bt_relbuf(btree, buf);
	if (!equal)
		return false;

	buf = _bt_get_endpoint(btree, 0, true);
	if (!BufferIsValid(buf))
		return false;
	page   = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);
	off	   = PageGetMaxOffsetNumber(page);
	equal  = off >= P_FIRSTDATAKEY(opaque) &&
			_bt_compare(btree, key, page, off) == 0;
	_bt_relbuf(btree, buf);
	return equal;
}

/* Leaf-level read of every entry equal to key. */
static TpFilterRead
read_leaves(Relation heap, Relation btree, BTScanInsert key, TidKeys *k)
{
	Buffer		 buf;
	Page		 page;
	BTPageOpaque opaque;
	BTStack		 stack;
	bool		 first_page	  = true;
	bool		 include_dead = TransactionStartedDuringRecovery();

	pgstat_count_index_scan(btree);
	stack = _bt_search(btree, heap, key, &buf, BT_READ);
	_bt_freestack(stack);
	if (!BufferIsValid(buf))
		return TP_FILTER_READ_OK;

	for (;;)
	{
		OffsetNumber low;
		OffsetNumber maxoff;
		OffsetNumber off;
		OffsetNumber end;
		BlockNumber	 next;

		page   = BufferGetPage(buf);
		opaque = BTPageGetOpaque(page);
		low	   = P_FIRSTDATAKEY(opaque);
		maxoff = PageGetMaxOffsetNumber(page);

		if (!P_IGNORE(opaque) && low <= maxoff)
		{
			/*
			 * Past the first page the run of equal entries continues from
			 * the page's first item, so one comparison places it.
			 */
			if (!first_page && _bt_compare(btree, key, page, low) == 0)
				off = low;
			else
				off = leaf_bound(btree, key, page, low, maxoff, false);
			first_page = false;

			if (off <= maxoff)
			{
				bool to_end = _bt_compare(btree, key, page, maxoff) == 0;

				end = to_end ? maxoff + 1
							 : leaf_bound(btree, key, page, off, maxoff, true);
				if (end > off &&
					!leaf_add_items(k, page, off, end - 1, include_dead))
				{
					_bt_relbuf(btree, buf);
					return TP_FILTER_READ_OVERFLOW;
				}
				if (!to_end)
					break;
			}
		}
		if (P_RIGHTMOST(opaque))
			break;
		next = opaque->btpo_next;
		_bt_relbuf(btree, buf);
		CHECK_FOR_INTERRUPTS();
		buf = _bt_getbuf(btree, next, BT_READ);
	}
	_bt_relbuf(btree, buf);
	return TP_FILTER_READ_OK;
}

/* Read through the index AM, for keys the leaf read cannot position. */
static TpFilterRead
read_scan(
		Relation				 heap,
		Relation				 btree,
		const TpQueryFilterHint *hint,
		Datum					 value,
		Snapshot				 snapshot,
		TidKeys					*k)
{
	IndexScanDesc bscan;
	ScanKeyData	  key;
	ItemPointer	  tid;
	TpFilterRead  result = TP_FILTER_READ_OK;

	ScanKeyEntryInitialize(
			&key,
			0,
			1,
			BTEqualStrategyNumber,
			hint->subtype,
			hint->collation,
			hint->proc_oid,
			value);
#if PG_VERSION_NUM >= 180000
	bscan = index_beginscan(heap, btree, snapshot, NULL, 1, 0);
#else
	bscan = index_beginscan(heap, btree, snapshot, 1, 0);
#endif
	index_rescan(bscan, &key, 1, NULL, 0);
	while ((tid = index_getnext_tid(bscan, ForwardScanDirection)) != NULL)
	{
		if (!keys_add(k, tid, 1))
		{
			result = TP_FILTER_READ_OVERFLOW;
			break;
		}
	}
	index_endscan(bscan);
	return result;
}

TpFilterRead
tp_filter_read_tids(
		Relation				 heap,
		Relation				 btree,
		const TpQueryFilterHint *hint,
		Datum					 value,
		Snapshot				 snapshot,
		int						 max_rows,
		ItemPointer				*tids_out,
		int						*count_out)
{
	TidKeys k =
			{NULL, 0, 0, Min(max_rows, (int)(MaxAllocSize / sizeof(uint64)))};
	TpFilterRead result;
	ItemPointer	 tids;
	int			 w;

	*tids_out  = NULL;
	*count_out = 0;

	/*
	 * The opclass's ordering function compares two values of its input
	 * type; a cross-type clause goes through the index AM instead.
	 */
	if (btree->rd_opcintype[0] == hint->subtype)
	{
		BTScanInsert key = filter_insertion_key(btree, value);

		/* A partial btree lists only some rows, so it can't show all match. */
		if (heap_attisnull(
					btree->rd_indextuple, Anum_pg_index_indpred, NULL) &&
			btree_all_equal(btree, key))
			result = TP_FILTER_READ_ALL;
		else
			result = read_leaves(heap, btree, key, &k);
		pfree(key);
	}
	else
		result = read_scan(heap, btree, hint, value, snapshot, &k);

	if (result != TP_FILTER_READ_OK || k.n == 0)
	{
		if (k.keys)
			pfree(k.keys);
		return result;
	}

	sort_keys(k.keys, k.n);
	tids = palloc(k.n * sizeof(ItemPointerData));
	w	 = 0;
	for (int r = 0; r < k.n; r++)
		if (r == 0 || k.keys[r] != k.keys[r - 1])
			ItemPointerSet(
					&tids[w++],
					(BlockNumber)(k.keys[r] >> 16),
					(OffsetNumber)k.keys[r]);
	pfree(k.keys);
	*tids_out  = tids;
	*count_out = w;
	return TP_FILTER_READ_OK;
}
