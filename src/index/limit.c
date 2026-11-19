/* Query LIMIT optimization shared by the planner and index scan. */
#include <postgres.h>

#include <math.h>

#include "constants.h"
#include "index/limit.h"

int tp_default_limit = TP_DEFAULT_QUERY_LIMIT;

int
tp_seed_limit_for_filter(int user_limit, double selectivity)
{
	double seeded;

	if (!tp_filtered_seed || selectivity <= 0.0 || selectivity >= 1.0)
		return user_limit;
	seeded = ceil(tp_filtered_seed_margin * (double)user_limit / selectivity);
	if (seeded > (double)TP_MAX_INITIAL_QUERY_LIMIT)
		seeded = TP_MAX_INITIAL_QUERY_LIMIT;
	return seeded > user_limit ? (int)seeded : user_limit;
}

/*
 * Filter pushdown cost rule.  The unfiltered scan ranks about k * N / rows
 * documents to find k filter rows, twice as many when it re-drives (each
 * pass doubles the batch and starts over); only a seeded scan finishes in
 * one pass.  Pushdown costs one unit per filter row plus SPAN_COST per
 * segment document between the filter's first and last row, which the
 * CTID merge walks.  cost_ratio prices one ranked document in those units,
 * so pushdown wins while rows * (rows + SPAN_COST * span) <= budget.
 */
static double
tp_filter_pushdown_budget(double k, double reltuples, bool one_pass)
{
	if (k <= 0)
		k = tp_default_limit;
	return tp_filter_pushdown_cost_ratio * k * reltuples * (one_pass ? 1 : 2);
}

/*
 * Largest row count the rule allows before the btree is read, with the
 * span estimated from N (scattered) to rows (clustered) by correlation
 * squared, as in btree costing.
 */
double
tp_filter_pushdown_row_limit(
		double k, double reltuples, double correlation, bool one_pass)
{
	double c2 = correlation * correlation;
	double a  = 1.0 + TP_FILTER_PUSHDOWN_SPAN_COST * c2;
	double b  = TP_FILTER_PUSHDOWN_SPAN_COST * reltuples * (1.0 - c2);
	double c  = tp_filter_pushdown_budget(k, reltuples, one_pass);

	if (tp_filter_pushdown_cost_ratio <= 0)
		return 0;
	return (sqrt(b * b + 4.0 * a * c) - b) / (2.0 * a);
}

/* The rule with the row count and span known after the btree read. */
bool
tp_filter_pushdown_cheaper(
		double rows, double span, double k, double reltuples, bool one_pass)
{
	return rows * (rows + TP_FILTER_PUSHDOWN_SPAN_COST * span) <=
		   tp_filter_pushdown_budget(k, reltuples, one_pass);
}
