#ifndef AUTOINDEX_H
#define AUTOINDEX_H

#include "nodes/execnodes.h"
#include "storage/spin.h"

#define AUTOINDEX_MAX_ENTRIES   256
#define AUTOINDEX_QUEUE_SIZE    32
#define AUTOINDEX_MIN_SCANS     5       /* don't decide on fewer scans */
#define AUTOINDEX_BUILD_FACTOR  5.0     /* build cost = 5 x one full scan */
#define AUTOINDEX_DATABASE      "postgres"

typedef enum AutoIndexState
{
    AI_EMPTY = 0,       /* unused slot (memset 0 gives this) */
    AI_TRACKING,
    AI_READY,
    AI_QUEUED,
    AI_BUILDING,
    AI_DONE,
    AI_FAILED
} AutoIndexState;

typedef struct AutoIndexEntry
{
    AutoIndexState state;
    Oid         dbid;
    Oid         relid;
    AttrNumber  attnum;
    int64       nscans;
    int64       tuples_examined;
    int64       tuples_matched;
    double      selectivity;        /* matched / examined */
    double      total_seq_cost;
    double      total_benefit;
    double      build_cost;
} AutoIndexEntry;

typedef struct AutoIndexRequest
{
    Oid         dbid;
    Oid         relid;
    AttrNumber  attnum;
} AutoIndexRequest;

typedef struct AutoIndexShared
{
    slock_t          mutex;
    AutoIndexEntry   entries[AUTOINDEX_MAX_ENTRIES];
    int              head;
    int              tail;
    AutoIndexRequest queue[AUTOINDEX_QUEUE_SIZE];
} AutoIndexShared;

extern bool am_autoindex_worker;

/* autoindex.c */
extern Size AutoIndexShmemSize(void);
extern void AutoIndexShmemInit(void);
extern void autoindex_init_scan(SeqScanState *node, int eflags);
extern void autoindex_report_scan(SeqScanState *node);
extern bool autoindex_column_has_index(Relation rel, AttrNumber attno);
extern bool autoindex_pop_request(AutoIndexRequest *req);
extern void autoindex_mark(const AutoIndexRequest *req, AutoIndexState st);

/* autoindex_worker.c */
extern void AutoIndexWorkerRegister(void);
extern void AutoIndexWorkerMain(Datum main_arg);

#endif