/* frontend.c - Database fronend code for keyboxd
 * Copyright (C) 2019 g10 Code GmbH
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 * SPDX-License-Identifier: GPL-3.0+
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "keyboxd.h"
#include <assuan.h>
#include "../common/i18n.h"
#include "../common/userids.h"
#include "backend.h"
#include "frontend.h"


/* An object to keep infos about the database.  */
struct
{
  enum database_types db_type;
  backend_handle_t backend_handle;
} the_database;



/* Take a lock for reading the databases.  */
static void
take_read_lock (ctrl_t ctrl)
{
  /* FIXME */
  (void)ctrl;
}


/* Take a lock for reading and writing the databases.  */
static void
take_read_write_lock (ctrl_t ctrl)
{
  /* FIXME */
  (void)ctrl;
}


/* Release a lock.  It is valid to call this even if no lock has been
 * taken in which case this is a nop.  */
static void
release_lock (ctrl_t ctrl)
{
  /* FIXME */
  (void)ctrl;
}


/* Set the database to use.  Depending on the FILENAME suffix we
 * decide which one to use.  This function must be called at daemon
 * startup because it employs no locking.  If FILENAME has no
 * directory separator, the file is expected or created below
 * "$GNUPGHOME/public-keys.d/".  In READONLY mode the file must exists;
 * otherwise it is created.  */
gpg_error_t
kbxd_set_database (ctrl_t ctrl, const char *filename_arg, int readonly)
{
  gpg_error_t err;
  char *filename;
  enum database_types db_type = 0;
  backend_handle_t handle = NULL;
  unsigned int n;

  /* Do tilde expansion etc. */
  if (strchr (filename_arg, DIRSEP_C)
#ifdef HAVE_W32_SYSTEM
      || strchr (filename_arg, '/')  /* Windows also accepts a slash.  */
#endif
      )
    filename = make_filename (filename_arg, NULL);
  else
    filename = make_filename (gnupg_homedir (), GNUPG_PUBLIC_KEYS_DIR,
                              filename_arg, NULL);

  /* If this is the first call to the function and the request is not
   * for the cache backend, add the cache backend so that it will
   * always be the first to be queried.  */
  if (the_database.db_type)
    {
      log_error ("error: only one database allowed\n");
      err = gpg_error (GPG_ERR_CONFLICT);
      goto leave;
    }

  /* Init the cache.  */
  err = be_cache_initialize ();
  if (err)
    goto leave;

  n = strlen (filename);
  if (db_type)
    ; /* We already know it.  */
  else if (n > 4 && !strcmp (filename + n - 4, ".kbx"))
    db_type = DB_TYPE_KBX;
  else
    {
      log_error (_("can't use file '%s': %s\n"), filename, _("unknown suffix"));
      err = gpg_error (GPG_ERR_NOT_SUPPORTED);
      goto leave;
    }

  err = gpg_error (GPG_ERR_BUG);
  switch (db_type)
    {
    case DB_TYPE_NONE: /* NOTREACHED */
      break;

    case DB_TYPE_CACHE:
      err = be_cache_add_resource (ctrl, &handle);
      break;

    case DB_TYPE_KBX:
      err = be_kbx_add_resource (ctrl, &handle, filename, readonly);
      break;
    }
  if (err)
    goto leave;

  the_database.db_type = db_type;
  the_database.backend_handle = handle;
  handle = NULL;

 leave:
  if (err)
    {
      log_error ("error setting database '%s': %s\n",
                 filename, gpg_strerror (err));
      be_generic_release_backend (ctrl, handle);
    }
  xfree (filename);
  return err;
}


/* Release all per session objects.  */
void
kbxd_release_session_info (ctrl_t ctrl)
{
  if (!ctrl)
    return;
  be_release_request (ctrl->opgp_req);
  ctrl->opgp_req = NULL;
  be_release_request (ctrl->x509_req);
  ctrl->x509_req = NULL;
}



/* Search for the keys described by (DESC,NDESC) and return them to
 * the caller.  If RESET is set, the search state is first reset. */
gpg_error_t
kbxd_search (ctrl_t ctrl, KEYDB_SEARCH_DESC *desc, unsigned int ndesc,
             int reset)
{
  gpg_error_t err;
  int i;
  db_request_t request;
  int start_at_ubid = 0;

  if (DBG_CLOCK)
    log_clock ("%s: enter", __func__);

  if (DBG_LOOKUP)
    {
      log_debug ("%s: %u search descriptions:\n", __func__, ndesc);
      for (i = 0; i < ndesc; i ++)
        {
          /* char *t = keydb_search_desc_dump (&desc[i]); */
          /* log_debug ("%s   %d: %s\n", __func__, i, t); */
          /* xfree (t); */
        }
    }

  take_read_lock (ctrl);

  /* Allocate a handle object if none exists for this context.  */
  if (!ctrl->opgp_req)
    {
      ctrl->opgp_req = xtrycalloc (1, sizeof *ctrl->opgp_req);
      if (!ctrl->opgp_req)
        {
          err = gpg_error_from_syserror ();
          goto leave;
        }
    }
  request = ctrl->opgp_req;

  if (!the_database.db_type)
    {
      log_error ("%s: error: no database configured\n", __func__);
      err = gpg_error (GPG_ERR_NOT_INITIALIZED);
      goto leave;
    }

  /* If requested do a reset.  Using the reset flag is faster than
   * letting the caller do a separate call for an intial reset.  */
  if (!desc || reset)
    {
      switch (the_database.db_type)
        {
        case DB_TYPE_CACHE:
          err = 0; /* Nothing to do.  */
          break;

        case DB_TYPE_KBX:
          err = be_kbx_search (ctrl, the_database.backend_handle,
                               request, NULL, 0);
          break;

        default:
          err = gpg_error (GPG_ERR_INTERNAL);
          break;
        }
      if (err)
        {
          log_error ("error during the %ssearch reset: %s\n",
                     reset? "initial ":"", gpg_strerror (err));
          goto leave;
        }
      request->any_search = 0;
      request->any_found = 0;
      request->next_dbidx = 0;
      if (!desc) /* Reset only mode */
        {
          err = 0;
          goto leave;
        }
    }

  /* Divert to the backend for the actual search.  */
  switch (the_database.db_type)
    {
    case DB_TYPE_CACHE:
      err = be_cache_search (ctrl, the_database.backend_handle, request,
                             desc, ndesc);
      /* Expected error codes from the cache lookup are:
       *  0 - found and returned via the cache
       *  GPG_ERR_NOT_FOUND - marked in the cache as not available
       *  GPG_ERR_EOF - cache miss. */
      break;

    case DB_TYPE_KBX:
      if (start_at_ubid)
        {
          /* We need to set the startpoint for the search.  */
          err = be_kbx_seek (ctrl, the_database.backend_handle, request,
                             request->last_cached_ubid);
          if (err)
            {
              log_debug ("%s: seeking %s to an UBID failed: %s\n", __func__,
                         strdbtype (the_database.db_type), gpg_strerror (err));
              break;
            }
        }
      err = be_kbx_search (ctrl, the_database.backend_handle, request,
                           desc, ndesc);
      if (start_at_ubid && gpg_err_code (err) == GPG_ERR_EOF)
        be_cache_mark_final (ctrl, request);
      break;

    default:
      log_error ("%s: unsupported database type %d\n",
                 __func__, the_database.db_type);
      err = gpg_error (GPG_ERR_INTERNAL);
      break;
    }

  if (DBG_LOOKUP)
    log_debug ("%s: searched %s => %s\n", __func__,
               strdbtype (the_database.db_type), gpg_strerror (err));
  request->any_search = 1;
  start_at_ubid = 0;
  if (!err)
    {
      request->any_found = 1;
    }
  else if (gpg_err_code (err) == GPG_ERR_EOF)
    {
      if (the_database.db_type == DB_TYPE_CACHE && request->last_cached_valid)
        {
          if (request->last_cached_final)
            goto leave;
          start_at_ubid = 1;
        }
      request->next_dbidx++;
      /* FIXME: We need to see which pubkey type we need to insert.  */
      be_cache_not_found (ctrl, PUBKEY_TYPE_UNKNOWN, desc, ndesc);
      err = gpg_error (GPG_ERR_NOT_FOUND);
      goto leave;
    }


 leave:
  release_lock (ctrl);
  if (DBG_CLOCK)
    log_clock ("%s: leave (%s)", __func__, err? "not found" : "found");
  return err;
}



/* Store; that is insert or update the key (BLOB,BLOBLEN).  MODE
 * controls whether only updates or only inserts are allowed.  */
gpg_error_t
kbxd_store (ctrl_t ctrl, const void *blob, size_t bloblen,
            enum kbxd_store_modes mode)
{
  gpg_error_t err;
  db_request_t request;
  char ubid[UBID_LEN];
  enum pubkey_types pktype;
  int insert = 0;

  if (DBG_CLOCK)
    log_clock ("%s: enter", __func__);

  take_read_write_lock (ctrl);

  /* Allocate a handle object if none exists for this context.  */
  if (!ctrl->opgp_req)
    {
      ctrl->opgp_req = xtrycalloc (1, sizeof *ctrl->opgp_req);
      if (!ctrl->opgp_req)
        {
          err = gpg_error_from_syserror ();
          goto leave;
        }
    }
  request = ctrl->opgp_req;

  if (!the_database.db_type)
    {
      log_error ("%s: error: no database configured\n", __func__);
      err = gpg_error (GPG_ERR_NOT_INITIALIZED);
      goto leave;
    }

  /* Check whether to insert or update.  */
  err = be_ubid_from_blob (blob, bloblen, &pktype, ubid);
  if (err)
    goto leave;

  if (the_database.db_type == DB_TYPE_KBX)
    {
      err = be_kbx_seek (ctrl, the_database.backend_handle, request, ubid);
    }
  else
    {
      log_error ("%s: unsupported database type %d\n",
                 __func__, the_database.db_type);
      err = gpg_error (GPG_ERR_INTERNAL);
    }

  if (!err)
    ; /* Found - need to update.  */
  else if (gpg_err_code (err) == GPG_ERR_EOF)
    insert = 1; /* Not found - need to insert.  */
  else
    {
      log_debug ("%s: searching fingerprint failed: %s\n",
                 __func__, gpg_strerror (err));
      goto leave;
    }

  if (insert)
    {
      if (mode == KBXD_STORE_UPDATE)
        err = gpg_error (GPG_ERR_CONFLICT);
      else if (the_database.db_type == DB_TYPE_KBX)
        err = be_kbx_insert (ctrl, the_database.backend_handle, request,
                             pktype, blob, bloblen);
      else
        err = gpg_error (GPG_ERR_INTERNAL);
    }
  else /* Update.  */
    {
      if (mode == KBXD_STORE_INSERT)
        err = gpg_error (GPG_ERR_CONFLICT);
      else if (the_database.db_type == DB_TYPE_KBX)
        err = be_kbx_update (ctrl, the_database.backend_handle, request,
                             pktype, blob, bloblen);
      else
        err = gpg_error (GPG_ERR_INTERNAL);
    }

 leave:
  release_lock (ctrl);
  if (DBG_CLOCK)
    log_clock ("%s: leave", __func__);
  return err;
}




/* Delete; remove the blob identified by UBID.  */
gpg_error_t
kbxd_delete (ctrl_t ctrl, const unsigned char *ubid)
{
  gpg_error_t err;
  db_request_t request;

  if (DBG_CLOCK)
    log_clock ("%s: enter", __func__);

  take_read_write_lock (ctrl);

  /* Allocate a handle object if none exists for this context.  */
  if (!ctrl->opgp_req)
    {
      ctrl->opgp_req = xtrycalloc (1, sizeof *ctrl->opgp_req);
      if (!ctrl->opgp_req)
        {
          err = gpg_error_from_syserror ();
          goto leave;
        }
    }
  request = ctrl->opgp_req;

  if (!the_database.db_type)
    {
      log_error ("%s: error: no database configured\n", __func__);
      err = gpg_error (GPG_ERR_NOT_INITIALIZED);
      goto leave;
    }

  if (the_database.db_type == DB_TYPE_KBX)
    {
      err = be_kbx_seek (ctrl, the_database.backend_handle, request, ubid);
    }
  else
    {
      log_error ("%s: unsupported database type %d\n",
                 __func__, the_database.db_type);
      err = gpg_error (GPG_ERR_INTERNAL);
    }

  if (!err)
    ; /* Found - we can delete.  */
  else if (gpg_err_code (err) == GPG_ERR_EOF)
    {
      err = gpg_error (GPG_ERR_NOT_FOUND);
      goto leave;
    }
  else
    {
      log_debug ("%s: searching primary fingerprint failed: %s\n",
                 __func__, gpg_strerror (err));
      goto leave;
    }

  if (the_database.db_type == DB_TYPE_KBX)
    err = be_kbx_delete (ctrl, the_database.backend_handle, request);
  else
    err = gpg_error (GPG_ERR_INTERNAL);

 leave:
  release_lock (ctrl);
  if (DBG_CLOCK)
    log_clock ("%s: leave", __func__);
  return err;
}
