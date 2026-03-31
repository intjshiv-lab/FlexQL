/*
 * flexql.h — public C API
 *
 * This is the only header clients need. The handle is opaque so
 * nothing from the internals leaks out.
 *
 *   flexql_open()   — connect to server
 *   flexql_close()  — disconnect
 *   flexql_exec()   — run a SQL statement
 *   flexql_free()   — free API-allocated memory
 *
 * Author: Ramesh Choudhary
 */

#ifndef FLEXQL_H
#define FLEXQL_H

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Error Codes ──────────────────────────────────────────────────────── */
#define FLEXQL_OK       0   /* Operation succeeded                        */
#define FLEXQL_ERROR    1   /* Generic error                              */
#define FLEXQL_BUSY     2   /* Server is busy, try again                  */
#define FLEXQL_MISUSE   3   /* API misuse (e.g., exec on closed handle)   */
#define FLEXQL_NOMEM    4   /* Out of memory                              */
#define FLEXQL_NOTFOUND 5   /* Table or column not found                  */

/* ─── Opaque Database Handle ───────────────────────────────────────────── */
typedef struct FlexQL FlexQL;

/* ─── Callback Type ────────────────────────────────────────────────────── */
/*
 * Called once per result row.
 *
 *   data        — User-provided pointer (pass-through from flexql_exec arg)
 *   columnCount — Number of columns in this row
 *   values      — Array of column value strings  (e.g., ["1", "Alice"])
 *   columnNames — Array of column name strings    (e.g., ["id", "name"])
 *
 * Return 0 to continue processing, 1 to abort the query.
 */
typedef int (*flexql_callback)(
    void  *data,
    int    columnCount,
    char **values,
    char **columnNames
);

/* ─── API Functions ────────────────────────────────────────────────────── */

/*
 * flexql_open — Establish a connection to the FlexQL server.
 *
 *   host — Hostname or IP (e.g., "127.0.0.1", "localhost")
 *   port — Port number the server is listening on
 *   db   — [out] Pointer to a handle pointer; set on success
 *
 * Returns FLEXQL_OK on success, FLEXQL_ERROR on failure.
 */
int flexql_open(const char *host, int port, FlexQL **db);

/*
 * flexql_close — Close the connection and free all resources.
 *
 *   db — A previously opened database handle
 *
 * Returns FLEXQL_OK on success, FLEXQL_ERROR if handle is invalid.
 */
int flexql_close(FlexQL *db);

/*
 * flexql_exec — Execute a SQL statement.
 *
 *   db       — Open database connection
 *   sql      — SQL string to execute (e.g., "SELECT * FROM users;")
 *   callback — Function called for each result row; NULL if no output needed
 *   arg      — User pointer forwarded to callback's `data` parameter
 *   errmsg   — [out] Error message on failure; caller must free via flexql_free
 *
 * Returns FLEXQL_OK on success, FLEXQL_ERROR on failure.
 */
int flexql_exec(
    FlexQL          *db,
    const char      *sql,
    flexql_callback  callback,
    void            *arg,
    char           **errmsg
);

/*
 * flexql_free — Free memory allocated by the FlexQL API.
 *
 *   ptr — Pointer returned by the API (e.g., errmsg from flexql_exec)
 */
void flexql_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* FLEXQL_H */
