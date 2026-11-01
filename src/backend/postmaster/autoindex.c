#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/stratnum.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "postmaster/autoindex.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "optimizer/optimizer.h"

bool am_autoindex_worker = false;

static AutoIndexShared *AutoIndexShmem = NULL;

Size
AutoIndexShmemSize(void)
{
    return sizeof(AutoIndexShared);
}

void
AutoIndexShmemInit(void)
{
    bool found;

    AutoIndexShmem = ShmemInitStruct("AutoIndex Data", sizeof(AutoIndexShared), &found);
    if (!found)
    {
        memset(AutoIndexShmem, 0, sizeof(AutoIndexShared));   /* all slots AI_EMPTY */
        SpinLockInit(&AutoIndexShmem->mutex);
    }
}

/* caller holds mutex */
static AutoIndexEntry *
find_entry(Oid dbid, Oid relid, AttrNumber attnum, bool create)
{
    AutoIndexEntry *free_slot = NULL;

    for (int i = 0; i < AUTOINDEX_MAX_ENTRIES; i++)
    {
        AutoIndexEntry *e = &AutoIndexShmem->entries[i];

        if (e->state == AI_EMPTY)
        {
            if (free_slot == NULL)
                free_slot = e;
            continue;
        }
        if (e->dbid == dbid && e->relid == relid && e->attnum == attnum)
            return e;
    }
    if (!create || free_slot == NULL)
        return NULL;

    memset(free_slot, 0, sizeof(AutoIndexEntry));
    free_slot->state = AI_TRACKING;
    free_slot->dbid = dbid;
    free_slot->relid = relid;
    free_slot->attnum = attnum;
    return free_slot;
}

void
autoindex_report_scan(SeqScanState *node)
{
	Relation	rel = node->ss.ss_currentRelation;
	AutoIndexEntry *e;
	AutoIndexEntry copy;
	bool		have = false;

	/* costs are computed BEFORE taking the spinlock */
	double		pages = (double) RelationGetNumberOfBlocks(rel);
	double		examined = (double) node->ai_examined;
	double		matched = (double) node->ai_matched;

	/* what this seq scan cost (same ingredients as cost_seqscan) */
	double		seq_cost = pages * seq_page_cost
		+ examined * (cpu_tuple_cost + cpu_operator_cost);

	/* what an index scan would have cost: ~3 page descent + 1 random page per match */
	double		idx_cost = 3 * random_page_cost
		+ matched * (random_page_cost + cpu_index_tuple_cost + cpu_tuple_cost);

	double		benefit = Max(0.0, seq_cost - idx_cost);
	double		build_cost = AUTOINDEX_BUILD_FACTOR *
		(pages * seq_page_cost + examined * cpu_tuple_cost);

	SpinLockAcquire(&AutoIndexShmem->mutex);
	e = find_entry(MyDatabaseId, RelationGetRelid(rel), node->ai_attno, true);
	if (e != NULL)
	{
		e->nscans++;
		e->tuples_examined += node->ai_examined;
		e->tuples_matched += node->ai_matched;
		e->selectivity = e->tuples_examined > 0 ?
			(double) e->tuples_matched / e->tuples_examined : 1.0;

		e->total_seq_cost += seq_cost;
		e->total_benefit += benefit;	/* = benefit per scan x scans */
		e->build_cost = build_cost;

		if (e->state == AI_TRACKING &&
			e->nscans >= AUTOINDEX_MIN_SCANS &&
			e->total_benefit > e->build_cost)
			e->state = AI_READY;

		copy = *e;
		have = true;
	}
	SpinLockRelease(&AutoIndexShmem->mutex);

	/* never elog while holding a spinlock: log from the copy */
	if (have)
		elog(DEBUG1, "autoindex: %s.%d scans=%lld examined=%lld matched=%lld sel=%.6f "
			 "benefit=%.0f total_benefit=%.0f build_cost=%.0f state=%d",
			 RelationGetRelationName(rel), copy.attnum,
			 (long long) copy.nscans, (long long) copy.tuples_examined,
			 (long long) copy.tuples_matched, copy.selectivity,
			 benefit, copy.total_benefit, copy.build_cost, (int) copy.state);
}
/* Is this qual "column = constant" on our table, with a btree '=' operator? */
static bool
match_equality_clause(Expr *clause, Index scanrelid, AttrNumber *attno)
{
    OpExpr *op;
    Node   *l, *r, *other;
    Var    *var;
    Oid     opclass;

    if (!IsA(clause, OpExpr))
        return false;
    op = (OpExpr *) clause;
    if (list_length(op->args) != 2)
        return false;

    l = (Node *) linitial(op->args);
    r = (Node *) lsecond(op->args);
    while (IsA(l, RelabelType)) l = (Node *) ((RelabelType *) l)->arg;
    while (IsA(r, RelabelType)) r = (Node *) ((RelabelType *) r)->arg;

    if (IsA(l, Var))      { var = (Var *) l; other = r; }
    else if (IsA(r, Var)) { var = (Var *) r; other = l; }
    else
        return false;

    if (var->varno != scanrelid || var->varattno <= 0 || var->varlevelsup != 0)
        return false;

    if (IsA(other, Const))
    {
        if (((Const *) other)->constisnull)
            return false;
    }
    else if (!(IsA(other, Param) && ((Param *) other)->paramkind == PARAM_EXTERN))
        return false;

    opclass = GetDefaultOpClass(var->vartype, BTREE_AM_OID);
    if (!OidIsValid(opclass))
        return false;
    if (get_op_opfamily_strategy(op->opno, get_opclass_family(opclass))
        != BTEqualStrategyNumber)
        return false;

    *attno = var->varattno;
    return true;
}

/* Is there already a valid, non-partial index whose first column is attno? */
bool
autoindex_column_has_index(Relation rel, AttrNumber attno)
{
    List     *indexoids = RelationGetIndexList(rel);
    ListCell *lc;
    bool      found = false;

    foreach(lc, indexoids)
    {
        HeapTuple     tup = SearchSysCache1(INDEXRELID,
                                            ObjectIdGetDatum(lfirst_oid(lc)));
        Form_pg_index idx;

        if (!HeapTupleIsValid(tup))
            continue;
        idx = (Form_pg_index) GETSTRUCT(tup);
        if (idx->indisvalid && idx->indkey.values[0] == attno &&
            heap_attisnull(tup, Anum_pg_index_indpred, NULL))
            found = true;
        ReleaseSysCache(tup);
        if (found)
            break;
    }
    list_free(indexoids);
    return found;
}

void
autoindex_init_scan(SeqScanState *node, int eflags)
{
    SeqScan  *plan = (SeqScan *) node->ss.ps.plan;
    Relation  rel = node->ss.ss_currentRelation;
    ListCell *lc;

    node->ai_active = false;
    node->ai_examined = node->ai_matched = 0;
    node->ai_reached_end = false;

    /* things we never track */
    if (am_autoindex_worker || (eflags & EXEC_FLAG_EXPLAIN_ONLY) ||
        plan->scan.plan.parallel_aware || IsParallelWorker() ||
        RecoveryInProgress() ||
        RelationGetRelid(rel) < FirstNormalObjectId ||
        rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP ||
        rel->rd_rel->relkind != RELKIND_RELATION)
        return;

    foreach(lc, plan->scan.plan.qual)
    {
        AttrNumber attno;

        if (match_equality_clause((Expr *) lfirst(lc), plan->scan.scanrelid, &attno) &&
            !autoindex_column_has_index(rel, attno))
        {
            node->ai_active = true;
            node->ai_attno = attno;
            elog(DEBUG1, "autoindex: candidate %s.%s",
                 RelationGetRelationName(rel),
                 get_attname(RelationGetRelid(rel), attno, false));
            break;              
        }
    }
}