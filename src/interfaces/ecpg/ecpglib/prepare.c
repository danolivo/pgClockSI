/* src/interfaces/ecpg/ecpglib/prepare.c */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include <ctype.h>

#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "ecpglib_extern.h"
#include "sqlca.h"

#define STMTID_SIZE 32

/*
 * The statement cache contains stmtCacheNBuckets hash buckets, each
 * having stmtCacheEntPerBucket entries, which we recycle as needed,
 * giving up the least-executed entry in the bucket.
 * stmtCacheEntries[0] is never used, so that zero can be a "not found"
 * indicator.
 */
#define stmtCacheNBuckets		2039	/* should be a prime number */
#define stmtCacheEntPerBucket	8

#define stmtCacheArraySize (stmtCacheNBuckets * stmtCacheEntPerBucket + 1)

typedef struct
{
	int			lineno;
	char		stmtID[STMTID_SIZE];
	char	   *ecpgQuery;
	long		execs;			/* # of executions */
	const char *connection;		/* connection for the statement */
} stmtCacheEntry;

static int	nextStmtID = 1;
static stmtCacheEntry *stmtCacheEntries = NULL;
static struct declared_statement *g_declared_list; 

static bool deallocate_one(int lineno, enum COMPAT_MODE c, struct connection *con,
			   struct prepared_statement *prev, struct prepared_statement *this);
static struct declared_statement *ecpg_find_declared_statement(const char *); 
static bool
isvarchar(unsigned char c)
{
	if (isalnum(c))
		return true;

	if (c == '_' || c == '>' || c == '-' || c == '.')
		return true;

	if (c >= 128)
		return true;

	return false;
}

static bool
replace_variables(char **text, int lineno)
{
	bool		string = false;
	int			counter = 1,
				ptr = 0;

	for (; (*text)[ptr] != '\0'; ptr++)
	{
		if ((*text)[ptr] == '\'')
			string = string ? false : true;

		if (string || (((*text)[ptr] != ':') && ((*text)[ptr] != '?')))
			continue;

		if (((*text)[ptr] == ':') && ((*text)[ptr + 1] == ':'))
			ptr += 2;			/* skip  '::' */
		else
		{
			/* a rough guess of the size we need: */
			int			buffersize = sizeof(int) * CHAR_BIT * 10 / 3;
			int			len;
			char	   *buffer,
					   *newcopy;

			if (!(buffer = (char *) ecpg_alloc(buffersize, lineno)))
				return false;

			snprintf(buffer, buffersize, "$%d", counter++);

			for (len = 1; (*text)[ptr + len] && isvarchar((*text)[ptr + len]); len++)
				 /* skip */ ;
			if (!(newcopy = (char *) ecpg_alloc(strlen(*text) -len + strlen(buffer) + 1, lineno)))
			{
				ecpg_free(buffer);
				return false;
			}

			memcpy(newcopy, *text, ptr);
			strcpy(newcopy + ptr, buffer);
			strcat(newcopy, (*text) +ptr + len);

			ecpg_free(*text);
			ecpg_free(buffer);

			*text = newcopy;

			if ((*text)[ptr] == '\0')	/* we reached the end */
				ptr--;			/* since we will (*text)[ptr]++ in the top
								 * level for loop */
		}
	}
	return true;
}

static bool
prepare_common(int lineno, struct connection *con, const char *name, const char *variable)
{
	struct statement *stmt;
	struct prepared_statement *this;
	PGresult   *query;

	/* allocate new statement */
	this = (struct prepared_statement *) ecpg_alloc(sizeof(struct prepared_statement), lineno);
	if (!this)
		return false;

	stmt = (struct statement *) ecpg_alloc(sizeof(struct statement), lineno);
	if (!stmt)
	{
		ecpg_free(this);
		return false;
	}
	memset(stmt, 0, sizeof(struct statement));

	/* create statement */
	stmt->lineno = lineno;
	stmt->connection = con;
	stmt->command = ecpg_strdup(variable, lineno);
	stmt->inlist = stmt->outlist = NULL;

	/* if we have C variables in our statement replace them with '?' */
	replace_variables(&(stmt->command), lineno);

	/* add prepared statement to our list */
	this->name = ecpg_strdup(name, lineno);
	this->stmt = stmt;

	/* and finally really prepare the statement */
	query = PQprepare(stmt->connection->connection, name, stmt->command, 0, NULL);
	if (!ecpg_check_PQresult(query, stmt->lineno, stmt->connection->connection, stmt->compat))
	{
		ecpg_free(stmt->command);
		ecpg_free(this->name);
		ecpg_free(this);
		ecpg_free(stmt);
		return false;
	}

	ecpg_log("prepare_common on line %d: name %s; query: \"%s\"\n", stmt->lineno, name, stmt->command);
	PQclear(query);
	this->prepared = true;

	if (con->prep_stmts == NULL)
		this->next = NULL;
	else
		this->next = con->prep_stmts;

	con->prep_stmts = this;
	return true;
}

/* handle the EXEC SQL PREPARE statement */
/* questionmarks is not needed but remains in there for the time being to not change the API */
bool
ECPGprepare(int lineno, const char *connection_name, const bool questionmarks,
			const char *name, const char *variable)
{
	struct connection *con;
	struct prepared_statement *this,
			   *prev;
	const char *real_connection_name = NULL; 

	(void) questionmarks;		/* quiet the compiler */

	real_connection_name = ecpg_get_con_name_by_declared_name(name);
	if (real_connection_name == NULL)
	{
		/* 
		 * If can't get the connection name by declared name then using connection name
		 * coming from the parameter connection_name
		 */
		real_connection_name = connection_name;
	}

	con = ecpg_get_connection(real_connection_name);
	if (!ecpg_init(con, real_connection_name, lineno))
		return false;

	/* check if we already have prepared this statement */
	this = ecpg_find_prepared_statement(name, con, &prev);
	if (this && !deallocate_one(lineno, ECPG_COMPAT_PGSQL, con, prev, this))
		return false;

	return prepare_common(lineno, con, name, variable);
}

struct prepared_statement *
ecpg_find_prepared_statement(const char *name,
							 struct connection *con, struct prepared_statement **prev_)
{
	struct prepared_statement *this,
			   *prev;

	for (this = con->prep_stmts, prev = NULL;
		 this != NULL;
		 prev = this, this = this->next)
	{
		if (strcmp(this->name, name) == 0)
		{
			if (prev_)
				*prev_ = prev;
			return this;
		}
	}
	return NULL;
}

static bool
deallocate_one(int lineno, enum COMPAT_MODE c, struct connection *con,
			   struct prepared_statement *prev, struct prepared_statement *this)
{
	bool		r = false;

	ecpg_log("deallocate_one on line %d: name %s\n", lineno, this->name);

	/* first deallocate the statement in the backend */
	if (this->prepared)
	{
		char	   *text;
		PGresult   *query;

		text = (char *) ecpg_alloc(strlen("deallocate \"\" ") + strlen(this->name), this->stmt->lineno);

		if (text)
		{
			sprintf(text, "deallocate \"%s\"", this->name);
			query = PQexec(this->stmt->connection->connection, text);
			ecpg_free(text);
			if (ecpg_check_PQresult(query, lineno,
									this->stmt->connection->connection,
									this->stmt->compat))
			{
				PQclear(query);
				r = true;
			}
		}
	}

	/*
	 * Just ignore all errors since we do not know the list of cursors we are
	 * allowed to free. We have to trust the software.
	 */
	if (!r && !INFORMIX_MODE(c))
	{
		ecpg_raise(lineno, ECPG_INVALID_STMT, ECPG_SQLSTATE_INVALID_SQL_STATEMENT_NAME, this->name);
		return false;
	}

	/* okay, free all the resources */
	ecpg_free(this->stmt->command);
	ecpg_free(this->stmt);
	ecpg_free(this->name);
	if (prev != NULL)
		prev->next = this->next;
	else
		con->prep_stmts = this->next;

	ecpg_free(this);
	return true;
}

/* handle the EXEC SQL DEALLOCATE PREPARE statement */
bool
ECPGdeallocate(int lineno, int c, const char *connection_name, const char *name)
{
	struct connection *con;
	struct prepared_statement *this,
			   *prev;
	const char *real_connection_name = NULL;

	real_connection_name = ecpg_get_con_name_by_declared_name(name);
	if (real_connection_name == NULL)
	{
		/*
		 * If can't get the connection name by declared name then using connection name
		 * coming from the parameter connection_name
		 */
		real_connection_name = connection_name;
	}

	con = ecpg_get_connection(real_connection_name);
	if (!ecpg_init(con, connection_name, lineno))
		return false;

	this = ecpg_find_prepared_statement(name, con, &prev);
	if (this)
		return deallocate_one(lineno, c, con, prev, this);

	/* prepared statement is not found */
	if (INFORMIX_MODE(c))
		return true;
	ecpg_raise(lineno, ECPG_INVALID_STMT, ECPG_SQLSTATE_INVALID_SQL_STATEMENT_NAME, name);
	return false;
}

bool
ecpg_deallocate_all_conn(int lineno, enum COMPAT_MODE c, struct connection *con)
{
	/* deallocate all prepared statements */
	while (con->prep_stmts)
	{
		if (!deallocate_one(lineno, c, con, NULL, con->prep_stmts))
			return false;
	}

	return true;
}

bool
ECPGdeallocate_all(int lineno, int compat, const char *connection_name)
{
	return ecpg_deallocate_all_conn(lineno, compat,
									ecpg_get_connection(connection_name));
}

char *
ecpg_prepared(const char *name, struct connection *con)
{
	struct prepared_statement *this;

	this = ecpg_find_prepared_statement(name, con, NULL);
	return this ? this->stmt->command : NULL;
}

/* return the prepared statement */
/* lineno is not used here, but kept in to not break API */
char *
ECPGprepared_statement(const char *connection_name, const char *name, int lineno)
{
	const char *real_connection_name = NULL;

	(void) lineno;				/* keep the compiler quiet */

	real_connection_name = ecpg_get_con_name_by_declared_name(name);
	if (real_connection_name == NULL)
	{
		/*
		 * If can't get the connection name by declared name then using connection name
		 * coming from the parameter connection_name
		 */
		real_connection_name = connection_name;
	}

	return ecpg_prepared(name, ecpg_get_connection(real_connection_name));
}

/*
 * hash a SQL statement -  returns entry # of first entry in the bucket
 */
static int
HashStmt(const char *ecpgQuery)
{
	int			stmtIx,
				bucketNo,
				hashLeng,
				stmtLeng;
	uint64		hashVal,
				rotVal;

	stmtLeng = strlen(ecpgQuery);
	hashLeng = 50;				/* use 1st 50 characters of statement */
	if (hashLeng > stmtLeng)	/* if the statement isn't that long */
		hashLeng = stmtLeng;	/* use its actual length */

	hashVal = 0;
	for (stmtIx = 0; stmtIx < hashLeng; ++stmtIx)
	{
		hashVal = hashVal + (unsigned char) ecpgQuery[stmtIx];
		/* rotate 32-bit hash value left 13 bits */
		hashVal = hashVal << 13;
		rotVal = (hashVal & UINT64CONST(0x1fff00000000)) >> 32;
		hashVal = (hashVal & UINT64CONST(0xffffffff)) | rotVal;
	}

	bucketNo = hashVal % stmtCacheNBuckets;

	/* Add 1 so that array entry 0 is never used */
	return bucketNo * stmtCacheEntPerBucket + 1;
}

/*
 * search the statement cache - search for entry with matching ECPG-format query
 * Returns entry # in cache if found
 *	 OR  zero if not present (zero'th entry isn't used)
 */
static int
SearchStmtCache(const char *ecpgQuery)
{
	int			entNo,
				entIx;

	/* quick failure if cache not set up */
	if (stmtCacheEntries == NULL)
		return 0;

	/* hash the statement */
	entNo = HashStmt(ecpgQuery);

	/* search the cache */
	for (entIx = 0; entIx < stmtCacheEntPerBucket; ++entIx)
	{
		if (stmtCacheEntries[entNo].stmtID[0])	/* check if entry is in use */
		{
			if (strcmp(ecpgQuery, stmtCacheEntries[entNo].ecpgQuery) == 0)
				break;			/* found it */
		}
		++entNo;				/* incr entry # */
	}

	/* if entry wasn't found - set entry # to zero */
	if (entIx >= stmtCacheEntPerBucket)
		entNo = 0;

	return entNo;
}

/*
 * free an entry in the statement cache
 * Returns entry # in cache used
 *	 OR  negative error code
 */
static int
ecpg_freeStmtCacheEntry(int lineno, int compat,
						int entNo)	/* entry # to free */
{
	stmtCacheEntry *entry;
	struct connection *con;
	struct prepared_statement *this,
			   *prev;

	/* fail if cache isn't set up */
	if (stmtCacheEntries == NULL)
		return -1;

	entry = &stmtCacheEntries[entNo];
	if (!entry->stmtID[0])		/* return if the entry isn't in use */
		return 0;

	con = ecpg_get_connection(entry->connection);

	/* free the 'prepared_statement' list entry */
	this = ecpg_find_prepared_statement(entry->stmtID, con, &prev);
	if (this && !deallocate_one(lineno, compat, con, prev, this))
		return -1;

	entry->stmtID[0] = '\0';

	/* free the memory used by the cache entry */
	if (entry->ecpgQuery)
	{
		ecpg_free(entry->ecpgQuery);
		entry->ecpgQuery = 0;
	}

	return entNo;
}

/*
 * add an entry to the statement cache
 * returns entry # in cache used  OR  negative error code
 */
static int
AddStmtToCache(int lineno,		/* line # of statement */
			   const char *stmtID,	/* statement ID */
			   const char *connection,	/* connection */
			   int compat,		/* compatibility level */
			   const char *ecpgQuery)	/* query */
{
	int			ix,
				initEntNo,
				luEntNo,
				entNo;
	stmtCacheEntry *entry;

	/* allocate and zero cache array if we haven't already */
	if (stmtCacheEntries == NULL)
	{
		stmtCacheEntries = (stmtCacheEntry *)
			ecpg_alloc(sizeof(stmtCacheEntry) * stmtCacheArraySize, lineno);
		if (stmtCacheEntries == NULL)
			return -1;
	}

	/* hash the statement */
	initEntNo = HashStmt(ecpgQuery);

	/* search for an unused entry */
	entNo = initEntNo;			/* start with the initial entry # for the
								 * bucket */
	luEntNo = initEntNo;		/* use it as the initial 'least used' entry */
	for (ix = 0; ix < stmtCacheEntPerBucket; ++ix)
	{
		entry = &stmtCacheEntries[entNo];
		if (!entry->stmtID[0])	/* unused entry  -	use it */
			break;
		if (entry->execs < stmtCacheEntries[luEntNo].execs)
			luEntNo = entNo;	/* save new 'least used' entry */
		++entNo;				/* increment entry # */
	}

	/*
	 * if no unused entries were found, re-use the 'least used' entry found in
	 * the bucket
	 */
	if (ix >= stmtCacheEntPerBucket)
		entNo = luEntNo;

	/* 'entNo' is the entry to use - make sure its free */
	if (ecpg_freeStmtCacheEntry(lineno, compat, entNo) < 0)
		return -1;

	/* add the query to the entry */
	entry = &stmtCacheEntries[entNo];
	entry->lineno = lineno;
	entry->ecpgQuery = ecpg_strdup(ecpgQuery, lineno);
	entry->connection = connection;
	entry->execs = 0;
	memcpy(entry->stmtID, stmtID, sizeof(entry->stmtID));

	return entNo;
}

/* handle cache and preparation of statements in auto-prepare mode */
bool
ecpg_auto_prepare(int lineno, const char *connection_name, const int compat, char **name, const char *query)
{
	int			entNo;

	/* search the statement cache for this statement */
	entNo = SearchStmtCache(query);

	/* if not found - add the statement to the cache */
	if (entNo)
	{
		char	   *stmtID;
		struct connection *con;
		struct prepared_statement *prep;

		ecpg_log("ecpg_auto_prepare on line %d: statement found in cache; entry %d\n", lineno, entNo);

		stmtID = stmtCacheEntries[entNo].stmtID;

		con = ecpg_get_connection(connection_name);
		prep = ecpg_find_prepared_statement(stmtID, con, NULL);
		/* This prepared name doesn't exist on this connection. */
		if (!prep && !prepare_common(lineno, con, stmtID, query))
			return false;

		*name = ecpg_strdup(stmtID, lineno);
	}
	else
	{
		char		stmtID[STMTID_SIZE];

		ecpg_log("ecpg_auto_prepare on line %d: statement not in cache; inserting\n", lineno);

		/* generate a statement ID */
		sprintf(stmtID, "ecpg%d", nextStmtID++);

		if (!ECPGprepare(lineno, connection_name, 0, stmtID, query))
			return false;

		entNo = AddStmtToCache(lineno, stmtID, connection_name, compat, query);
		if (entNo < 0)
			return false;

		*name = ecpg_strdup(stmtID, lineno);
	}

	/* increase usage counter */
	stmtCacheEntries[entNo].execs++;

	return true;
}

/*
 * handle the EXEC SQL DECLARE STATEMENT
 * Input: connection_name -- connection name
 *		  name -- declared name
 */
bool
ECPGdeclare(int lineno, const char *connection_name, const char *name)
{
	struct connection *con = NULL;
	struct declared_statement *p = NULL;

	if (name == NULL)
	{
		/* Should never go to here because ECPG pre-compiler will check it */
		return false;
	}

	if (connection_name == NULL)
	{
		/*
		 * Going to here means not using AT clause in the DECLARE STATEMENT
		 * ECPG pre-processor allows this case.
		 * However, we don't allocate a node to store the declared name
		 * because the DECLARE STATEMENT without using AT clause will be ignored.
		 * The following statement such as PREPARE, EXECUTE are executed
		 * as usual on the current connection.
		 */
		return true;
	}

	con = ecpg_get_connection(connection_name);
	if (!ecpg_init(con, connection_name, lineno))
		return false;

	if (ecpg_find_declared_statement(name))
	{
		/* Should not go to here because the pre-compiler has check the duplicate name */
		return false;
	}

	/* allocate a declared_statement as a new node */
	p = (struct declared_statement *) ecpg_alloc(sizeof(struct declared_statement), lineno);
	if (!p)
		return false;

	memset(p, 0, sizeof(struct declared_statement));

	ecpg_log("ECPGdeclare on line %d: declared name %s on connection: \"%s\"\n", lineno, name, connection_name);

	p->name = ecpg_strdup(name, lineno);
	p->connection_name = ecpg_strdup(connection_name, lineno);

	/* Add the new node into the g_declared_list */
	if (g_declared_list != NULL)
	{
		p->next = g_declared_list;
		g_declared_list = p;
	}
	else
		g_declared_list = p;

	return true;
}

/*
 * Find a declared node by declared name
 * Input: name -- declared name
 * Return: Found -- The pointer points to the declared node
 *		   Not found -- NULL
 */
static struct declared_statement *
ecpg_find_declared_statement(const char *name)
{
	struct declared_statement *p;

	if (name == NULL)
		return NULL;

	p = g_declared_list;
	while (p)
	{
		if (strcmp(p->name, name) == 0)
			return p;
		p = p->next;
	}

	return NULL;
}

/*
 * Build the relationship between the declared name and cursor name
 * Input: declared_name -- the name declared in the DECLARE STATEMENT
 *		  cursor_name -- cursor name declared in the DECLARE/OPEN CURSOR statement
 */
void
ecpg_update_declare_statement(const char *declared_name, const char *cursor_name, const int lineno)
{
	struct declared_statement *p = NULL;

	if (!declared_name || !cursor_name)
		return;

	/* Find the declared node by declared name */
	p = ecpg_find_declared_statement(declared_name);
	if (p)
		p->cursor_name = ecpg_strdup(cursor_name,lineno);
}

/*
 * Find and return the connection name referred by the declared name
 * Input: declared_name -- the name declared in the DECLARE STATEMENT
 * Return: Found -- The connection name
 *		   Not found -- NULL
 */
char *
ecpg_get_con_name_by_declared_name(const char *declared_name)
{
	struct declared_statement *p;

	p = ecpg_find_declared_statement(declared_name);
	if (p)
		return p->connection_name;

	return NULL;
}

/*
 * Find the connection name by referring the declared statements
 * cursors by using the provided cursor name
 * Input: cursor_name -- the cursor name
 * Return: Found -- The connection name
 *		   Not found -- NULL
 */
const char *
ecpg_get_con_name_by_cursor_name(const char *cursor_name)
{
	struct declared_statement *p;

	if (cursor_name == NULL)
		return NULL;

	p = g_declared_list;
	while (p)
	{
		/* Search the cursor name in the declared list */
		if (p->cursor_name && (strcmp(p->cursor_name, cursor_name) == 0))
			return p->connection_name;

		p = p->next;
	}

	return NULL;
}

/*
 * Release the declare node from the g_declared_list which refers the connection_name
 * Input: connection_name -- connection name
 */
void
ecpg_release_declared_statement(const char *connection_name)
{
	struct declared_statement *cur = NULL;
	struct declared_statement *prev = NULL;

	if (connection_name == NULL)
		return;

	cur = g_declared_list;
	while (cur)
	{
		if (strcmp(cur->connection_name, connection_name) == 0)
		{
			/* If find then release the declared node from the list */
			if (prev)
				prev->next = cur->next;
			else
				g_declared_list = cur->next;

			ecpg_log("ecpg_release_declared_statement: declared name %s is released\n", cur->name);

			ecpg_free(cur->name);
			ecpg_free(cur->connection_name);
			ecpg_free(cur->cursor_name);
			ecpg_free(cur);

			/* One connection can be used by multiple declared name, so no break here */
		}
		else
			prev = cur;

		if (prev)
			cur = prev->next;
		else
			cur = g_declared_list;
	}
}
