/*
 * bgheap.c
 *
 * PostgreSQL integrated cleaner of HEAP and INDEX relations
 *
 * Made in autovacuum analogy. Uses 'Target' strategy for clean relations,
 * without full scan.
 *
 * Copyright (c) 1996-2018, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/bgheap.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"


#include <unistd.h>
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "commands/vacuum.h"
#include "common/ip.h"
#include "executor/executor.h"
#include "libpq/pqsignal.h"
#include "postmaster/bgheap.h"
#include "postmaster/bgworker.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/ps_status.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/syscache.h"
#include "utils/timeout.h"

/* Number of work items in a launcher/worker shared buffer */
#define WORK_ITEMS_MAX	(256)

/* Minimal data for cleanup a block of heap relation and its index relations */
typedef struct CleanerMessage
{
	Oid	relid;		/* Relation ID */
	Oid	dbNode;		/* Database ID */
	Oid	spcNode;	/* Tablespace ID*/
	int	blkno;		/* Block number */
} CleanerMessage;

/*
 * Shared memory data to control and two-way communication with worker
 */
typedef struct WorkerInfoData
{
	dlist_node		links;
	Oid				dbOid;					/* Database ID of the worker */
	TimestampTz 	launchtime;				/* To define a time of last worker activity */
	int				pid;					/* Used for system signals passing */
	CleanerMessage	buffer[WORK_ITEMS_MAX];	/* Array of work items */
	int				nitems;					/* Number of work items in buffer */
	LWLock			WorkItemLock;			/* Locker for safe buffer access */
	int				id;						/* Used for Internal launcher buffers management */
} WorkerInfoData;

typedef struct WorkerInfoData *WorkerInfo;

/*
 * Shared memory lists.
 * Launcher get an element from freeWorkers list and init startingWorker value.
 * Worker set startingWorker to NULL value after startup and add himself
 * to runningWorkers list.
 */
typedef struct
{
	dlist_head	freeWorkers;
	dlist_head	runningWorkers;
	WorkerInfo	startingWorker;
} HeapCleanerShmemStruct;

/*
 * Element of waiting list
 */
typedef struct WorkWaitingList
{
	dlist_node		links;
	CleanerMessage	msg;
} WorkWaitingList;

static bool am_heapcleaner_launcher = false;
static bool am_heapcleaner_worker = false;

int heapcleaner_max_workers = 10;

static WorkerInfo MyWorkerInfo = NULL;

static HeapCleanerShmemStruct *HeapCleanerShmem;

static dlist_head DatabaseList = DLIST_STATIC_INIT(DatabaseList);
static MemoryContext DatabaseListCxt = NULL;

/* Signal handling */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t got_SIGTERM = false;
static volatile sig_atomic_t got_SIGUSR2 = false;

NON_EXEC_STATIC pgsocket RelCleanerSock = PGINVALID_SOCKET;
static struct sockaddr_storage pgStatAddr;

static int TrancheId;

/*
 * Waiting lists: one for each active worker and one for all messages
 * intended to not-running workers. Reasons:
 * 1. Worker in startup process.
 * 2. Another worker in startup and we can't launch new worker.
 * 3. We have not free slots for new workers.
 */
static dlist_head	*WaitingList;

#ifdef EXEC_BACKEND
static pid_t hclauncher_forkexec(void);
static pid_t hcworker_forkexec(void);
#endif

NON_EXEC_STATIC void HeapCleanerLauncherMain(int argc, char *argv[]) pg_attribute_noreturn();
NON_EXEC_STATIC void HeapCleanerWorkerMain(int argc, char *argv[]) pg_attribute_noreturn();

static void index_cleanup(Oid spcNode, Oid relNode, BlockNumber blkno);
static void launch_worker(Oid dbNode);
static WorkerInfo look_for_worker(Oid dbNode);
static void main_launcher_loop(void);
static void main_worker_loop(void);

static CleanerMessage pop_waiting_list(int listId);
static void push_waiting_list(int listId, CleanerMessage *msg);

static void SIGHUP_Handler(SIGNAL_ARGS);
static void SIGTERM_Handler(SIGNAL_ARGS);
static void SIGUSR2_Handler(SIGNAL_ARGS);
static bool try_send_message(WorkerInfo worker, CleanerMessage *msg);

#ifdef EXEC_BACKEND
/*
 * forkexec routine for the autovacuum launcher process.
 *
 * Format up the arglist, then fork and exec.
 */
static pid_t
hclauncher_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkhclauncher";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}

/*
 * We need this set from the outside, before InitProcess is called
 */
void
HeapCleanerLauncherIAm(void)
{
	am_heapcleaner_launcher = true;
}

static pid_t
hcworker_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkhcworker";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}

/*
 * We need this set from the outside, before InitProcess is called
 */
void
HeapCleanerWorkerIAm(void)
{
	am_heapcleaner_worker = true;
}
#endif

/*
 * Free any launcher resources before close process
 */
static void
FreeLauncherInfo(int code, Datum arg)
{
	pfree(WaitingList);
}

/*
 * Return a WorkerInfo to the free list
 */
static void
FreeWorkerInfo(int code, Datum arg)
{
	if (MyWorkerInfo != NULL)
	{
		LWLockAcquire(HeapCleanerLock, LW_EXCLUSIVE);

		dlist_delete(&MyWorkerInfo->links);
		MyWorkerInfo->dbOid = InvalidOid;
		MyWorkerInfo->launchtime = 0;
		dlist_push_head(&HeapCleanerShmem->freeWorkers,
						&MyWorkerInfo->links);
		/* not mine anymore */
		MyWorkerInfo = NULL;

		LWLockRelease(HeapCleanerLock);
	}
	else
		elog(ERROR, "---> MyWorkerInfo is NULL");
}

/*
 * Initialize communication with backends
 */
void
HeapCleanerInit()
{
	struct addrinfo *addrs = NULL,
			*addr, hints;
	ACCEPT_TYPE_ARG3 alen;
	int				ret;
	fd_set		rset;
	struct timeval tv;
	char		test_byte;
	int			sel_res;
	int			tries = 0;

#define TESTBYTEVAL ((char) 199)

	/*
	 * Create the UDP socket for sending and receiving statistic messages
	 */
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_addr = NULL;
	hints.ai_canonname = NULL;
	hints.ai_next = NULL;
	ret = pg_getaddrinfo_all("localhost", NULL, &hints, &addrs);
	Assert(ret == 0);

	for (addr = addrs; addr; addr = addr->ai_next)
	{
#ifdef HAVE_UNIX_SOCKETS
		/* Ignore AF_UNIX sockets, if any are returned. */
		if (addr->ai_family == AF_UNIX)
			continue;
#endif

		if (++tries > 1)
			ereport(LOG,
					(errmsg("trying another address for the statistics collector")));

		if ((RelCleanerSock = socket(addr->ai_family, SOCK_DGRAM, 0)) == PGINVALID_SOCKET)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					errmsg("could not create socket for relation cleaner: %m")));
			continue;
		}

		if (bind(RelCleanerSock, addr->ai_addr, addr->ai_addrlen) < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not bind socket for relation cleaner: %m")));
			closesocket(RelCleanerSock);
			RelCleanerSock = PGINVALID_SOCKET;
			continue;
		}

		alen = sizeof(pgStatAddr);
		if (getsockname(RelCleanerSock, (struct sockaddr *) &pgStatAddr, &alen) < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not get address of socket for statistics collector: %m")));
			closesocket(RelCleanerSock);
			RelCleanerSock = PGINVALID_SOCKET;
			continue;
		}

		if (connect(RelCleanerSock, (struct sockaddr *) &pgStatAddr, alen) < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not connect socket for statistics collector: %m")));
			closesocket(RelCleanerSock);
			RelCleanerSock = PGINVALID_SOCKET;
			continue;
		}
		/*
		 * Try to send and receive a one-byte test message on the socket. This
		 * is to catch situations where the socket can be created but will not
		 * actually pass data (for instance, because kernel packet filtering
		 * rules prevent it).
		 */
		test_byte = TESTBYTEVAL;

retry1:
		if (send(RelCleanerSock, &test_byte, 1, 0) != 1)
		{
			if (errno == EINTR)
				goto retry1;	/* if interrupted, just retry */
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not send test message on socket for statistics collector: %m")));
			closesocket(RelCleanerSock);
			RelCleanerSock = PGINVALID_SOCKET;
			continue;
		}

		/*
		 * There could possibly be a little delay before the message can be
		 * received.  We arbitrarily allow up to half a second before deciding
		 * it's broken.
		 */
		for (;;)				/* need a loop to handle EINTR */
		{
			FD_ZERO(&rset);
			FD_SET(RelCleanerSock, &rset);

			tv.tv_sec = 0;
			tv.tv_usec = 500000;
			sel_res = select(RelCleanerSock + 1, &rset, NULL, NULL, &tv);
			if (sel_res >= 0 || errno != EINTR)
				break;
		}
		if (sel_res < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("select() failed in statistics collector: %m")));
			closesocket(RelCleanerSock);
			RelCleanerSock = PGINVALID_SOCKET;
			continue;
		}
		if (sel_res == 0 || !FD_ISSET(RelCleanerSock, &rset))
		{
			/*
			 * This is the case we actually think is likely, so take pains to
			 * give a specific message for it.
			 *
			 * errno will not be set meaningfully here, so don't use it.
			 */
			ereport(LOG,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("test message did not get through on socket for statistics collector")));
			closesocket(RelCleanerSock);
			RelCleanerSock = PGINVALID_SOCKET;
			continue;
		}

		test_byte++;			/* just make sure variable is changed */

retry2:
		if (recv(RelCleanerSock, &test_byte, 1, 0) != 1)
		{
			if (errno == EINTR)
				goto retry2;	/* if interrupted, just retry */
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not receive test message on socket for statistics collector: %m")));
			closesocket(RelCleanerSock);
			RelCleanerSock = PGINVALID_SOCKET;
			continue;
		}

		if (test_byte != TESTBYTEVAL)	/* strictly paranoia ... */
		{
			ereport(LOG,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("incorrect test message transmission on socket for statistics collector")));
			closesocket(RelCleanerSock);
			RelCleanerSock = PGINVALID_SOCKET;
			continue;
		}

		/* If we get here, we have a working socket */

		break;
	}

	/* Did we find a working address? */
	if (!addr || RelCleanerSock == PGINVALID_SOCKET)
		goto startup_failed;

	/*
	 * Set the socket to non-blocking IO.  This ensures that if the collector
	 * falls behind, statistics messages will be discarded; backends won't
	 * block waiting to send messages to the collector.
	 */
	if (!pg_set_noblock(RelCleanerSock))
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not set statistics collector socket to nonblocking mode: %m")));
		goto startup_failed;
	}
	pg_freeaddrinfo_all(hints.ai_family, addrs);

	return;

startup_failed:
	ereport(LOG,
			(errmsg("disabling heap cleaner for lack of working socket")));

	if (addrs)
		pg_freeaddrinfo_all(hints.ai_family, addrs);

	if (RelCleanerSock != PGINVALID_SOCKET)
		closesocket(RelCleanerSock);
	RelCleanerSock = PGINVALID_SOCKET;
}

/*
 * Start and initialization logic of a launcher
 */
NON_EXEC_STATIC void
HeapCleanerLauncherMain(int argc, char *argv[])
{
	sigjmp_buf		local_sigjmp_buf;
	MemoryContext	bgheap_context;

	am_heapcleaner_launcher = true;

	/*
	 * Identify myself via ps
	 */
	init_ps_display(pgstat_get_backend_desc(B_BG_HEAPCLNR_LAUNCHER), "", "", "");

	SetProcessingMode(InitProcessing);

	pqsignal(SIGHUP, SIGHUP_Handler);	/* set flag to read config file */
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, SIGTERM_Handler);
	pqsignal(SIGQUIT, quickdie); /* hard crash time */
	InitializeTimeouts();
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	BaseInit();

#ifndef EXEC_BACKEND
	InitProcess();
#endif

	InitPostgres(NULL, InvalidOid, NULL, InvalidOid, NULL, false);

	SetProcessingMode(NormalProcessing);

	/*
	 * Create a resource owner to keep track of our resources (currently only
	 * buffer pins).
	 */
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "Background cleaner");

	bgheap_context = AllocSetContextCreate(TopMemoryContext,
												"Background cleaner",
												ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(bgheap_context);

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Forget any pending QueryCancel or timeout request */
		disable_all_timeouts(false);
		/* Report the error to the server log */
		EmitErrorReport();

		LWLockReleaseAll();
		pgstat_report_wait_end();
		AbortBufferIO();
		UnlockBuffers();
		if (CurrentResourceOwner)
			/* buffer pins are released here: */
			ResourceOwnerRelease(CurrentResourceOwner,
									RESOURCE_RELEASE_BEFORE_LOCKS,
									false, true);
		/* we needn't bother with the other ResourceOwnerRelease phases */
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(bgheap_context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextResetAndDeleteChildren(bgheap_context);

		/* don't leave dangling pointers to freed memory */
		DatabaseListCxt = NULL;
		dlist_init(&DatabaseList);

		/*
		 * Make sure pgstat also considers our stat data as gone.  Note: we
		 * mustn't use autovac_refresh_stats here.
		 */
		pgstat_clear_snapshot();

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		if (got_SIGTERM)
			proc_exit(0);
		/*
		 * Sleep at least 1 second after any error.  A write error is likely
		 * to be repeated, and we don't want to be filling the error logs as
		 * fast as we can.
		 */
		pg_usleep(1000000L);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	PG_SETMASK(&UnBlockSig);

	WaitingList = palloc((heapcleaner_max_workers+1) * sizeof(dlist_head));
	on_shmem_exit(FreeLauncherInfo, 0);

	main_launcher_loop();

	proc_exit(0);
}

/*
 * HeapCleanerShmemInit
 *		Allocate and initialize heapcleaner-related shared memory
 */
void
HeapCleanerShmemInit(void)
{
	bool		found;

	HeapCleanerShmem = (HeapCleanerShmemStruct *)
		ShmemInitStruct("HeapCleaner Data",
						HeapCleanerShmemSize(),
						&found);

	if (!IsUnderPostmaster)
	{
		WorkerInfo	worker;
		int			i;

		Assert(!found);

		dlist_init(&HeapCleanerShmem->freeWorkers);
		dlist_init(&HeapCleanerShmem->runningWorkers);

		worker = (WorkerInfo) ((char *) HeapCleanerShmem +
							   MAXALIGN(sizeof(HeapCleanerShmemStruct)));

		/* initialize the WorkerInfo free list */
		for (i = 0; i < heapcleaner_max_workers; i++)
		{
			worker[i].id = i;
			LWLockInitialize(&worker[i].WorkItemLock, TrancheId);
			dlist_push_head(&HeapCleanerShmem->freeWorkers,
							&worker[i].links);
		}
	}
	else
		Assert(found);
	HeapCleanerShmem->startingWorker = NULL;

	TrancheId = LWLockNewTrancheId();
	LWLockRegisterTranche(TrancheId, "heapcleaner");
}

/*
 * HeapCleanerShmemSiz
 *		Compute space needed for heap cleaner-related shared memory
 */
Size
HeapCleanerShmemSize(void)
{
	Size		size;

	/*
	 * Need the fixed struct and the array of WorkerInfoData.
	 */
	size = sizeof(HeapCleanerShmemStruct);
	size = MAXALIGN(size);
	size = add_size(size, mul_size(heapcleaner_max_workers,
								   sizeof(WorkerInfoData)));
	return size;
}

/*
 * Send a dirty block of relation to Heap cleaner launcher
 */
void
HeapCleanerSend(Relation relation, BlockNumber blkno)
{
	int		rc;
	CleanerMessage msg;

	if (RecoveryInProgress())
			return;

	if (RelCleanerSock == PGINVALID_SOCKET)
		return;

	msg.relid = RelationGetRelid(relation);
	msg.dbNode = relation->rd_node.dbNode;
	msg.spcNode = relation->rd_node.spcNode;
	msg.blkno = blkno;

	/* We'll retry after EINTR, but ignore all other failures */
	do
	{
		rc = send(RelCleanerSock, &msg, sizeof(CleanerMessage), 0);
	} while (rc < 0 && errno == EINTR);
}

/*
 * Start and initialization logic of a worker
 */
NON_EXEC_STATIC void
HeapCleanerWorkerMain(int argc, char *argv[])
{
	sigjmp_buf	local_sigjmp_buf;

	am_heapcleaner_worker = true;

	/* Identify myself via ps */
	init_ps_display(pgstat_get_backend_desc(B_BG_HEAPCLNR_WORKER), "", "", "");

	SetProcessingMode(InitProcessing);

	pqsignal(SIGHUP, SIGHUP_Handler);
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, SIGTERM_Handler);
	pqsignal(SIGQUIT, quickdie);
	InitializeTimeouts();		/* establishes SIGALRM handler */

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIGUSR2_Handler); /* Signal: Buffer contains a message */
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/* Early initialization */
	BaseInit();

#ifndef EXEC_BACKEND
	InitProcess();
#endif

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		proc_exit(0);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	PG_SETMASK(&UnBlockSig);

	/* Initialization steps */
	LWLockAcquire(HeapCleanerLock, LW_EXCLUSIVE);
	if (HeapCleanerShmem->startingWorker != NULL)
	{
		MyWorkerInfo = HeapCleanerShmem->startingWorker;
		MyWorkerInfo->pid = getpid();
		HeapCleanerShmem->startingWorker = NULL;

		/* insert into the running list */
		dlist_push_head(&HeapCleanerShmem->runningWorkers, &MyWorkerInfo->links);

		on_shmem_exit(FreeWorkerInfo, 0);
	}
	else
		elog(ERROR, "No Starting worker!");
	LWLockRelease(HeapCleanerLock);

	if (OidIsValid(MyWorkerInfo->dbOid))
	{
		char		dbname[NAMEDATALEN];

		InitPostgres(NULL, MyWorkerInfo->dbOid, NULL, InvalidOid, dbname, false);
		SetProcessingMode(NormalProcessing);
		set_ps_display(dbname, false);

		main_worker_loop();
	}
	else
		elog(ERROR, "dbid not valid!");

	proc_exit(0);
}

/*
 * Main logic of HEAP and index relations cleaning
 */
static void
index_cleanup(Oid spcNode, Oid relid, BlockNumber blkno)
{
	Relation		heapRelation;
	Relation   		*IndexRelations;
	int				nindexes;
	int				irnum;
	ItemPointerData	dead_tuples[MaxOffsetNumber];
	int				num_dead_tuples = 0;
	Buffer			buffer;
	OffsetNumber	offnum;
	Page 			page;
	ItemId 			lp;
	bool			needLock;
	BlockNumber		nblocks;

	LOCKMODE	lmode = AccessExclusiveLock; /* ShareUpdateExclusiveLock; */
	LockRelId	onerelid;
	int tnum;
	Oid toast_relid;
	bool found_non_nbtree = false;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;

	if (RecoveryInProgress())
		return;

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	CHECK_FOR_INTERRUPTS();

	/*
	 * At this point relation relation availability is not guaranteed.
	 * Make safe test to check this.
	 */
	if (ConditionalLockRelationOid(relid, lmode))
		heapRelation = try_relation_open(relid, AccessExclusiveLock);
	else
		heapRelation = NULL;

	if (!heapRelation)
	{
		PopActiveSnapshot();
		CommitTransactionCommand();
		return;
	}

	/* Now we not clean system tables because it will induce some
	 * random stdout logging and we need to change regression tests
	 * (it is frequently seen on DROP CASCADE operations)
	 */
	if (IsSystemNamespace(heapRelation->rd_rel->relnamespace))
	{
		relation_close(heapRelation, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return;
	}
	/*
	 * Check relation type similarly vacuum
	 */
	if (!(pg_class_ownercheck(RelationGetRelid(heapRelation), GetUserId()) ||
		  (pg_database_ownercheck(MyDatabaseId, GetUserId()) && !heapRelation->rd_rel->relisshared)))
	{
		if (heapRelation->rd_rel->relisshared)
			ereport(WARNING,
					(errmsg("skipping \"%s\" --- only superuser can vacuum it",
							RelationGetRelationName(heapRelation))));
		else if (heapRelation->rd_rel->relnamespace == PG_CATALOG_NAMESPACE)
			ereport(WARNING,
					(errmsg("skipping \"%s\" --- only superuser or database owner can vacuum it",
							RelationGetRelationName(heapRelation))));
		else
			ereport(WARNING,
					(errmsg("skipping \"%s\" --- only table or database owner can vacuum it",
							RelationGetRelationName(heapRelation))));
		relation_close(heapRelation, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return;
	}

	if (heapRelation->rd_rel->relkind != RELKIND_RELATION &&
			heapRelation->rd_rel->relkind != RELKIND_MATVIEW &&
			heapRelation->rd_rel->relkind != RELKIND_TOASTVALUE &&
			heapRelation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
	{
		ereport(WARNING,
				(errmsg("skipping \"%s\" --- cannot vacuum non-tables or special system tables",
						RelationGetRelationName(heapRelation))));
		relation_close(heapRelation, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return;
	}
	if (RELATION_IS_OTHER_TEMP(heapRelation))
	{
		relation_close(heapRelation, lmode);
		elog(LOG, "--- S problem");
		PopActiveSnapshot();
		CommitTransactionCommand();
		return;
	}

	if (heapRelation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		relation_close(heapRelation, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return;
	}

	onerelid = heapRelation->rd_lockInfo.lockRelId;
	LockRelationIdForSession(&onerelid, lmode);

	needLock = !RELATION_IS_LOCAL(heapRelation);
	if (needLock)
		LockRelationForExtension(heapRelation, ExclusiveLock);
	nblocks = RelationGetNumberOfBlocks(heapRelation);
	if (needLock)
		UnlockRelationForExtension(heapRelation, ExclusiveLock);

	if (blkno > nblocks)
	{
		relation_close(heapRelation, lmode);
		UnlockRelationIdForSession(&onerelid, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return;
	}

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(heapRelation->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	/* Create TID list */
	buffer = ReadBufferExtended(heapRelation, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = (Page) BufferGetPage(buffer);
	for (offnum = FirstOffsetNumber; offnum < PageGetMaxOffsetNumber(page); offnum = OffsetNumberNext(offnum))
	{
		lp = PageGetItemId(page, offnum);
		if (ItemIdIsDead(lp) && ItemIdHasStorage(lp))
		{
			ItemPointerSet(&(dead_tuples[num_dead_tuples]),blkno, offnum);
			num_dead_tuples++;
		}
	}

//	UnlockReleaseBuffer(buffer);

//	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	/* Open and lock index relations correspond to the heap relation */
	vac_open_indexes(heapRelation, RowExclusiveLock, &nindexes, &IndexRelations);

	/* Iterate across all index relations */
	for (irnum = 0; irnum < nindexes; irnum++)
	{
		if (IndexRelations[irnum]->rd_amroutine->amtargetdelete == NULL)
		{
			found_non_nbtree = true;
			continue;
		}

		quick_vacuum_index(IndexRelations[irnum], heapRelation,
						    dead_tuples,
							num_dead_tuples);

	}

	vac_close_indexes(nindexes, IndexRelations, NoLock);

	/* Roll back any GUC changes executed by index functions */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	if (!found_non_nbtree)
	{
		OffsetNumber	unusable[MaxOffsetNumber];
		int				nunusable = 0;

//		buffer = ReadBufferExtended(heapRelation, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
//		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		LockBufferForCleanup(buffer);

		START_CRIT_SECTION();

		page = BufferGetPage(buffer);
		/* Release DEAD heap tuples storage */
		for (tnum = 0; tnum < num_dead_tuples; tnum++)
		{
			OffsetNumber	offnum = ItemPointerGetOffsetNumber(&dead_tuples[tnum]);
			ItemId			lp = PageGetItemId(page, offnum);

			Assert(ItemIdIsDead(lp));
			ItemIdSetUnused(lp);
			unusable[nunusable++] = offnum;
		}

		if (nunusable > 0)
		{
			XLogRecPtr	recptr;

			((PageHeader) page)->pd_prune_xid = InvalidTransactionId;
			PageRepairFragmentation(page);
			PageClearFull(page);
			MarkBufferDirty(buffer);
			if (RelationNeedsWAL(heapRelation))
			{
				recptr = log_heap_clean(heapRelation, buffer,
								NULL, 0,
								NULL, 0,
								unusable, nunusable,
								InvalidTransactionId);

				PageSetLSN(BufferGetPage(buffer), recptr);
			}
		}

		END_CRIT_SECTION();
		UnlockReleaseBuffer(buffer);
	}

	toast_relid = heapRelation->rd_rel->reltoastrelid;
	relation_close(heapRelation, NoLock);
	PopActiveSnapshot();
	CommitTransactionCommand();

	/* Clean next TOAST relation */
	if (toast_relid != InvalidOid)
		index_cleanup(spcNode, toast_relid, blkno);

	UnlockRelationIdForSession(&onerelid, lmode);
}

/*
 * IsHeapCleaner functions
 *		Return whether this is either a launcher heap cleaner process or
 *		a worker process.
 */
bool
IsHeapCleanerLauncherProcess(void)
{
	return am_heapcleaner_launcher;
}

bool
IsHeapCleanerWorkerProcess(void)
{
	return am_heapcleaner_worker;
}

/*
 * Send signal to Postmaster for launch new worker instance.
 * Eject one node from freeWorkers list and assign to startingWorker
 * HeapCleanerLock must be exclusive-locked
 */
static void
launch_worker(Oid dbNode)
{
	WorkerInfo worker;
	dlist_node *wptr;

	if (dlist_is_empty(&HeapCleanerShmem->freeWorkers))
		elog(ERROR, "NO a free slot for background cleaner worker!");

	wptr = dlist_pop_head_node(&HeapCleanerShmem->freeWorkers);
	worker = dlist_container(WorkerInfoData, links, wptr);
	worker->dbOid = dbNode;
	worker->launchtime = GetCurrentTimestamp();
	worker->nitems = 0;
	HeapCleanerShmem->startingWorker = worker;
	dlist_init(&WaitingList[worker->id]);

	SendPostmasterSignal(PMSIGNAL_START_HEAPCLNR_WORKER);
}

/*
 * Return worker that initialized for dbNode database or NULL
 * Caller need to acquire share lock on HeapCleanerLock
 */
static WorkerInfo
look_for_worker(Oid dbNode)
{
	dlist_node	*node;
	WorkerInfo worker = NULL;

	if (!dlist_is_empty(&HeapCleanerShmem->runningWorkers))
		for (node = dlist_head_node(&HeapCleanerShmem->runningWorkers);
			 ;
			 node = dlist_next_node(&HeapCleanerShmem->runningWorkers, node))
		{
			if (((WorkerInfo)node)->dbOid == dbNode)
			{
				worker = (WorkerInfo) node;
				break;
			}
			if (!dlist_has_next(&HeapCleanerShmem->runningWorkers, node))
				break;
		}

	return worker;
}

/*
 * Entry point of a launcher behavior logic
 */
static void
main_launcher_loop()
{
	Assert(RelCleanerSock != PGINVALID_SOCKET);

	while (!got_SIGTERM)
	{
		int				rc;
		int				len;
		CleanerMessage	msg;
		WorkerInfo		startingWorker;
		long			timeout;
		dlist_node		*node;
		WorkerInfo		worker;
		bool			haveWork = false;

		/* Process system signals */
		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		ResetLatch(MyLatch);

		/* At First, receive a message from backend */
		len = recv(RelCleanerSock, &msg, sizeof(CleanerMessage), 0);

		LWLockAcquire(HeapCleanerLock, LW_EXCLUSIVE);
		startingWorker = HeapCleanerShmem->startingWorker;

		if (len > 0)
		{
			WorkerInfo worker;

			/* Base consistency check */
			if (len != sizeof(CleanerMessage))
				elog(ERROR, "Cleaner message size not consistent: %d. expected: %lu", len, sizeof(CleanerMessage));

			worker = look_for_worker(msg.dbNode);

			if (worker == NULL)
				/* Add to special list, which contains messages to non-launched workers */
				push_waiting_list(heapcleaner_max_workers, &msg);

			else if (dlist_is_empty(&WaitingList[worker->id]))
			{


				// TODO: check WaitList

				/* Try to send into the shared buffer */
				if (!try_send_message(worker, &msg))
					push_waiting_list(worker->id, &msg);
			}
			else
				/* Worker is lazy */
				push_waiting_list(worker->id, &msg);
		}

		if ((startingWorker == NULL) && !dlist_is_empty(&WaitingList[heapcleaner_max_workers]))
		{
			bool startWorker = false;

			/*
			 * Try to send messages to active worker
			 */
			for (node = dlist_tail_node(&WaitingList[heapcleaner_max_workers]);
				 ;
				 node = dlist_prev_node(&WaitingList[heapcleaner_max_workers], node))
				{
					msg = ((WorkWaitingList *) node)->msg;

					worker = look_for_worker(msg.dbNode);
					if (worker)
					{
						/* Send message to young worker or save to the private wait list */
						if (!try_send_message(worker, &msg))
							push_waiting_list(worker->id, &msg);
						dlist_delete(node);

					} else if (!startWorker)
					{
						/* Start new worker */
						launch_worker(msg.dbNode);
						startWorker = true;
					}

					if (!dlist_has_prev(&WaitingList[heapcleaner_max_workers], node))
						break;
				}
		}

		if (!dlist_is_empty(&WaitingList[heapcleaner_max_workers]))
			haveWork = true;

		/* See waiting lists of active workers and try to send messages */
		if (!dlist_is_empty(&HeapCleanerShmem->runningWorkers))
			for (node = dlist_head_node(&HeapCleanerShmem->runningWorkers);
				 ;
				 node = dlist_next_node(&HeapCleanerShmem->runningWorkers, node))
			{
				worker = (WorkerInfo) node;

				if (dlist_is_empty(&WaitingList[worker->id]))
				{
					if (!dlist_has_next(&HeapCleanerShmem->runningWorkers, node))
						break;
					else
						continue;
				}

				LWLockAcquire(&MyWorkerInfo->WorkItemLock, LW_EXCLUSIVE);
				while ((worker->nitems < WORK_ITEMS_MAX) && !dlist_is_empty(&WaitingList[worker->id]))
					worker->buffer[worker->nitems++] = pop_waiting_list(worker->id);
				LWLockRelease(&MyWorkerInfo->WorkItemLock);

				if (!dlist_is_empty(&WaitingList[worker->id]))
					haveWork = true;

				/* You have a work! */
				kill(worker->pid, SIGUSR2);

				if (!dlist_has_next(&HeapCleanerShmem->runningWorkers, node))
					break;
			}

		LWLockRelease(HeapCleanerLock);

		if (!haveWork)
		{
			timeout = -1L;
			/* Wait data or signals */
			rc = WaitLatchOrSocket(MyLatch,
							WL_LATCH_SET | WL_POSTMASTER_DEATH | WL_SOCKET_READABLE,
							RelCleanerSock, timeout,
							WAIT_EVENT_BGHEAP_MAIN);
		}
/*		else
		{
			pg_usleep(1);
			continue;
		}
*/
		/* Emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
		{
			elog(LOG, "Heap Launcher exit with 1");
			proc_exit(1);
		}
	}
	{
		FILE *f = fopen("/home/andrey/test.log", "a+");
		fprintf(f, "Heap Launcher exit with 0\n");
		fclose(f);
	}
	elog(LOG, "Heap Launcher exit with 0");
	proc_exit(0);
}

/*
 * Entry point of a worker behavior logic
 */
static void
main_worker_loop(void)
{
	CleanerMessage	lbuf[WORK_ITEMS_MAX];
	int			nitems;

	while (!got_SIGTERM)
	{
		int	rc;

		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		if (got_SIGUSR2)
			/* It is needed only for wakeup worker */
			got_SIGUSR2 = false;

		/* Task buffer is not empty */
		LWLockAcquire(&MyWorkerInfo->WorkItemLock, LW_SHARED);
		if (MyWorkerInfo->nitems > 0)
		{
			nitems = MyWorkerInfo->nitems;
			memcpy(lbuf, MyWorkerInfo->buffer, nitems*sizeof(CleanerMessage));
			MyWorkerInfo->nitems = 0;
		}
		LWLockRelease(&MyWorkerInfo->WorkItemLock);

		if (nitems > 0)
		{
			int i;

			PG_TRY();
			{
				for (i=0; i < nitems; i++)
					index_cleanup(lbuf[i].spcNode, lbuf[i].relid, lbuf[i].blkno);

				nitems = 0;
				QueryCancelPending = false;
			}
			PG_CATCH();
			{
				HOLD_INTERRUPTS();
				EmitErrorReport();
				AbortOutOfAnyTransaction();
				FlushErrorState();

				RESUME_INTERRUPTS();
			}
			PG_END_TRY();
		}

		/* Wait data or signals */
		rc = WaitLatch(MyLatch,
				WL_LATCH_SET /*| WL_TIMEOUT */| WL_POSTMASTER_DEATH,
				-1L,
				WAIT_EVENT_BGHEAP_MAIN);

		ResetLatch(MyLatch);

		/* Emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);
	}

	elog(LOG, "HEAP Cleaner (worker) exit with 0");
	proc_exit(0);
}

/*
 * Pop a message from the end of waiting list and free memory
 */
static CleanerMessage
pop_waiting_list(int listId)
{
	dlist_node *wptr;
	WorkWaitingList	*elem;
	CleanerMessage msg;

	Assert(!dlist_is_empty(&WaitingList[listId]));
	wptr = dlist_tail_node(&WaitingList[listId]);
	elem = dlist_container(WorkWaitingList, links, wptr);
	msg = elem->msg;
	dlist_delete(&elem->links);
	pfree(wptr);
	return msg;
}

/*
 * Push a message into the head of waiting list
 */
static void
push_waiting_list(int listId, CleanerMessage *msg)
{
	WorkWaitingList *elem = palloc(sizeof(WorkWaitingList));

	memcpy(&elem->msg, msg, sizeof(CleanerMessage));
	dlist_push_head(&WaitingList[listId], &elem->links);
}

/*
 * SIGHUP_Handler
 */
static void
SIGHUP_Handler(SIGNAL_ARGS)
{
	int	save_errno = errno;

	got_SIGHUP = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * SIGTERM_Handler
 */
static void
SIGTERM_Handler(SIGNAL_ARGS)
{
	int save_errno = errno;

	got_SIGTERM = true;

	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * SIGUSR2_Handler
 * Wake up a worker to read some messages from launcher
 */
static void
SIGUSR2_Handler(SIGNAL_ARGS)
{
	int save_errno = errno;

	got_SIGUSR2 = true;

	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * Main entry point for background heap cleaner (launcher) process, to be called from the
 * postmaster.
 */
int
StartHeapCleanerLauncher(void)
{
	pid_t		HeapCleanerLauncherPID;

#ifdef EXEC_BACKEND
	switch ((HeapCleanerLauncherPID = bghclauncher_forkexec()))
#else
	switch ((HeapCleanerLauncherPID = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork background heap cleaner (launcher) process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			InitPostmasterChild();

			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

			HeapCleanerLauncherMain(0, NULL);
			break;
#endif
		default:
			return (int) HeapCleanerLauncherPID;
	}

	/* shouldn't get here */
	return 0;
}

/*
 * Main entry point for background heap cleaner (worker) process, to be called from the
 * postmaster.
 */
int
StartHeapCleanerWorker(void)
{
	pid_t		worker_pid;

#ifdef EXEC_BACKEND
	switch ((worker_pid = hcworker_forkexec()))
#else
	switch ((worker_pid = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork background heap cleaner (worker) process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			InitPostmasterChild();

			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

			HeapCleanerWorkerMain(0, NULL);
			break;
#endif
		default:
			return (int) worker_pid;
	}

	/* shouldn't get here */
	return 0;
}

/*
 * Send a message to worker, if it possible.
 */
static bool
try_send_message(WorkerInfo worker, CleanerMessage *msg)
{
	bool result = false;

	LWLockAcquire(&worker->WorkItemLock, LW_EXCLUSIVE);
	if (worker->nitems < WORK_ITEMS_MAX)
	{
		memcpy(&worker->buffer[worker->nitems], msg, sizeof(CleanerMessage));
		worker->nitems++;

		/* Worker will do a work */
		worker->launchtime = GetCurrentTimestamp();

		result = true;
	}
	LWLockRelease(&worker->WorkItemLock);

	/* Notify the worker */
	kill(worker->pid, SIGUSR2);

	return result;
}
