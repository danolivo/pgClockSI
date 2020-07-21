/*
 * csn_log.h
 *
 * Commit-Sequence-Number log.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/csn_log.h
 */
#ifndef CSNLOG_H
#define CSNLOG_H

#include "access/xlog.h"
#include "utils/snapshot.h"

/* XLOG stuff */
#define XLOG_CSN_ASSIGNMENT         0x00
#define XLOG_CSN_SETXIDCSN       0x10
#define XLOG_CSN_ZEROPAGE           0x20
#define XLOG_CSN_TRUNCATE           0x30

typedef struct xl_xidcsn_set
{
	XidCSN		  xidcsn;
	TransactionId xtop;			/* XID's top-level XID */
	int			nsubxacts;		/* number of subtransaction XIDs */
	TransactionId xsub[FLEXIBLE_ARRAY_MEMBER];	/* assigned subxids */
} xl_xidcsn_set;

#define MinSizeOfXidCSNSet offsetof(xl_xidcsn_set, xsub)
#define	CSNAddByNanosec(csn,second) (csn + second * 1000000000L)

extern void CSNLogSetCSN(TransactionId xid, int nsubxids,
							   TransactionId *subxids, XidCSN csn, bool write_xlog);
extern XidCSN CSNLogGetCSNByXid(TransactionId xid);

extern Size CSNLogShmemSize(void);
extern void CSNLogShmemInit(void);
extern void BootStrapCSNLog(void);
extern void ShutdownCSNLog(void);
extern void CheckPointCSNLog(void);
extern void ExtendCSNLog(TransactionId newestXact);
extern void TruncateCSNLog(TransactionId oldestXact);

extern void csnlog_redo(XLogReaderState *record);
extern void csnlog_desc(StringInfo buf, XLogReaderState *record);
extern const char *csnlog_identify(uint8 info);
extern void WriteAssignCSNXlogRec(XidCSN xidcsn);
extern void set_last_max_csn(XidCSN xidcsn);
extern void set_last_log_wal_csn(XidCSN xidcsn);
extern XidCSN get_last_log_wal_csn(void);
extern void set_xmin_for_csn(void);

#endif   /* CSNLOG_H */