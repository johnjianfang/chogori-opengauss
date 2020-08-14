/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 * snapshot.cpp
 *
 *    Automatically collect MPP snapshots in the background,
 * you can also manually collect snapshots
 *
 * IDENTIFICATION
 *	  src/gausskernel/cbb/instruments/wdr/snapshot.cpp
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/xact.h"
#include "postmaster/postmaster.h"
#include "pgxc/pgxc.h"
#include "pgxc/pgxcnode.h"
#include "pgxc/poolutils.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "storage/latch.h"
#include "storage/ipc.h"
#include "workload/workload.h"
#include "catalog/pg_database.h"
#include "gssignal/gs_signal.h"
#include "utils/guc.h"
#include "utils/ps_status.h"
#include "utils/elog.h"
#include "utils/memprot.h"
#include "utils/builtins.h"
#include "tcop/dest.h"
#include "tcop/tcopprot.h"
#include "gs_thread.h"
#include "access/heapam.h"
#include "optimizer/autoanalyzer.h"
#include "utils/rel.h"
#include "utils/postinit.h"
#include "pgstat.h"
#include "pgxc/execRemote.h"
#include "workload/gscgroup.h"
#include "commands/dbcommands.h"
#include "instruments/generate_report.h"
#include "instruments/snapshot.h"
#include "instruments/dblink_query.h"
#include "libpq/pqsignal.h"
#include "pgxc/groupmgr.h"

const int PGSTAT_RESTART_INTERVAL = 60;
#define MAX_INT ((unsigned)(-1) >> 1)

using namespace std;

typedef struct TableList {
    List* multiDbTableList;   /* tables do not need to read data across the database */
    List* oneDbTableList;     /* tables need to read data from multi database */
    List* lastOneDbTableList; /* the table of "snap_class_vital_info" must be placed at end of each snapshot */
    List* lastStatTableList;  /* the table of "global_record_reset_time" */
    List* analyzeTableList;   /* Some tables are often updated and need to be analyzed in advance */
} TablesList;
/*
 * function
 */
NON_EXEC_STATIC void SnapshotMain();

namespace SnapshotNameSpace {
void CleanSnapshot(uint64 curr_min_snapid, const TablesList& tablesList);
void take_snapshot(const TablesList& tablesList);
void SubSnapshotMain(void);
void InsertOneTableData(List* ViewNameList, uint64 snapid);
void InsertTablesData(uint64 snapid, const TablesList& tablesList);
void DeleteTablesData(List* tableList, uint64 snapid);
void GetQueryData(const char* query, bool with_column_name, List** cstring_values);
bool ExecuteQuery(const char* query, int SPI_OK_STAT);
void init_curr_table_size(void);
void init_curr_snapid(void);
void CreateTable(List* tableList, bool ismultidbtable);
void InitTables(TablesList& tablesList);
void CreateSnapStatTables(void);
void CreateIndexes(void);
void CreateSequence(void);
void UpdateSnapEndTime(uint64 curr_snapid);
void GetQueryStr(StringInfoData& query, const char* viewname, uint64 curr_snapid, const char* dbname);
void GetDatabaseList(List** databaselist);
void InsertOneDbTables(uint64 curr_snapid, const List* tablesList);
void InsertDatabaseData(const char* dbname, uint64 curr_snapid, const List* tablesList);
void InitTableList(TablesList& tablesList);
void SplitString(const char* str, const char* delim, List** res);
void SetSnapshotSize(uint32* max_snap_size);
void SleepCheckInterrupt(uint32 minutes);
void ResetPortDefaultKeepalivesValue(void);
void SetThrdCxt(void);
void AnalyzeTable(List* analyzeList);
void GetAnalyzeList(List** analyzeList);
char* GetTableColAttr(const char* viewname, bool onlyViewCol, bool addType);
}  // namespace SnapshotNameSpace

void instrSnapshotClean(void)
{
    dblinkCloseConn();
}

void instrSnapshotCancel(void)
{
    dblinkRequestCancel();
}

/* SIGQUIT signal handler for auditor process */
static void instr_snapshot_exit(SIGNAL_ARGS)
{
    t_thrd.perf_snap_cxt.need_exit = true;
    die(postgres_signal_arg);
}

/* SIGHUP handler for collector process */
static void instr_snapshot_sighup_handler(SIGNAL_ARGS)
{
    t_thrd.perf_snap_cxt.got_SIGHUP = true;
}

static void request_snapshot(SIGNAL_ARGS)
{
    t_thrd.perf_snap_cxt.request_snapshot = true;
}
static char* GetCurrentTimeStampString()
{
    return Datum_to_string(TimestampGetDatum(GetCurrentTimestamp()), TIMESTAMPTZOID, false);
}
/* 1. during redistribution, will set 'in_redistribution' field to 'y'
 *    in pgxc_group table, exit snapshot thread to avoid dead lock issue
 * 2. during upgrade, snapshot table can be modified,
 *    exit snapshot thread to avoid insert data failed
 */
static void check_snapshot_thd_exit()
{
    bool need_exit = false;

    StartTransactionCommand();
    char* redis_group = PgxcGroupGetInRedistributionGroup();
    if (redis_group != NULL) {
        need_exit = true;
    }
    CommitTransactionCommand();

    if (need_exit || u_sess->attr.attr_common.upgrade_mode != 0) {
        /* to avoid postmaster to start snapshot thread again immediately,
         * we sleep several minute before exit.
         */
        const int SLEEP_GAP = 5;
        ereport(LOG, (errmsg("snapshot thread will exit during redistribution or upgrade")));
        SnapshotNameSpace::SleepCheckInterrupt(SLEEP_GAP);
        ereport(LOG, (errmsg("snapshot thread is exited during redistribution or upgrade")));

        gs_thread_exit(0);
    }
}

#ifdef ENABLE_MULTIPLE_NODES
/* kill snapshot thread on all CN node */
void kill_snapshot_remote()
{
    ExecNodes* exec_nodes = NULL;
    StringInfoData buf;
    ParallelFunctionState* state = NULL;

    exec_nodes = (ExecNodes*)makeNode(ExecNodes);
    exec_nodes->baselocatortype = LOCATOR_TYPE_HASH;
    exec_nodes->accesstype = RELATION_ACCESS_READ;
    exec_nodes->primarynodelist = NIL;
    exec_nodes->en_expr = NULL;
    exec_nodes->en_relid = InvalidOid;
    exec_nodes->nodeList = NIL;

    initStringInfo(&buf);
    appendStringInfo(&buf, "SELECT kill_snapshot();");
    state = RemoteFunctionResultHandler(buf.data, exec_nodes, NULL, true, EXEC_ON_COORDS, true);
    FreeParallelFunctionState(state);
    pfree_ext(buf.data);
}
#endif

/*
 * kill snapshot thread
 * There will be deadlocks between snapshot thread and redistribution,
 * so during redistribution, will stop snapshot thread.
 *
 * return true if killed snapshot thread successfully
 */
Datum kill_snapshot(PG_FUNCTION_ARGS)
{
    if (!superuser()) {
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("only system admin can kill snapshot thread")));
    }

    if (IS_PGXC_COORDINATOR || IS_SINGLE_NODE) {
        /* CN need to kill snapshot thread */
        if (g_instance.pid_cxt.SnapshotPID != 0) {
            if (PgxcGroupGetInRedistributionGroup() != NULL) {
                ereport(LOG, (errmsg("snapshot thread - redistribution in progress")));
            }

            /* wait 100 seconds */
            const int MAX_RETRY_COUNT = 1000000;
            const int SLEEP_GAP = 100;
            int err = 0;
            uint32 old_snapshot_thread_counter = pg_atomic_read_u32(&g_instance.stat_cxt.snapshot_thread_counter);
            if ((err = gs_signal_send(g_instance.pid_cxt.SnapshotPID, SIGTERM)) != 0) {
                ereport(ERROR,
                    (errcode(ERRCODE_OPERATE_FAILED), errmsg("kill snapshot thread failed %s", gs_strerror(err))));
            } else {
                int wait_times = 0;
                /* wait snapshot thread is exit.
                 * - exit case: g_instance.pid_cxt.SnapshotPID is zero
                 * - new start case: g_instance.stat_cxt.snapshot_thread_counter will be changed
                 *
                 * new started snapshot thread PID can be same with before,
                 * so use a status counter to indicate snapshot thread restart
                 */
                while (g_instance.pid_cxt.SnapshotPID != 0 &&
                       g_instance.stat_cxt.snapshot_thread_counter == old_snapshot_thread_counter) {
                    if (wait_times++ > MAX_RETRY_COUNT) {
                        ereport(ERROR,
                            (errcode(ERRCODE_OPERATE_FAILED),
                                errmsg("kill snapshot thread failed, exceeds MAX_RETRY_COUNT(%d)", MAX_RETRY_COUNT)));
                    }
                    pg_usleep(SLEEP_GAP);
                }
                ereport(LOG, (errmsg("snapshot thread is killed")));
            }
        }

        if (!IsConnFromCoord()) {
            /* get kill snapshot request from GSQL/JDBC/SPI, etc, notity to other CNs */
            ereport(LOG, (errmsg("send 'kill snapshot thread' request to other CNs")));
#ifdef ENABLE_MULTIPLE_NODES
            kill_snapshot_remote();
#endif
        }
    }

    PG_RETURN_VOID();
}
static void ReloadInfo()
{
    /* Reload configuration if we got SIGHUP from the postmaster. */
    if (t_thrd.perf_snap_cxt.got_SIGHUP) {
        t_thrd.perf_snap_cxt.got_SIGHUP = false;
        ProcessConfigFile(PGC_SIGHUP);
    }
    if (u_sess->sig_cxt.got_PoolReload) {
        processPoolerReload();
        u_sess->sig_cxt.got_PoolReload = false;
    }
}

/* to avoid deadlock with APP or gs_redis, set lockwait_timeout to 3s */
static void set_lock_timeout()
{
    int rc = 0;
    /* lock wait timeout to 3s */
    const char* timeout_sql = "set lockwait_timeout = 3000;";
    if ((rc = SPI_execute(timeout_sql, false, 0)) != SPI_OK_UTILITY) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR), errmsg("set lockwait_timeout failed:  %s", SPI_result_code_string(rc))));
    }
}

/*
 * Manually performing a snapshot
 */
Datum create_wdr_snapshot(PG_FUNCTION_ARGS)
{
    const uint32 maxTryCount = 100;
    const uint32 waitTime = 100000;
    /* ensure limited access to create the snapshot */
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("Superuser privilege is need to operate snapshot")));
    }
    if (!u_sess->attr.attr_common.enable_wdr_snapshot) {
        ereport(WARNING, (errcode(ERRCODE_WARNING), errmsg("GUC parameter 'enable_wdr_snapshot' is off")));
        PG_RETURN_TEXT_P(cstring_to_text("WDR snapshot request can't be executed"));
        ;
    }
    if (!PgxcIsCentralCoordinator(g_instance.attr.attr_common.PGXCNodeName) && (IS_SINGLE_NODE != true)) {
        ereport(NOTICE, (errmsg("take snapshot must be on CCN node")));
        PG_RETURN_TEXT_P(cstring_to_text("WDR snapshot request can't be executed"));
    }
    for (uint32 ntries = 0;; ntries++) {
        if (g_instance.pid_cxt.SnapshotPID == 0) {
            if (ntries >= maxTryCount) {
                ereport(ERROR,
                    (errcode(ERRCODE_OPERATE_FAILED),
                        errmsg("WDR snapshot request can not be accepted, please retry later")));
            }
        } else if (gs_signal_send(g_instance.pid_cxt.SnapshotPID, SIGINT) != 0) {
            if (ntries >= maxTryCount) {
                ereport(ERROR, (errcode(ERRCODE_OPERATE_FAILED), errmsg("Cannot respond to WDR snapshot request")));
            }
        } else {
            break;
        }
        CHECK_FOR_INTERRUPTS();
        pg_usleep(waitTime); /* wait 0.1 sec, then retry */
    }
    PG_RETURN_TEXT_P(cstring_to_text("WDR snapshot request has been submitted"));
}
/*
 * Execute query
 * parameter:
 *    query     -- query text
 *    SPI_OK_STAT -- Query execution status
 */
bool SnapshotNameSpace::ExecuteQuery(const char* query, int SPI_OK_STAT)
{
    if (query == NULL) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("query is NULL")));
    }

    if (SPI_execute(query, false, 0) != SPI_OK_STAT) {
        ereport(WARNING, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("invalid query : %s", query)));
        return false;
    }
    return true;
}

/*
 * GetDatumValue
 * return the result of select sql
 * but the type of return value is Datum, You must
 * convert the value to the corresponding type
 * row : the row of value (begin from 0)
 * col : the col of value (begin from 0)
 */
Datum GetDatumValue(const char* query, uint32 row, uint32 col, bool* isnull)
{
    Datum colval;
    bool tmp_isnull = false;

    if (query == NULL) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("query is null")));
    }

    if (!SnapshotNameSpace::ExecuteQuery(query, SPI_OK_SELECT)) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("this query can not get datum values")));
    }

    /* if row and col out of the size of query values, we return the values is null */
    if (row >= SPI_processed || col >= (uint32)SPI_tuptable->tupdesc->natts) {
        ereport(WARNING,
            (errcode(ERRCODE_DATA_EXCEPTION),
                errmsg("row : %d, col : %d out of the size of query data in get datum value", row, col)));
        return (Datum)NULL;
    }
    colval = SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, (col + 1), isnull ? isnull : &tmp_isnull);
    return colval;
}
/*
 * Initialize the size of the current table Every time
 * you boot you need to check the size of the table
 * size of the snapshot to initialize the curr_table_size.
 */
void SnapshotNameSpace::init_curr_table_size(void)
{
    Datum colval;
    bool isnull = false;
    const char* sql = "select count(*) from snapshot.snapshot";
    colval = GetDatumValue(sql, 0, 0, &isnull);
    if (isnull) {
        colval = 0;
    }
    t_thrd.perf_snap_cxt.curr_table_size = DatumGetInt32(colval);
}

void SnapshotNameSpace::CreateSequence(void)
{
    Datum colval;
    bool isnull = false;
    const char* query_seq_sql =
        "select count(*) from pg_statio_all_sequences where schemaname = 'snapshot' and relname = 'snap_seq'";
    colval = GetDatumValue(query_seq_sql, 0, 0, &isnull);
    if (isnull) {
        colval = 0;
    }
    if (!colval) {
        const char* create_seq_sql = "create sequence snapshot.snap_seq CYCLE";
        if (!SnapshotNameSpace::ExecuteQuery(create_seq_sql, SPI_OK_UTILITY)) {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("create sequence failed")));
        }
    }
}
void SnapshotNameSpace::init_curr_snapid(void)
{
    Datum colval;
    bool isnull = false;
    const char* sql = "select nextval('snapshot.snap_seq')";
    colval = GetDatumValue(sql, 0, 0, &isnull);
    if (isnull) {
        colval = 0;
    }
    t_thrd.perf_snap_cxt.curr_snapid = DatumGetInt32(colval);
    if (t_thrd.perf_snap_cxt.curr_snapid >= MAX_INT - 1) {
        t_thrd.perf_snap_cxt.curr_snapid = 0;
    }
}
/*
 * UpdateSnapEndTime -- update snapshot end time stamp
 */
void SnapshotNameSpace::UpdateSnapEndTime(uint64 curr_snapid)
{
    if (curr_snapid == 0) {
        return;
    }
    StringInfoData query;
    initStringInfo(&query);
    appendStringInfo(&query,
        "update snapshot.snapshot set end_ts = '%s' where snapshot_id = %lu",
        GetCurrentTimeStampString(),
        curr_snapid);
    if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_UPDATE)) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("update snapshot end time stamp filled")));
    }
    pfree_ext(query.data);
}

void SnapshotNameSpace::SetSnapshotSize(uint32* max_snap_size)
{
    if (u_sess->attr.attr_common.wdr_snapshot_interval == 0) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("wdr_snapshot_interval is 0")));
    }
    const int HOUR_OF_DAY = 24;
    const int MINUTE_Of_HOUR = 60;
    /*
     * 1 <= wdr_snapshot_retention_days <= 8(d)
     * 10 <= wdr_snapshot_interval <= 60(min)
     */
    *max_snap_size = u_sess->attr.attr_common.wdr_snapshot_retention_days * HOUR_OF_DAY * MINUTE_Of_HOUR /
                     u_sess->attr.attr_common.wdr_snapshot_interval;
}

void SnapshotNameSpace::GetQueryData(const char* query, bool with_column_name, List** cstring_values)
{
    bool isnull = false;
    List* colname_cstring = NIL;
    if (cstring_values != NULL) {
        list_free_deep(*cstring_values);
    }
    /* Establish SPI connection and get query execution result */
    if (query == NULL) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("query is null")));
    }

    ereport(DEBUG1, (errmodule(MOD_INSTR), errmsg("[Instruments/Report] query: %s", query)));

    if (SPI_execute(query, false, 0) != SPI_OK_SELECT) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("invalid query")));
    }
    /* get colname */
    if (with_column_name) {
        for (int32 i = 0; i < SPI_tuptable->tupdesc->natts; i++) {
            char* strName = pstrdup(SPI_tuptable->tupdesc->attrs[i]->attname.data);
            colname_cstring = lappend(colname_cstring, strName);
        }
        *cstring_values = lappend(*cstring_values, colname_cstring);
    }
    /* Get the data in the table and convert it to string format */
    for (uint32 i = 0; i < SPI_processed; i++) {
        List* row_string = NIL;
        uint32 colNum = (uint32)SPI_tuptable->tupdesc->natts;
        for (uint32 j = 1; j <= colNum; j++) {
            Oid type = SPI_gettypeid(SPI_tuptable->tupdesc, j);
            Datum colval = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, j, &isnull);
            char* tmpStr = Datum_to_string(colval, type, isnull);
            row_string = lappend(row_string, tmpStr);
        }
        *cstring_values = lappend(*cstring_values, row_string);
    }
}
/*
 * take_snapshot
 * All the snapshot processes are executed here,
 * we will collect snapshot info from views in dbe_perf schema,
 * then insert into snapshot tables in snapshot schema, In order to control
 * the amount of disk space occupied by the snapshot,
 * we use curr_tables_size to record the number of snapshots,
 * and use MAX_TABLE_SIZE to control the size of the snapshot disk space.
 */
void SnapshotNameSpace::take_snapshot(const TablesList& tablesList)
{
    StringInfoData query;
    initStringInfo(&query);
    PG_TRY();
    {
        int rc = 0;
        uint32 max_snap_size = 0;
        SnapshotNameSpace::SetSnapshotSize(&max_snap_size);
        /* connect SPI to execute query */
        if ((rc = SPI_connect()) != SPI_OK_CONNECT) {
            ereport(
                ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %s", SPI_result_code_string(rc))));
        }
        set_lock_timeout();
        SnapshotNameSpace::init_curr_table_size();
        while (t_thrd.perf_snap_cxt.curr_table_size >= max_snap_size) {
            bool isnull = false;
            const char* sql = "select min(snapshot_id) from snapshot.snapshot";
            Datum colval = GetDatumValue(sql, 0, 0, &isnull);
            if (isnull) {
                colval = 0;
            }
            SnapshotNameSpace::CleanSnapshot(DatumGetObjectId(colval), tablesList);
            t_thrd.perf_snap_cxt.curr_table_size--;
        }
        SnapshotNameSpace::init_curr_snapid();
        appendStringInfo(&query,
            "INSERT INTO snapshot.snapshot(snapshot_id, start_ts) "
            "values (%lu, '%s')",
            t_thrd.perf_snap_cxt.curr_snapid,
            GetCurrentTimeStampString());
        if (SPI_execute(query.data, false, 0) == SPI_OK_INSERT) {
            SnapshotNameSpace::InsertTablesData(t_thrd.perf_snap_cxt.curr_snapid, tablesList);
            t_thrd.perf_snap_cxt.curr_table_size++;
        } else {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("invalid query: %s", query.data)));
        }
        SnapshotNameSpace::UpdateSnapEndTime(t_thrd.perf_snap_cxt.curr_snapid);
        pfree_ext(query.data);
        SPI_finish();
    }
    PG_CATCH();
    {
        pfree_ext(query.data);
        SPI_finish();
        /* Carry on with error handling. */
        PG_RE_THROW();
    }
    PG_END_TRY();
}

static bool IsCgroupInit(const char* viewname)
{
    StringInfoData query;

    initStringInfo(&query);
    if (strncmp(viewname, "wlm_controlgroup_config", strlen("wlm_controlgroup_config")) == 0 ||
        strncmp(viewname, "wlm_controlgroup_ng_config", strlen("wlm_controlgroup_ng_config")) == 0 ||
        strncmp(viewname, "wlm_cgroup_config", strlen("wlm_cgroup_config")) == 0) {
        if (g_instance.wlm_cxt->gscgroup_init_done == 0) {
            return false;
        }
    }
    pfree_ext(query.data);
    return true;
}

void SnapshotNameSpace::DeleteTablesData(List* viewNameList, uint64 snapid)
{
    StringInfoData query;
    initStringInfo(&query);
    foreach_cell(cellViewName, viewNameList)
    {
        char* viewName = (char*)lfirst(cellViewName);
        if (!IsCgroupInit(viewName)) {
            continue;
        }
        resetStringInfo(&query);
        appendStringInfo(&query, "delete from snapshot.snap_%s where snapshot_id = %lu", viewName, snapid);
        if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_DELETE)) {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("clean table of snap_%s is failed", viewName)));
        }
    }
    pfree_ext(query.data);
}

void SnapshotNameSpace::GetAnalyzeList(List** analyzeTableList)
{
    Datum colval;
    bool isnull = false;
    StringInfoData query;
    initStringInfo(&query);
    const char* tableName = "tables_snap_timestamp";
    appendStringInfo(&query,
        "select relid from dbe_perf.class_vital_info "
        "where schemaname = 'snapshot' and relname = '%s'",
        tableName);
    colval = GetDatumValue(query.data, 0, 0, &isnull);
    if (!colval) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("WDR snapshot analyze table:%s not exist", tableName)));
    }
    MemoryContext old_context = MemoryContextSwitchTo(t_thrd.perf_snap_cxt.PerfSnapMemCxt);
    Oid* relationObjectId = (Oid*)palloc(sizeof(Oid));
    *relationObjectId = DatumGetObjectId(colval);
    *analyzeTableList = lappend(*analyzeTableList, relationObjectId);
    (void)MemoryContextSwitchTo(old_context);
    pfree_ext(query.data);
}

void SnapshotNameSpace::AnalyzeTable(List* analyzeList)
{
    Relation relation = NULL;
    foreach_cell(cellAnalyze, analyzeList)
    {
        Oid* analyzeName = (Oid*)lfirst(cellAnalyze);
        relation = heap_open(*analyzeName, NoLock);
        bool is_analyzed = AutoAnaProcess::runAutoAnalyze(relation);
        if (!is_analyzed) {
            ereport(WARNING, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("snapshot analyze is failed")));
        }
        heap_close(relation, NoLock);
    }
}

/*
 * is_skipped_view - check which view is needed to skip during snapshot
 */
static bool is_skipped_view(const char* view_name)
{
    if (view_name == NULL) {
        return true;
    }

    /* skip session/workload manager related views */
    if (strstr(view_name, "session_") != NULL || strstr(view_name, "wlm_") != NULL ||
        strstr(view_name, "wlmstat_") != NULL || strstr(view_name, "stat_sys_tables") ||
        strstr(view_name, "stat_all_tables")) {
        return true;
    }

    if (!IsCgroupInit(view_name)) {
        return true;
    }

    if ((!g_instance.attr.attr_memory.enable_memory_limit || maxChunksPerProcess) &&
        (strcmp(view_name, "session_memory") == 0 || strcmp(view_name, "memory_node_detail") == 0 ||
            strcmp(view_name, "memory_node_ng_detail") == 0 || strcmp(view_name, "global_session_memory") == 0 ||
            strcmp(view_name, "global_memory_node_detail") == 0 ||
            strcmp(view_name, "global_memory_node_ng_detail") == 0 ||
            strcmp(view_name, "summary_session_memory") == 0 || strcmp(view_name, "summary_memory_node_detail") == 0 ||
            strcmp(view_name, "summary_memory_node_ng_detail") == 0))
        return true;

    return false;
}

/*
 * InsertOneTableData -- insert snapshot into snapshot.snap_xxx_xxx table
 *
 * insert into a table of snapshot schema from a view data from dbe_perf schema
 * and must get start time and end time when snapshot table insert
 */
void SnapshotNameSpace::InsertOneTableData(List* ViewNameList, uint64 snapid)
{
    char* dbName = NULL;
    StringInfoData query;
    initStringInfo(&query);

    dbName = get_database_name(u_sess->proc_cxt.MyDatabaseId);
    if (dbName == NULL) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                    errmsg("database with OID %u does not exist",
                        u_sess->proc_cxt.MyDatabaseId)));
    }

    foreach_cell(cellViewName, ViewNameList)
    {
        char* viewName = (char*)lfirst(cellViewName);
        if (is_skipped_view(viewName)) {
            continue;
        }

        resetStringInfo(&query);
        CHECK_FOR_INTERRUPTS();
        /* Record snapshot start time of a single table */
        appendStringInfo(&query,
            "INSERT INTO snapshot.tables_snap_timestamp(snapshot_id, db_name, tablename, start_ts) "
            "values(%lu, '%s', 'snap_%s', '%s')",
            snapid,
            dbName,
            viewName,
            GetCurrentTimeStampString());
        if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_INSERT)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                    errmsg("insert into tables_snap_timestamp start time stamp is failed")));
        }

        CHECK_FOR_INTERRUPTS();
        resetStringInfo(&query);
        char* snapColAttr = SnapshotNameSpace::GetTableColAttr(viewName, false, false);
        char* colAttr = SnapshotNameSpace::GetTableColAttr(viewName, true, false);
        appendStringInfo(&query,
            "INSERT INTO snapshot.snap_%s(snapshot_id, %s) select %lu, %s from dbe_perf.%s",
            viewName,
            snapColAttr,
            snapid,
            colAttr,
            viewName);
        pfree(colAttr);
        pfree(snapColAttr);
        if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_INSERT)) {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("insert into snap_%s is failed", viewName)));
        }
        CHECK_FOR_INTERRUPTS();

        resetStringInfo(&query);
        /* Record snapshot end time of a single table */
        appendStringInfo(&query,
            "update snapshot.tables_snap_timestamp set end_ts = '%s' "
            "where snapshot_id = %lu and db_name = '%s' and tablename = 'snap_%s'",
            GetCurrentTimeStampString(),
            snapid,
            dbName,
            viewName);
        if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_UPDATE)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION), errmsg("update tables_snap_timestamp end time stamp is failed")));
        }
    }
    pfree_ext(dbName);
    pfree_ext(query.data);
}

void SnapshotNameSpace::InsertTablesData(uint64 snapid, const TablesList& tablesList)
{
    SnapshotNameSpace::InsertOneDbTables(snapid, tablesList.oneDbTableList);
    SnapshotNameSpace::InsertOneTableData(tablesList.multiDbTableList, snapid);
    SnapshotNameSpace::InsertOneTableData(tablesList.lastStatTableList, snapid);
    SnapshotNameSpace::InsertOneDbTables(snapid, tablesList.lastOneDbTableList);
}

static void DeleteStatTableDate(uint64 curr_min_snapid)
{
    StringInfoData query;
    initStringInfo(&query);

    appendStringInfo(&query, "delete from snapshot.snapshot where snapshot_id = %lu", curr_min_snapid);
    if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_DELETE)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_EXCEPTION),
                errmsg("clean snapshot id %lu is failed in snapshot table", curr_min_snapid)));
    }

    resetStringInfo(&query);
    appendStringInfo(&query, "delete from snapshot.tables_snap_timestamp where snapshot_id = %lu", curr_min_snapid);
    if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_DELETE)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_EXCEPTION),
                errmsg("clean snapshot id %lu is failed in tables_snap_timestamp table", curr_min_snapid)));
    }
    pfree_ext(query.data);
}

/*
 * Called when the tablespace exceeds the set value
 * Delete the smallest case of snapid in the table
 */
void SnapshotNameSpace::CleanSnapshot(uint64 curr_min_snapid, const TablesList& tablesList)
{
    DeleteStatTableDate(curr_min_snapid);
    SnapshotNameSpace::DeleteTablesData(tablesList.multiDbTableList, curr_min_snapid);
    SnapshotNameSpace::DeleteTablesData(tablesList.oneDbTableList, curr_min_snapid);
    SnapshotNameSpace::DeleteTablesData(tablesList.lastOneDbTableList, curr_min_snapid);
    SnapshotNameSpace::DeleteTablesData(tablesList.lastStatTableList, curr_min_snapid);
    ereport(LOG, (errmsg("delete snapshot where snapshot_id = " UINT64_FORMAT, curr_min_snapid)));
}
/*
 * snapshot_start
 *
 *    Called from postmaster at startup or after an existing collector
 *    died.  Attempt to fire up a fresh statistics collector.
 *
 *    Returns PID of child process, or 0 if fail.
 *
 *    Note: if fail, we will be called again from the postmaster main loop.
 */
ThreadId snapshot_start(void)
{
    time_t curtime;

    /*
     * Do nothing if too soon since last collector start.  This is a safety
     * valve to protect against continuous respawn attempts if the collector
     * is dying immediately at launch.    Note that since we will be re-called
     * from the postmaster main loop, we will get another chance later.
     */
    curtime = time(NULL);
    if ((unsigned int)(curtime - t_thrd.perf_snap_cxt.last_snapshot_start_time) < (unsigned int)PGSTAT_RESTART_INTERVAL) {
        return 0;
    }
    t_thrd.perf_snap_cxt.last_snapshot_start_time = curtime;

    return initialize_util_thread(SNAPSHOT_WORKER);
}

void JobSnapshotIAm(void)
{
    t_thrd.role = SNAPSHOT_WORKER;
}

/*
 * called in  initpostgres() function
 */
bool IsJobSnapshotProcess(void)
{
    return t_thrd.role == SNAPSHOT_WORKER;
}

static void CreateStatTable(const char* query, const char* tablename)
{
    Datum colval;
    bool isnull = false;
    StringInfoData sql;
    initStringInfo(&sql);
    if (query == NULL || tablename == NULL) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_EXCEPTION),
                errmsg("query or the tablename is null when snapshot create stat table")));
    }

    /* if the table is not created ,we will create it */
    appendStringInfo(
        &sql, "select count(*) from pg_tables where tablename = '%s' and schemaname = 'snapshot'", tablename);
    colval = GetDatumValue(sql.data, 0, 0, &isnull);
    if (!DatumGetInt32(colval)) {
        if (!SnapshotNameSpace::ExecuteQuery(query, SPI_OK_UTILITY)) {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("can not create snapshot stat table")));
        }
    }
    pfree_ext(sql.data);
}

void SnapshotNameSpace::CreateSnapStatTables(void)
{
    Datum colval;
    bool isnull = false;
    const char* check_snap_schema_sql = "select count(*) from pg_namespace where nspname = 'snapshot';";
    const char* createSchema = "create schema snapshot";
    colval = GetDatumValue(check_snap_schema_sql, 0, 0, &isnull);
    if (!DatumGetInt32(colval)) {
        (void)SnapshotNameSpace::ExecuteQuery(createSchema, SPI_OK_UTILITY);
    }

    const char* createTs = "create table snapshot.tables_snap_timestamp(snapshot_id bigint not null, db_name text, "
                           "tablename text, start_ts timestamp with time zone, end_ts timestamp with time zone)";
    const char* tablename1 = "tables_snap_timestamp";

    CreateStatTable(createTs, tablename1);

    const char* createSnapshot =
        "create table snapshot.snapshot(snapshot_id bigint not null, "
        "start_ts  timestamp with time zone, end_ts  timestamp with time zone, primary key (snapshot_id))";
    const char* tablename2 = "snapshot";

    CreateStatTable(createSnapshot, tablename2);
}
/*
 *
 *
 */
void SnapshotNameSpace::InitTables(TablesList& tablesList)
{
    tablesList.multiDbTableList = NIL;
    tablesList.oneDbTableList = NIL;
    tablesList.lastOneDbTableList = NIL;
    tablesList.lastStatTableList = NIL;
    tablesList.analyzeTableList = NIL;
    SnapshotNameSpace::InitTableList(tablesList);

    SnapshotNameSpace::CreateSnapStatTables();
    SnapshotNameSpace::CreateSequence();

    SnapshotNameSpace::CreateTable(tablesList.multiDbTableList, true);
    SnapshotNameSpace::CreateTable(tablesList.oneDbTableList, false);
    SnapshotNameSpace::CreateTable(tablesList.lastOneDbTableList, false);
    SnapshotNameSpace::CreateTable(tablesList.lastStatTableList, true);
    SnapshotNameSpace::CreateIndexes();
}

void SnapshotNameSpace::CreateTable(List* tableList, bool isMultiDbTable)
{
    bool isnull = false;
    StringInfoData query;
    initStringInfo(&query);
    foreach_cell(cellTable, tableList)
    {
        char* tableName = (char*)lfirst(cellTable);
        resetStringInfo(&query);
        /* if the table is not created, we will create it */
        appendStringInfo(&query,
            "select count(*) from pg_tables where tablename = 'snap_%s' "
            "and schemaname = 'snapshot'",
            tableName);
        Datum colval = GetDatumValue(query.data, 0, 0, &isnull);
        if (!DatumGetInt32(colval)) {
            resetStringInfo(&query);
            /* Get the attributes of a table's columns */
            char* snapColAttrType = SnapshotNameSpace::GetTableColAttr(tableName, false, true);
            if (isMultiDbTable) {
                appendStringInfo(
                    &query, "create table snapshot.snap_%s(snapshot_id bigint, %s)", tableName, snapColAttrType);
            } else {
                appendStringInfo(&query,
                    "create table snapshot.snap_%s(snapshot_id bigint, db_name text, %s)",
                    tableName,
                    snapColAttrType);
            }
            if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_UTILITY)) {
                ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("create WDR snapshot data table failed")));
            }
            pfree(snapColAttrType);
        }
    }
    pfree_ext(query.data);
}
char* SnapshotNameSpace::GetTableColAttr(const char* viewName, bool onlyViewCol, bool addType)
{
    List* stringtables = NIL;
    StringInfoData sql;
    initStringInfo(&sql);
    StringInfoData colType;
    initStringInfo(&colType);
    /* Get the attributes of a view's columns under the dbe_perf schema */
    appendStringInfo(&sql,
        "SELECT a.attname AS field,t.typname AS type FROM "
        "pg_class c,pg_attribute a,pg_type t, pg_namespace n"
        " WHERE n.nspname = 'dbe_perf' and c.relnamespace = n.oid and c.relname = '%s' "
        "and a.attnum > 0 and a.attrelid = c.oid and a.atttypid = t.oid ORDER BY a.attnum;",
        viewName);
    SnapshotNameSpace::GetQueryData(sql.data, false, &stringtables);
    foreach_cell(cell, stringtables)
    {
        ListCell* row = ((List*)lfirst(cell))->head;
        if (onlyViewCol) {
            appendStringInfo(&colType, "\"%s\" ", (char*)lfirst(row));
        } else {
            appendStringInfo(&colType, "\"snap_%s\" ", (char*)lfirst(row));
        }
        if (addType) {
            appendStringInfo(&colType, "%s", (char*)lfirst(row->next));
        }
        if (cell->next != NULL)
            appendStringInfo(&colType, ", ");
    }
    pfree_ext(sql.data);
    DeepListFree(stringtables, true);
    return colType.data;
}
void SnapshotNameSpace::GetQueryStr(StringInfoData& query, const char* viewname, uint64 curr_snapid, const char* dbname)
{
    char* snapColAttr = SnapshotNameSpace::GetTableColAttr(viewname, false, false);
    char* snapColAttrType = SnapshotNameSpace::GetTableColAttr(viewname, false, true);
    appendStringInfo(&query,
        "insert into snapshot.snap_%s(snapshot_id, db_name, %s) select snapshot_id, dbname1, %s from"
        " wdr_xdb_query('dbname=%s'::text, 'select %lu, ''%s'', t.* from dbe_perf.%s t'::text)"
        " as i(snapshot_id int8, dbname1 text, %s)",
        viewname,
        snapColAttr,
        snapColAttr,
        dbname,
        curr_snapid,
        dbname,
        viewname,
        snapColAttrType);
    pfree_ext(snapColAttr);
    pfree(snapColAttrType);
}
void SnapshotNameSpace::GetDatabaseList(List** databaselist)
{
    StringInfoData query;

    initStringInfo(&query);

    appendStringInfo(&query, "select datname from pg_database where datistemplate = 'f'");
    SnapshotNameSpace::GetQueryData(query.data, false, databaselist);
    pfree_ext(query.data);
}

/*
 * InsertOneDbTables
 *        for exemple pg_stat_xxx and pg_statio_xxx tables relation with pg_class
 */
void SnapshotNameSpace::InsertOneDbTables(uint64 curr_snapid, const List* tablesList)
{
    List* databaseList = NIL;
    SnapshotNameSpace::GetDatabaseList(&databaseList);
    foreach_cell(cell, databaseList)
    {
        List* row = (List*)lfirst(cell);
        SnapshotNameSpace::InsertDatabaseData((char*)linitial(row), curr_snapid, tablesList);
    }
    DeepListFree(databaseList, true);
}

/*
 * SpiltString
 *     @str :string will be split
 *    @delim :Splitter
 *    @res :splited strings
 */
void SnapshotNameSpace::SplitString(const char* str, const char* delim, List** res)
{
    errno_t rc;
    char* next_token = NULL;
    char* token = NULL;

    if (str == NULL) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("spilt str can not null")));
    }
    char* strs = (char*)palloc(strlen(str) + 1);
    rc = strncpy_s(strs, strlen(str) + 1, str, strlen(str));
    securec_check_c(rc, "\0", "\0");

    token = strtok_s(strs, delim, &next_token);
    while (token != NULL) {
        size_t strSize = strlen(token) + 1;
        char* subStr = (char*)palloc(strSize * sizeof(char));
        rc = strncpy_s(subStr, strSize, token, strSize - 1);
        securec_check_c(rc, "\0", "\0");
        *res = lappend(*res, subStr);
        token = strtok_s(NULL, delim, &next_token);
    }
    pfree(strs);
}

void SnapshotNameSpace::InitTableList(TablesList& tablesList)
{
    const int NAME_SIZE = 3;
    List* viewNames = NIL;
    const char* delim = "_";  // spliter
    const char* sql = "select viewname from pg_views where schemaname = 'dbe_perf'";

    SnapshotNameSpace::GetQueryData(sql, false, &viewNames);
    /* table such as global_stat_xxx ,global_statio_xxx, summary_stat_xxx or summary_statio_xxx
     * come from pg_class, only describe statistics in a database
     */
    MemoryContext old_context = MemoryContextSwitchTo(t_thrd.perf_snap_cxt.PerfSnapMemCxt);
    foreach_cell(cell, viewNames)
    {
        List* row = (List*)lfirst(cell);
        foreach_cell(rowCell, row)
        {
            List* splitName = NIL;
            SnapshotNameSpace::SplitString((char*)lfirst(rowCell), delim, &splitName);
            char* tmpStr = pstrdup((char*)lfirst(rowCell));
            if (splitName->length >= NAME_SIZE &&
                (strcmp((char*)lfirst(splitName->head), "summary") == 0 ||
                    strcmp((char*)lfirst(splitName->head), "global") == 0) &&
                (strcmp((char*)lsecond(splitName), "stat") == 0 || strcmp((char*)lsecond(splitName), "statio") == 0) &&
                (strcmp((char*)lthird(splitName), "user") == 0 || strcmp((char*)lthird(splitName), "all") == 0 ||
                    strcmp((char*)lthird(splitName), "sys") == 0)) {
                tablesList.oneDbTableList = lappend(tablesList.oneDbTableList, tmpStr);
            } else if (strcmp(tmpStr, "class_vital_info") == 0) {
                tablesList.lastOneDbTableList = lappend(tablesList.lastOneDbTableList, tmpStr);
            } else if (strcmp(tmpStr, "global_record_reset_time") == 0) {
                tablesList.lastStatTableList = lappend(tablesList.lastStatTableList, tmpStr);
            } else {
                tablesList.multiDbTableList = lappend(tablesList.multiDbTableList, tmpStr);
            }
            list_free_deep(splitName);
        }
    }
    (void)MemoryContextSwitchTo(old_context);
    DeepListFree(viewNames, false);
}
/*
 * GetDatabaseData
 * get other database data for snapshot
 */
void SnapshotNameSpace::InsertDatabaseData(const char* dbname, uint64 curr_snapid, const List* tableList)
{
    StringInfoData query;
    initStringInfo(&query);
    StringInfoData sql;
    initStringInfo(&sql);

    foreach_cell(cellViewName, tableList)
    {
        char* viewName = (char*)lfirst(cellViewName);
        if (is_skipped_view(viewName)) {
            continue;
        }

        SnapshotNameSpace::GetQueryStr(query, viewName, curr_snapid, dbname);
        resetStringInfo(&sql);
        CHECK_FOR_INTERRUPTS();
        appendStringInfo(&sql,
            "INSERT INTO  snapshot.tables_snap_timestamp"
            "(snapshot_id, db_name, tablename, start_ts) "
            "values(%lu, '%s', 'snap_%s', '%s')",
            curr_snapid,
            dbname,
            viewName,
            GetCurrentTimeStampString());
        if (!SnapshotNameSpace::ExecuteQuery(sql.data, SPI_OK_INSERT)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION), errmsg("insert into tables_snap_timestamp start time stamp failed")));
        }

        CHECK_FOR_INTERRUPTS();
        if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_INSERT)) {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("insert into snap_%s is failed", viewName)));
        }

        CHECK_FOR_INTERRUPTS();
        resetStringInfo(&sql);
        appendStringInfo(&sql,
            "update snapshot.tables_snap_timestamp set end_ts = '%s' "
            "where snapshot_id = %lu and db_name = '%s' and tablename = 'snap_%s'",
            GetCurrentTimeStampString(),
            curr_snapid,
            dbname,
            viewName);
        if (!SnapshotNameSpace::ExecuteQuery(sql.data, SPI_OK_UPDATE)) {
            ereport(
                ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("update tables_snap_timestamp end time stamp failed")));
        }

        resetStringInfo(&query);
    }
    pfree_ext(query.data);
    pfree_ext(sql.data);
}

static bool IsNeedCreateIndex(const char* indexName)
{
    Datum colval;
    bool isNull = false;
    StringInfoData query;

    initStringInfo(&query);
    /* check the index which is existing or not */
    appendStringInfo(&query, "select count(*) from pg_class where relname = '%s' and relkind = 'i'", indexName);
    colval = GetDatumValue(query.data, 0, 0, &isNull);
    if (DatumGetInt32(colval)) {
        return false;
    }
    pfree_ext(query.data);
    return true;
}
/*
 In order to accelerate query for awr report, the index of some tables need to create
 The index is created immediately after whose table has existed at the start phase
*/
void SnapshotNameSpace::CreateIndexes(void)
{
    StringInfoData query;
    initStringInfo(&query);

    /* snap_summary_statio_all_indexes */
    if (IsNeedCreateIndex("snap_summary_statio_indexes_name")) {
        appendStringInfo(&query,
            "create index snap_summary_statio_indexes_name on"
            " snapshot.snap_summary_statio_all_indexes(db_name, snap_schemaname, snap_relname, snap_indexrelname);");

        if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_UTILITY)) {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("create index failed")));
        }
    }

    /* snap_summary_statio_all_tables */
    if (IsNeedCreateIndex("snap_summary_statio_tables_name")) {
        resetStringInfo(&query);
        appendStringInfo(&query,
            "create index snap_summary_statio_tables_name on"
            " snapshot.snap_summary_statio_all_tables(db_name, snap_schemaname, snap_relname);");

        if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_UTILITY)) {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("create index failed")));
        }
    }

    /* snap_summary_stat_all_indexes */
    if (IsNeedCreateIndex("snap_summary_stat_indexes_name")) {
        resetStringInfo(&query);
        appendStringInfo(&query,
            "create index snap_summary_stat_indexes_name on"
            " snapshot.snap_summary_stat_all_indexes(db_name, snap_schemaname, snap_relname, snap_indexrelname);");

        if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_UTILITY)) {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("create index failed")));
        }
    }

    /* snap_summary_stat_all_tables */
    if (IsNeedCreateIndex("snap_summary_stat_tables_name")) {
        resetStringInfo(&query);
        appendStringInfo(&query,
            "create index snap_summary_stat_tables_name on"
            " snapshot.snap_summary_stat_all_tables(db_name, snap_schemaname, snap_relname);");

        if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_UTILITY)) {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("create index failed")));
        }
    }

    /* snap_class_vital_info */
    if (IsNeedCreateIndex("snap_class_info_name")) {
        resetStringInfo(&query);
        appendStringInfo(&query,
            "create index snap_class_info_name on"
            " snapshot.snap_class_vital_info(db_name, snap_schemaname, snap_relname);");
        if (!SnapshotNameSpace::ExecuteQuery(query.data, SPI_OK_UTILITY)) {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("create index failed")));
        }
    }
    pfree_ext(query.data);
}

void SnapshotNameSpace::SleepCheckInterrupt(uint32 minutes)
{
    const int ONE_SECOND = 1000000;
    const uint32 ONE_MINUTE = 60; /* 60s */

    uint32 waitCycle = ONE_MINUTE * minutes;
    for (uint32 i = 0; i < waitCycle; i++) {
        if (t_thrd.perf_snap_cxt.need_exit) {
            break;
        }
        pg_usleep(ONE_SECOND);
    }
}
static void SleepToNextTS(TimestampTz nextTimeStamp)
{
    const int ONE_SECOND = 1000000;
    while (!t_thrd.perf_snap_cxt.request_snapshot && (GetCurrentTimestamp() < nextTimeStamp)) {
        if (t_thrd.perf_snap_cxt.need_exit) {
            break;
        }
        pg_usleep(ONE_SECOND);
    }
}

static TimestampTz GetNextSnapshotTS(TimestampTz currTS)
{
    return currTS + u_sess->attr.attr_common.wdr_snapshot_interval * USECS_PER_MINUTE;
}

static void ProcessSignal(void)
{
    /*
     * Ignore all signals usually bound to some action in the postmaster,
     * except SIGHUP, SIGTERM and SIGQUIT.
     */
    (void)gspqsignal(SIGHUP, instr_snapshot_sighup_handler);
    (void)gspqsignal(SIGINT, request_snapshot);
    (void)gspqsignal(SIGTERM, instr_snapshot_exit); /* cancel current query and exit */
    (void)gspqsignal(SIGQUIT, quickdie);
    (void)gspqsignal(SIGUSR1, procsignal_sigusr1_handler);
    (void)gspqsignal(SIGALRM, SIG_IGN);
    (void)gspqsignal(SIGPIPE, SIG_IGN);
    (void)gspqsignal(SIGUSR2, SIG_IGN);
    (void)gspqsignal(SIGCHLD, SIG_DFL);
    (void)gspqsignal(SIGTTIN, SIG_DFL);
    (void)gspqsignal(SIGTTOU, SIG_DFL);
    (void)gspqsignal(SIGCONT, SIG_DFL);
    (void)gspqsignal(SIGWINCH, SIG_DFL);

    gs_signal_setmask(&t_thrd.libpq_cxt.UnBlockSig, NULL);
    (void)gs_signal_unblock_sigusr2();
    if (u_sess->proc_cxt.MyProcPort->remote_host) {
        pfree(u_sess->proc_cxt.MyProcPort->remote_host);
    }
    u_sess->proc_cxt.MyProcPort->remote_host = pstrdup("localhost");

    t_thrd.wlm_cxt.thread_node_group = &g_instance.wlm_cxt->MyDefaultNodeGroup;  // initialize the default value
    t_thrd.wlm_cxt.thread_climgr = &t_thrd.wlm_cxt.thread_node_group->climgr;
    t_thrd.wlm_cxt.thread_srvmgr = &t_thrd.wlm_cxt.thread_node_group->srvmgr;
}

/*
 * The port info is initial status in snapshot thread when select
 * the config_setting view. The getsockopt is performed when the following
 * parameters are 0. The sock is -1 which make getsockopt failed and
 * unexpected log is printed. In order to avoid this, default values are set to -1.
 */
void SnapshotNameSpace::ResetPortDefaultKeepalivesValue(void)
{
    Port* port = u_sess->proc_cxt.MyProcPort;
    if (port->default_keepalives_idle == 0) {
        port->default_keepalives_idle = -1;
    }
    if (port->default_keepalives_interval == 0) {
        port->default_keepalives_interval = -1;
    }
    if (port->default_keepalives_count == 0) {
        port->default_keepalives_count = -1;
    }
}

void SnapshotNameSpace::SetThrdCxt(void)
{
    t_thrd.mem_cxt.msg_mem_cxt = AllocSetContextCreate(TopMemoryContext,
        "MessageContext",
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE);
    /* Create the memory context we will use in the main loop. */
    t_thrd.mem_cxt.mask_password_mem_cxt = AllocSetContextCreate(TopMemoryContext,
        "MaskPasswordCtx",
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE);
    t_thrd.perf_snap_cxt.PerfSnapMemCxt = AllocSetContextCreate(TopMemoryContext,
        "SnapshotContext",
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE);
    /*
     * Create a resource owner to keep track of our resources (currently only
     * buffer pins).
     */
    t_thrd.utils_cxt.CurrentResourceOwner = ResourceOwnerCreate(NULL, "Snapshot");
}
static void SetMyproc()
{
    /* reset MyProcPid */
    t_thrd.proc_cxt.MyProcPid = gs_thread_self();
    /* record Start Time for logging */
    t_thrd.proc_cxt.MyStartTime = time(NULL);

    knl_thread_set_name("WDRSnapshot");
    u_sess->attr.attr_common.application_name = pstrdup("WDRSnapshot");
}
/*
 * This is the main function of the background thread
 * A node set to ccn by cm will call the SubSnapshotMain
 * function. Loop collection of snapshot information
 */
NON_EXEC_STATIC void SnapshotMain()
{
    ereport(LOG, (errmsg("snapshot thread is started")));
    pg_atomic_add_fetch_u32(&g_instance.stat_cxt.snapshot_thread_counter, 1);

    const int INTERVAL = 20000;  // 20 seconds
    char username[NAMEDATALEN] = {'\0'};

    /* we are a postmaster subprocess now */
    IsUnderPostmaster = true;
    JobSnapshotIAm();
    SetMyproc();
    if (IS_PGXC_COORDINATOR && IsPostmasterEnvironment) {
        on_shmem_exit(PGXCNodeCleanAndRelease, 0);
    }
    ProcessSignal();
    /* Early initialization */
    BaseInit();
#ifndef EXEC_BACKEND
    InitProcess();
#endif

    t_thrd.proc_cxt.PostInit->SetDatabaseAndUser((char*)pstrdup(DEFAULT_DATABASE), InvalidOid, username);
    t_thrd.proc_cxt.PostInit->InitSnapshotWorker();

    /* initialize funtion such as max(), min() */
    SetProcessingMode(NormalProcessing);
    /* Identify myself via ps */
    init_ps_display("WDR snapshot process", "", "", "");

    SnapshotNameSpace::SetThrdCxt();
    u_sess->proc_cxt.MyProcPort->SessionStartTime = GetCurrentTimestamp();
    SnapshotNameSpace::ResetPortDefaultKeepalivesValue();
    Reset_Pseudo_CurrentUserId();
    /* initialize current pool handles, it's also only once */
    exec_init_poolhandles();
    /* initialize function of sql such as now() */
    InitVecFuncMap();
    pgstat_bestart();
    pgstat_report_appname("WDRSnapshot");
    pgstat_report_activity(STATE_IDLE, NULL);
    while (!t_thrd.perf_snap_cxt.need_exit) {
        ReloadInfo();
        /*
         * First sleep and then judge CCN, mainly considering that
         * the wlm thread has not completed initialization
         * when the snapshot thread starts at startup.
         */
        pg_usleep(INTERVAL);
        if ((PgxcIsCentralCoordinator(g_instance.attr.attr_common.PGXCNodeName) || IS_SINGLE_NODE) &&
            u_sess->attr.attr_common.enable_wdr_snapshot) {
            /* to avoid dead lock with redis, disable snapshot during redistribution */
            check_snapshot_thd_exit();
            SnapshotNameSpace::SubSnapshotMain();
        }
        pgstat_report_activity(STATE_IDLE, NULL);
    }
    gs_thread_exit(0);
}

/*
 * To avoid generate below message,
 * "Do analyze for them in order to generate optimized plan",
 * request analyze from snapshot thread.
 */
static void analyze_snap_table(TablesList& tablesList)
{
    int rc = 0;
    StartTransactionCommand();
    if ((rc = SPI_connect()) != SPI_OK_CONNECT) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
                errmsg("analyze table, connection failed: %s", SPI_result_code_string(rc))));
    }
    SnapshotNameSpace::GetAnalyzeList(&(tablesList.analyzeTableList));
    set_lock_timeout();
    SnapshotNameSpace::AnalyzeTable(tablesList.analyzeTableList);
    SPI_finish();
    CommitTransactionCommand();
}

void InitSnapshot(TablesList& tablesList)
{
    /* All the tables list will be initialized once. when database is restarted or powered on */
    StartTransactionCommand();
    int rc = 0;
    /* connect SPI to execute query */
    if ((rc = SPI_connect()) != SPI_OK_CONNECT) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
                errmsg("snapshot thread SPI_connect failed: %s", SPI_result_code_string(rc))));
    }
    set_lock_timeout();
    SnapshotNameSpace::InitTables(tablesList);
    SPI_finish();
    CommitTransactionCommand();
    ereport(LOG, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("create snapshot tables succeed")));
    analyze_snap_table(tablesList);
}
/*
 * Background thread for loop collection snapshot
 */
void SnapshotNameSpace::SubSnapshotMain(void)
{
    pgstat_report_activity(STATE_RUNNING, NULL);
    TimestampTz next_timestamp = GetCurrentTimestamp();
    TablesList tablesList;
    const int SLEEP_GAP_AFTER_ERROR = 1;
    InitSnapshot(tablesList);

    while (!t_thrd.perf_snap_cxt.need_exit &&
           (PgxcIsCentralCoordinator(g_instance.attr.attr_common.PGXCNodeName) || IS_SINGLE_NODE) &&
           u_sess->attr.attr_common.enable_wdr_snapshot) {
        /* 1. to avoid dead lock with redis, disable snapshot during redistribution
           2. to avoid insert failed, disable snapshot during upgrade */
        check_snapshot_thd_exit();

        ReloadInfo();
        if (!t_thrd.perf_snap_cxt.request_snapshot) {
            next_timestamp = GetNextSnapshotTS(GetCurrentTimestamp());
        }
        PG_TRY();
        {
            pgstat_report_activity(STATE_RUNNING, NULL);
            ereport(LOG, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("WDR snapshot start")));
            StartTransactionCommand();
            SnapshotNameSpace::take_snapshot(tablesList);
            CommitTransactionCommand();
            ereport(LOG, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("WDR snapshot end")));

            /*  a snapshot has token on next_timestamp, we need get next_timestamp */
            while (GetCurrentTimestamp() >= next_timestamp) {
                next_timestamp = GetNextSnapshotTS(next_timestamp);
            }
            t_thrd.perf_snap_cxt.request_snapshot = false;
            pgstat_report_activity(STATE_IDLE, NULL);
            if (t_thrd.perf_snap_cxt.got_SIGHUP) {
                t_thrd.perf_snap_cxt.got_SIGHUP = false;
                ProcessConfigFile(PGC_SIGHUP);
            }
            SleepToNextTS(next_timestamp);
        }
        PG_CATCH();
        {
            EmitErrorReport();
            FlushErrorState();
            ereport(WARNING, (errcode(ERRCODE_WARNING), errmsg("WDR snapshot failed")));
            AbortCurrentTransaction();
            pgstat_report_activity(STATE_IDLE, NULL);
            t_thrd.perf_snap_cxt.request_snapshot = false;
            SnapshotNameSpace::SleepCheckInterrupt(SLEEP_GAP_AFTER_ERROR);
        }
        PG_END_TRY();
    }
}
