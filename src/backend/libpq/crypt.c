/*-------------------------------------------------------------------------
 *
 * crypt.c
 *	  Look into the password file and check the encrypted password with
 *	  the one passed in from the frontend.
 *
 * Original coding by Todd A. Brandys
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/libpq/crypt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#include "catalog/pg_authid.h"
#include "common/md5.h"
#include "libpq/crypt.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"


/*
 * Check given password for given user, and return STATUS_OK or STATUS_ERROR.
 *
 * 'client_pass' is the password response given by the remote user.  If
 * 'md5_salt' is not NULL, it is a response to an MD5 authentication
 * challenge, with the given salt.  Otherwise, it is a plaintext password.
 *
 * In the error case, optionally store a palloc'd string at *logdetail
 * that will be sent to the postmaster log (but not the client).
 */
int
md5_crypt_verify(const char *role, char *client_pass,
				 char *md5_salt, int md5_salt_len, char **logdetail)
{
	int			retval = STATUS_ERROR;
	char	   *shadow_pass,
			   *crypt_pwd;
	TimestampTz vuntil = 0;
	char	   *crypt_client_pass = client_pass;
	HeapTuple	roleTup;
	Datum		datum;
	bool		isnull;

	/* Get role info from pg_authid */
	roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(role));
	if (!HeapTupleIsValid(roleTup))
	{
		*logdetail = psprintf(_("Role \"%s\" does not exist."),
							  role);
		return STATUS_ERROR;	/* no such user */
	}

	datum = SysCacheGetAttr(AUTHNAME, roleTup,
							Anum_pg_authid_rolpassword, &isnull);
	if (isnull)
	{
		ReleaseSysCache(roleTup);
		*logdetail = psprintf(_("User \"%s\" has no password assigned."),
							  role);
		return STATUS_ERROR;	/* user has no password */
	}
	shadow_pass = TextDatumGetCString(datum);

	datum = SysCacheGetAttr(AUTHNAME, roleTup,
							Anum_pg_authid_rolvaliduntil, &isnull);
	if (!isnull)
		vuntil = DatumGetTimestampTz(datum);

	ReleaseSysCache(roleTup);

	if (*shadow_pass == '\0')
	{
		*logdetail = psprintf(_("User \"%s\" has an empty password."),
							  role);
		return STATUS_ERROR;	/* empty password */
	}

	/*
	 * Compare with the encrypted or plain password depending on the
	 * authentication method being used for this connection.  (We do not
	 * bother setting logdetail for pg_md5_encrypt failure: the only possible
	 * error is out-of-memory, which is unlikely, and if it did happen adding
	 * a psprintf call would only make things worse.)
	 */
	if (md5_salt)
	{
		/* MD5 authentication */
		Assert(md5_salt_len > 0);
		crypt_pwd = palloc(MD5_PASSWD_LEN + 1);
		if (isMD5(shadow_pass))
		{
			/* stored password already encrypted, only do salt */
			if (!pg_md5_encrypt(shadow_pass + strlen("md5"),
								md5_salt, md5_salt_len,
								crypt_pwd))
			{
				pfree(crypt_pwd);
				return STATUS_ERROR;
			}
		}
		else
		{
			/* stored password is plain, double-encrypt */
			char	   *crypt_pwd2 = palloc(MD5_PASSWD_LEN + 1);

			if (!pg_md5_encrypt(shadow_pass,
								role,
								strlen(role),
								crypt_pwd2))
			{
				pfree(crypt_pwd);
				pfree(crypt_pwd2);
				return STATUS_ERROR;
			}
			if (!pg_md5_encrypt(crypt_pwd2 + strlen("md5"),
								md5_salt, md5_salt_len,
								crypt_pwd))
			{
				pfree(crypt_pwd);
				pfree(crypt_pwd2);
				return STATUS_ERROR;
			}
			pfree(crypt_pwd2);
		}
	}
	else
	{
		/* Client sent password in plaintext */
		if (isMD5(shadow_pass))
		{
			/* Encrypt user-supplied password to match stored MD5 */
			crypt_client_pass = palloc(MD5_PASSWD_LEN + 1);
			if (!pg_md5_encrypt(client_pass,
								role,
								strlen(role),
								crypt_client_pass))
			{
				pfree(crypt_client_pass);
				return STATUS_ERROR;
			}
		}
		crypt_pwd = shadow_pass;
	}

	if (strcmp(crypt_client_pass, crypt_pwd) == 0)
	{
		/*
		 * Password OK, now check to be sure we are not past rolvaliduntil
		 */
		if (isnull)
			retval = STATUS_OK;
		else if (vuntil < GetCurrentTimestamp())
		{
			*logdetail = psprintf(_("User \"%s\" has an expired password."),
								  role);
			retval = STATUS_ERROR;
		}
		else
			retval = STATUS_OK;
	}
	else
		*logdetail = psprintf(_("Password does not match for user \"%s\"."),
							  role);

	if (crypt_pwd != shadow_pass)
		pfree(crypt_pwd);
	if (crypt_client_pass != client_pass)
		pfree(crypt_client_pass);

	return retval;
}
