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
 * Largest filter row count for which pushdown should beat the unfiltered
 * scan.  That scan ranks about k * N / rows documents to find k filter
 * rows.  Pushdown costs one unit per filter row plus SPAN_COST per
 * segment document between the filter's first and last row, a span
 * interpolated from N (scattered) to rows (clustered) by correlation
 * squared, as btree costing does.  cost_ratio prices one ranked document
 * in those units.  Solves rows * (rows + SPAN_COST * span) =
 * cost_ratio * k * N for rows.
 */
double
tp_filter_pushdown_row_limit(double k, double reltuples, double correlation)
{
	double c2 = correlation * correlation;
	double a  = 1.0 + TP_FILTER_PUSHDOWN_SPAN_COST * c2;
	double b  = TP_FILTER_PUSHDOWN_SPAN_COST * reltuples * (1.0 - c2);
	double c;

	if (tp_filter_pushdown_cost_ratio <= 0)
		return 0;
	if (k <= 0)
		k = tp_default_limit;
	c = tp_filter_pushdown_cost_ratio * k * reltuples;
	return (sqrt(b * b + 4.0 * a * c) - b) / (2.0 * a);
}
