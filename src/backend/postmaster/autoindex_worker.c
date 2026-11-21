#include "postgres.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autoindex.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

/* Called once by the postmaster at startup */
void
AutoIndexWorkerRegister(void)
{
	BackgroundWorker bgw;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = 10;		/* seconds, if it ever crashes */
	snprintf(bgw.bgw_library_name, sizeof(bgw.bgw_library_name), "postgres");
	snprintf(bgw.bgw_function_name, sizeof(bgw.bgw_function_name), "AutoIndexWorkerMain");
	snprintf(bgw.bgw_name, sizeof(bgw.bgw_name), "autoindex worker");
	snprintf(bgw.bgw_type, sizeof(bgw.bgw_type), "autoindex worker");
	RegisterBackgroundWorker(&bgw);
}

/* Build one index; never lets an error kill the worker */
static void
build_index(const AutoIndexRequest *req)
{
	MemoryContext oldcxt = CurrentMemoryContext;

	if (req->dbid != MyDatabaseId)
	{
		/* basic version: the worker only serves its own database */
		autoindex_mark(req, AI_FAILED);
		return;
	}

	autoindex_mark(req, AI_BUILDING);

	PG_TRY();
	{
		char	   *relname;
		char	   *nspname;
		char	   *attname;
		StringInfoData sql;

		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());

		relname = get_rel_name(req->relid);
		attname = get_attname(req->relid, req->attnum, true);
		nspname = relname ? get_namespace_name(get_rel_namespace(req->relid)) : NULL;

		if (relname == NULL || attname == NULL || nspname == NULL)
			elog(ERROR, "autoindex: table or column no longer exists");

		initStringInfo(&sql);
		appendStringInfo(&sql, "CREATE INDEX ON %s.%s (%s)",
						 quote_identifier(nspname),
						 quote_identifier(relname),
						 quote_identifier(attname));

		pgstat_report_activity(STATE_RUNNING, sql.data);
		elog(LOG, "autoindex: running: %s", sql.data);

		if (SPI_execute(sql.data, false, 0) != SPI_OK_UTILITY)
			elog(ERROR, "autoindex: CREATE INDEX failed");

		elog(LOG, "autoindex: created index on %s.%s (%s)", nspname, relname, attname);

		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		pgstat_report_activity(STATE_IDLE, NULL);

		autoindex_mark(req, AI_DONE);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcxt);
		HOLD_INTERRUPTS();
		EmitErrorReport();			/* write the error to the log */
		AbortOutOfAnyTransaction();
		FlushErrorState();
		RESUME_INTERRUPTS();
		pgstat_report_activity(STATE_IDLE, NULL);

		autoindex_mark(req, AI_FAILED);
	}
	PG_END_TRY();
}

/* The worker process starts here */
void
AutoIndexWorkerMain(Datum main_arg)
{
	am_autoindex_worker = true;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(AUTOINDEX_DATABASE, NULL, 0);

	elog(LOG, "autoindex worker started");

	for (;;)
	{
		AutoIndexRequest req;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 1000L,			/* wake up every second */
						 PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		while (autoindex_pop_request(&req))
			build_index(&req);
	}
}