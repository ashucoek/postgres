/*-------------------------------------------------------------------------
 *
 * percona.h
 *      Percona specific functions
 *
 * IDENTIFICATION
 *    src/include/utils/percona.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PERCONA__H__
#define PERCONA__H__

extern const PGDLLIMPORT int percona_api_version;

static inline void
check_percona_api_version(void)
{
	if (PERCONA_API_VERSION != percona_api_version)
	{
		elog(FATAL, "Percona API version mismatch, the extension was built against a different PostgreSQL version!");
	}
}

#endif
