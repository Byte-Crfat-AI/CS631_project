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

bool am_autoindex_worker = false;

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