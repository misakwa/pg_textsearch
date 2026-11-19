-- Test: filter pushdown into BM25 top-k index scans
--
-- A BM25 top-k scan with a Filter col = constant, where col leads a
-- btree of the same table, reads that btree once and scores only the
-- rows it lists (pg_textsearch.filter_pushdown).  Pushdown changes
-- which documents the scan scores, never which rows win or their
-- scores, so every check compares against the unpushed scan or an
-- independent oracle:
--   * parity   - pushdown on == off: same ids, same order, equal scores
--   * oracle   - with k above the match count, the filtered scan returns
--                exactly the filtered rows containing a query term
--   * passes   - a small filtered set needs one scoring pass
--   * sources  - two segments plus the memtable, HOT and non-HOT updates
--   * re-drive - rows lost above the scan (a join) still re-drive it
--   * fallback - above pg_textsearch.filter_pushdown_max_rows
--   * RLS      - user clauses behind a policy are not pushed
--   * identity - two BM25 scans in one statement use their own filters

SET log_duration = off;
SET client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_textsearch;
SET enable_seqscan = false;
SET enable_bitmapscan = false;

------------------------------------------------------------------------
-- Setup: 14,000 docs over 60 collections (coll = id % 60) plus three
-- small ones, words drawn from a 40-word vocabulary.  Two batches are
-- spilled to segments and the third stays in the memtable.
------------------------------------------------------------------------

CREATE TABLE fp_docs (
    id    int PRIMARY KEY,
    coll  int NOT NULL,
    owner text NOT NULL,
    body  text
) WITH (fillfactor = 50);

CREATE INDEX fp_docs_coll ON fp_docs (coll, id);
CREATE INDEX fp_docs_bm25 ON fp_docs USING bm25 (body)
    WITH (text_config = 'simple');

CREATE FUNCTION fp_body(g int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
    SELECT string_agg('w' || ((hashint4(g * 16 + i) & 1023) % 40), ' ')
    FROM generate_series(1, 3 + g % 10) i
$$;

CREATE FUNCTION fp_coll(g int) RETURNS int LANGUAGE sql IMMUTABLE AS $$
    SELECT CASE WHEN g IN (5, 7001, 13001) THEN 200
                WHEN g % 97 = 0 THEN 100
                ELSE g % 60 END
$$;

INSERT INTO fp_docs
SELECT g, fp_coll(g), CASE WHEN g % 2 = 0 THEN 'fp_reader' ELSE 'other' END,
       fp_body(g)
FROM generate_series(1, 6000) g;
SELECT bm25_spill_index('fp_docs_bm25') IS NOT NULL AS spilled;

INSERT INTO fp_docs
SELECT g, fp_coll(g), CASE WHEN g % 2 = 0 THEN 'fp_reader' ELSE 'other' END,
       fp_body(g)
FROM generate_series(6001, 12000) g;
SELECT bm25_spill_index('fp_docs_bm25') IS NOT NULL AS spilled;

INSERT INTO fp_docs
SELECT g, fp_coll(g), CASE WHEN g % 2 = 0 THEN 'fp_reader' ELSE 'other' END,
       fp_body(g)
FROM generate_series(12001, 14000) g;

ANALYZE fp_docs;

-- The plan under test: a BM25 index scan with the collection as Filter.
EXPLAIN (COSTS OFF)
SELECT id FROM fp_docs WHERE coll = 100
ORDER BY body <@> to_bm25query('w1 w2', 'fp_docs_bm25') LIMIT 10;

-- The scan reads the btree once and reports how many rows it allows.
SET client_min_messages = NOTICE;
SET pg_textsearch.log_filter_pushdown = on;
SELECT count(*) FROM (
    SELECT id FROM fp_docs WHERE coll = 100
    ORDER BY body <@> to_bm25query('w1 w2', 'fp_docs_bm25') LIMIT 10) s;
RESET pg_textsearch.log_filter_pushdown;
SET client_min_messages = WARNING;

------------------------------------------------------------------------
-- Helpers
------------------------------------------------------------------------

-- The top-k as "id:score" in result order, with pushdown on or off.
CREATE FUNCTION fp_list(qry text, pred text, k int, push bool)
RETURNS text[] LANGUAGE plpgsql AS $$
DECLARE
    r text[];
BEGIN
    PERFORM set_config('pg_textsearch.filter_pushdown', push::text, true);
    EXECUTE format(
        'SELECT coalesce(array_agg(id || '':'' || s::text ORDER BY n), '
        '                ARRAY[]::text[]) '
        'FROM (SELECT id, s, row_number() OVER () AS n FROM '
        '      (SELECT id, body <@> to_bm25query(%L, ''fp_docs_bm25'') AS s '
        '       FROM fp_docs WHERE %s '
        '       ORDER BY body <@> to_bm25query(%L, ''fp_docs_bm25'') '
        '       LIMIT %s) t) u', qry, pred, qry, k)
    INTO r;
    RETURN r;
END;
$$;

CREATE FUNCTION fp_check(qry text, pred text, k int) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    pushed   text[];
    unpushed text[];
BEGIN
    pushed   := fp_list(qry, pred, k, true);
    unpushed := fp_list(qry, pred, k, false);
    IF pushed IS NOT DISTINCT FROM unpushed THEN
        RETURN format('PASS (%s rows)', coalesce(array_length(pushed, 1), 0));
    END IF;
    RETURN format('FAIL pushed=%s unpushed=%s', pushed, unpushed);
END;
$$;

-- The ids of the filtered scan with k above the match count, against
-- the filtered rows that contain any query term.
CREATE FUNCTION fp_oracle(qry text, pred text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    idx int[];
    ora int[];
BEGIN
    -- Keep the BM25 scan: a btree scan plus Sort would score every row.
    SET LOCAL enable_sort = off;
    EXECUTE format(
        'SELECT coalesce(array_agg(id ORDER BY id), ARRAY[]::int[]) '
        'FROM (SELECT id FROM fp_docs WHERE %s '
        '      ORDER BY body <@> to_bm25query(%L, ''fp_docs_bm25'') '
        '      LIMIT 100000) s', pred, qry)
    INTO idx;
    EXECUTE format(
        'SELECT coalesce(array_agg(id ORDER BY id), ARRAY[]::int[]) '
        'FROM fp_docs WHERE (%s) AND string_to_array(body, '' '') && '
        '      string_to_array(%L, '' '')', pred, qry)
    INTO ora;
    IF idx IS NOT DISTINCT FROM ora THEN
        RETURN format('PASS (%s rows)', coalesce(array_length(idx, 1), 0));
    END IF;
    RETURN format('FAIL index=%s oracle=%s', idx, ora);
END;
$$;

-- Scoring passes of one filtered top-k with pushdown on.
CREATE FUNCTION fp_passes(qry text, pred text, k int) RETURNS bigint
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM bm25_debug_scoring_passes(true);
    PERFORM fp_list(qry, pred, k, true);
    RETURN bm25_debug_scoring_passes(true);
END;
$$;

------------------------------------------------------------------------
-- Parity and oracle over single- and multi-term queries
------------------------------------------------------------------------

SELECT n, qry, pred, k, fp_check(qry, pred, k) AS result
FROM (VALUES
    (1,  'w1',             'coll = 100',          10),
    (2,  'w1 w2',          'coll = 100',          10),
    (3,  'w3 w7 w11',      'coll = 100',          50),
    (4,  'w3 w7 w11 w19',  'coll = 7',            10),
    (5,  'w5',             'coll = 7',           300),
    (6,  'w0 w1 w2 w3 w4', 'coll = 200',          10),
    (7,  'w2 w9',          '100 = coll',          10),
    (8,  'w2 w9',          'coll = 100::bigint',  10),
    (9,  'w2 w9',          'coll = 100 AND id % 3 = 0', 10),
    (10, 'w39 w38',        'coll = 12',            1),
    (11, 'w4 w8',          'coll = 999',          10),
    (12, 'w1 w2',          'coll IN (7, 100)',    10)
) v(n, qry, pred, k)
ORDER BY n;

SELECT n, qry, pred, fp_oracle(qry, pred) AS result
FROM (VALUES
    (1, 'w1',             'coll = 100'),
    (2, 'w1 w2 w3',       'coll = 100'),
    (3, 'w6 w17',         'coll = 31'),
    (4, 'w0 w1 w2 w3 w4', 'coll = 200'),
    (5, 'w12',            'coll = 59')
) v(n, qry, pred)
ORDER BY n;

-- One scoring pass when the filtered set is smaller than the batch.
SELECT fp_passes('w1 w2 w3', 'coll = 100', 10) AS passes_small,
       fp_passes('w1 w2 w3', 'coll = 7', 10) AS passes_collection;

------------------------------------------------------------------------
-- Updates: HOT (unindexed column) and non-HOT (collection moves)
------------------------------------------------------------------------

UPDATE fp_docs SET owner = owner || '_x' WHERE coll = 100 AND id % 2 = 0;
UPDATE fp_docs SET coll = 7 WHERE coll = 100 AND id % 3 = 0;
UPDATE fp_docs SET body = body || ' w1' WHERE coll = 100 AND id % 5 = 0;

SELECT n, qry, pred, k, fp_check(qry, pred, k) AS result
FROM (VALUES
    (1, 'w1',       'coll = 100', 10),
    (2, 'w1 w2',    'coll = 100', 20),
    (3, 'w1 w2',    'coll = 7',   20)
) v(n, qry, pred, k)
ORDER BY n;

SELECT n, qry, pred, fp_oracle(qry, pred) AS result
FROM (VALUES
    (1, 'w1',    'coll = 100'),
    (2, 'w1 w2', 'coll = 7')
) v(n, qry, pred)
ORDER BY n;

------------------------------------------------------------------------
-- Re-drive: a join between the Limit and the scan drops rows, so the
-- scan must return more than its first batch.  default_limit 5 forces
-- the re-drive with pushdown active.
------------------------------------------------------------------------

CREATE TABLE fp_acl (id int PRIMARY KEY, ok bool NOT NULL);
INSERT INTO fp_acl SELECT id, id % 4 = 0 FROM fp_docs;
ANALYZE fp_acl;

CREATE FUNCTION fp_acl_list(push bool) RETURNS int[] LANGUAGE plpgsql AS $$
DECLARE
    r int[];
BEGIN
    PERFORM set_config('pg_textsearch.filter_pushdown', push::text, true);
    SELECT array_agg(id ORDER BY n) INTO r FROM (
        SELECT id, row_number() OVER () AS n FROM (
            SELECT d.id FROM fp_docs d
            WHERE d.coll = 7
              AND EXISTS (SELECT 1 FROM fp_acl a WHERE a.id = d.id AND a.ok)
            ORDER BY d.body <@> to_bm25query('w1 w2', 'fp_docs_bm25'), d.id
            LIMIT 30) t) u;
    RETURN r;
END;
$$;

SET pg_textsearch.default_limit = 5;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT fp_acl_list(true) = fp_acl_list(false) AS acl_parity,
       array_length(fp_acl_list(true), 1) AS acl_rows;
SELECT bm25_debug_scoring_passes(true) >= 0 AS reset;
SELECT array_length(fp_acl_list(true), 1) AS rows,
       bm25_debug_scoring_passes(true) > 1 AS redriven;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET pg_textsearch.default_limit;

------------------------------------------------------------------------
-- Fallback above filter_pushdown_max_rows (planned and executed)
------------------------------------------------------------------------

SET client_min_messages = NOTICE;
SET pg_textsearch.log_filter_pushdown = on;
SET pg_textsearch.filter_pushdown_max_rows = 100;
SELECT count(*) FROM (
    SELECT id FROM fp_docs WHERE coll = 7
    ORDER BY body <@> to_bm25query('w1 w2', 'fp_docs_bm25') LIMIT 10) s;
RESET pg_textsearch.log_filter_pushdown;
SET client_min_messages = WARNING;
SELECT fp_check('w1 w2', 'coll = 7', 10) AS capped;

-- A plan made under a larger cap still falls back at execution.
SET plan_cache_mode = force_generic_plan;
PREPARE fp_q AS
    SELECT count(*) FROM (
        SELECT id FROM fp_docs WHERE coll = 7
        ORDER BY body <@> to_bm25query('w1 w2', 'fp_docs_bm25') LIMIT 10) s;
SET client_min_messages = NOTICE;
SET pg_textsearch.log_filter_pushdown = on;
SET pg_textsearch.filter_pushdown_max_rows = 100000;
EXECUTE fp_q;
SET pg_textsearch.filter_pushdown_max_rows = 100;
EXECUTE fp_q;
RESET pg_textsearch.log_filter_pushdown;
SET client_min_messages = WARNING;
DEALLOCATE fp_q;
RESET plan_cache_mode;
RESET pg_textsearch.filter_pushdown_max_rows;

------------------------------------------------------------------------
-- RLS: under a policy the user's clause carries a security level and
-- is not pushed; results equal the oracle restricted to the policy.
------------------------------------------------------------------------

CREATE ROLE fp_reader;
GRANT SELECT ON fp_docs TO fp_reader;
GRANT USAGE ON SCHEMA public TO fp_reader;
ALTER TABLE fp_docs ENABLE ROW LEVEL SECURITY;
CREATE POLICY fp_own ON fp_docs USING (owner = current_user);

SET ROLE fp_reader;
SET enable_sort = off;
SET client_min_messages = NOTICE;
SET pg_textsearch.log_filter_pushdown = on;
SELECT count(*) AS rls_rows,
       array_agg(id ORDER BY id) IS NOT DISTINCT FROM (
           SELECT array_agg(id ORDER BY id) FROM fp_docs
           WHERE coll = 8 AND string_to_array(body, ' ') && ARRAY['w1', 'w2'])
       AS rls_oracle
FROM (SELECT id FROM fp_docs WHERE coll = 8
      ORDER BY body <@> to_bm25query('w1 w2', 'fp_docs_bm25')
      LIMIT 100000) s;
RESET pg_textsearch.log_filter_pushdown;
SET client_min_messages = WARNING;
RESET enable_sort;
RESET ROLE;

ALTER TABLE fp_docs DISABLE ROW LEVEL SECURITY;
DROP POLICY fp_own ON fp_docs;
REVOKE ALL ON fp_docs FROM fp_reader;
REVOKE USAGE ON SCHEMA public FROM fp_reader;
DROP ROLE fp_reader;

------------------------------------------------------------------------
-- Scan identity: two BM25 scans in one statement, each with its own
-- filter.
------------------------------------------------------------------------

SET enable_sort = off;
SET client_min_messages = NOTICE;
SET pg_textsearch.log_filter_pushdown = on;
SELECT coll, count(*) FROM (
    (SELECT coll, id FROM fp_docs WHERE coll = 100
     ORDER BY body <@> to_bm25query('w3', 'fp_docs_bm25') LIMIT 1000)
    UNION ALL
    (SELECT coll, id FROM fp_docs WHERE coll = 200
     ORDER BY body <@> to_bm25query('w0 w1 w2 w3 w4', 'fp_docs_bm25')
     LIMIT 1000)) s
GROUP BY coll ORDER BY coll;
RESET pg_textsearch.log_filter_pushdown;
SET client_min_messages = WARNING;

SELECT (SELECT count(*) FROM (
            SELECT id FROM fp_docs WHERE coll = 100
            ORDER BY body <@> to_bm25query('w3', 'fp_docs_bm25')
            LIMIT 1000) s) = (SELECT count(*) FROM fp_docs
                              WHERE coll = 100
                                AND string_to_array(body, ' ') && ARRAY['w3'])
       AS first_scan,
       (SELECT count(*) FROM (
            SELECT id FROM fp_docs WHERE coll = 200
            ORDER BY body <@> to_bm25query('w0 w1 w2 w3 w4', 'fp_docs_bm25')
            LIMIT 1000) s) = (SELECT count(*) FROM fp_docs
                              WHERE coll = 200
                                AND string_to_array(body, ' ') &&
                                    ARRAY['w0', 'w1', 'w2', 'w3', 'w4'])
       AS second_scan;
RESET enable_sort;

------------------------------------------------------------------------
-- GUC contract
------------------------------------------------------------------------

SHOW pg_textsearch.filter_pushdown;
SHOW pg_textsearch.filter_pushdown_max_rows;
SET pg_textsearch.filter_pushdown_max_rows = -1;

DROP TABLE fp_acl;
DROP TABLE fp_docs;
DROP FUNCTION fp_list, fp_check, fp_oracle, fp_passes, fp_acl_list,
              fp_body, fp_coll;
