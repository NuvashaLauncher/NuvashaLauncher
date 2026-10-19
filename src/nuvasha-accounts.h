/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaAccounts: the accounts a user can play as, and which one is active.
 *
 * There are two kinds of account and neither of them is a password:
 *
 *   Offline     a name and nothing else. The game is told that name and
 *               the UUID java hands out for it ("OfflinePlayer:<name>" as
 *               a name based UUID), which is what the game does when it is
 *               run without an account. Anyone can make one, and it is
 *               labelled as offline everywhere it is shown.
 *
 *   Microsoft   a real Microsoft account, signed in through the device
 *               code flow and traded for a Minecraft profile by
 *               nuvasha-auth-microsoft.h. Stored data includes the profile (name
 *               and UUID), the access token with the moment it stops
 *               being good and the refresh token that trades for a new
 *               one. No password is ever asked for or kept.
 *
 * Exactly one account is active, and it is the one the launcher runs the
 * game as. Tokens are never logged, never printed in an error and never
 * put anywhere but this file, which is written for nobody but the user:
 * mode 0600, written whole or not at all.
 *
 * The file is <user data dir>/nuvasha/accounts.json. A file that is missing,
 * unreadable or nonsense leaves the store empty rather than failing, the
 * same as the settings file. */

#ifndef nuvasha_ACCOUNTS_H
#define nuvasha_ACCOUNTS_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define nuvasha_ACCOUNTS_ERROR (nuvasha_accounts_error_quark ())

typedef enum {
  nuvasha_ACCOUNTS_ERROR_IO,      /* the file cannot be read or written */
  nuvasha_ACCOUNTS_ERROR_INVALID, /* invalid account data */
  nuvasha_ACCOUNTS_ERROR_UNKNOWN   /* there is no such account */
} NuvashaAccountsError;

GQuark nuvasha_accounts_error_quark (void);

/* How far ahead of its expiry a token stops being good: the game would
 * be handed a token that dies while it is still booting. */
#define nuvasha_ACCOUNTS_TOKEN_MARGIN 60

typedef enum {
  nuvasha_ACCOUNT_OFFLINE,   /* a name and nothing else */
  nuvasha_ACCOUNT_MICROSOFT  /* signed in with a Microsoft account */
} NuvashaAccountType;

/* One account. @id is the store key: the UUID of the
 * Minecraft profile for a Microsoft account, the name for an offline one,
 * which is what makes two offline accounts of one name one account.
 *
 * A token is not something to read: @expires_at is 0 when there is none
 * and @access_token is NULL when it is gone, which is the state a
 * Microsoft account is put in when its refresh failed and the user has to
 * sign in again. */
typedef struct {
  NuvashaAccountType type;

  char *id;
  char *name;         /* player name passed to the game */
  char *uuid;         /* dashed and lowercase, as the game is given it */

  char *access_token; /* a Microsoft account only */
  gint64 expires_at;  /* unix seconds; 0 when there is no token */
  char *refresh_token;

  char *avatar_url;   /* when the profile published one */
} NuvashaAccount;

void nuvasha_account_free (NuvashaAccount *account);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaAccount, nuvasha_account_free)

/* (transfer full). The name has to be one java accepts as a player name:
 * 3 to 16 characters of letters, digits and underscores, or this fails
 * with nuvasha_ACCOUNTS_ERROR_INVALID saying which of the two it is. */
NuvashaAccount *nuvasha_account_new_offline (const char *name, GError **error);

/* (transfer full) */
NuvashaAccount *nuvasha_account_copy (const NuvashaAccount *account);

/* Boxed account type used by signals. */
#define nuvasha_TYPE_ACCOUNT (nuvasha_account_get_type ())
GType nuvasha_account_get_type (void);

/* Human-readable account type: "Microsoft" or "Offline". */
const char *nuvasha_account_type_name (NuvashaAccountType type);

/* TRUE when the account can be played as at @now: an offline account
   always can, and a Microsoft account can while its access token has not
   come within nuvasha_ACCOUNTS_TOKEN_MARGIN seconds of its expiry. */
gboolean nuvasha_account_token_is_valid (const NuvashaAccount *account, gint64 now);

/* The UUID java hands out for an offline player: the MD5 of
 * "OfflinePlayer:<name>" as a name based UUID, which means the version
 * nibble is 3 and the variant bits are the IETF ones. (transfer full) */
char *nuvasha_account_offline_uuid (const char *name);

typedef struct _NuvashaAccounts NuvashaAccounts;

#define nuvasha_TYPE_ACCOUNTS (nuvasha_accounts_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaAccounts, nuvasha_accounts, nuvasha, ACCOUNTS, GObject)

/* Emitted when the list, active account, or account tokens change. */
enum {
  nuvasha_ACCOUNTS_SIGNAL_CHANGED,
  nuvasha_ACCOUNTS_N_SIGNALS
};

/* The store of the user's own file. (transfer full) */
NuvashaAccounts *nuvasha_accounts_new (void);

/* Create a store backed by @filename. (transfer full) */
NuvashaAccounts *nuvasha_accounts_new_for_path (const char *filename);

/* The file the store keeps its accounts in. Never NULL. */
const char *nuvasha_accounts_path (NuvashaAccounts *self);

/* (transfer container) The accounts in the order they were added, as
   NuvashaAccount* this store keeps. */
GPtrArray *nuvasha_accounts_list (NuvashaAccounts *self);

guint nuvasha_accounts_count (NuvashaAccounts *self);

/* Return the active account, or NULL when none is selected. */
NuvashaAccount *nuvasha_accounts_get_active (NuvashaAccounts *self);

const char *nuvasha_accounts_get_active_id (NuvashaAccounts *self);

/* Makes @id the active account, which is the only one that can be.
   nuvasha_ACCOUNTS_ERROR_UNKNOWN when there is no such account. */
gboolean nuvasha_accounts_set_active (NuvashaAccounts *self, const char *id,
                                  GError **error);

/* The account @id, or NULL. */
NuvashaAccount *nuvasha_accounts_find (NuvashaAccounts *self, const char *id);

/* Adds an account and makes it the active one, since a user who just
   signed in meant to play as it. Adding the identity of an account that
   is already there replaces it, which is how a refreshed token or a
   changed profile is stored. */
gboolean nuvasha_accounts_add (NuvashaAccounts *self, const NuvashaAccount *account,
                           GError **error);

/* Replace an existing account and keep it active. */
gboolean nuvasha_accounts_update (NuvashaAccounts *self, const NuvashaAccount *account,
                              GError **error);

/* Removes an account. The active one goes, the first of what is left
   takes its place, and the store can end up with none. */
gboolean nuvasha_accounts_remove (NuvashaAccounts *self, const char *id,
                              GError **error);

G_END_DECLS

#endif /* nuvasha_ACCOUNTS_H */
