/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherAccounts: the accounts a user can play as, and which one is active.
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
 *               nuvashalauncher-auth-microsoft.h. Stored data includes the profile (name
 *               and UUID), the access token with the moment it stops
 *               being good and the refresh token that trades for a new
 *               one. No password is ever asked for or kept.
 *
 * Exactly one account is active, and it is the one the launcher runs the
 * game as. Tokens are never logged or printed in an error. Account records,
 * including offline accounts and cosmetic selections, are stored in the
 * desktop Secret Service. The filename accepted by the test/path constructor
 * remains only as an API compatibility value; it is not read or written. */

#ifndef nuvashalauncher_ACCOUNTS_H
#define nuvashalauncher_ACCOUNTS_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define nuvashalauncher_ACCOUNTS_ERROR (nuvashalauncher_accounts_error_quark ())

typedef enum {
  nuvashalauncher_ACCOUNTS_ERROR_IO,      /* the file cannot be read or written */
  nuvashalauncher_ACCOUNTS_ERROR_INVALID, /* invalid account data */
  nuvashalauncher_ACCOUNTS_ERROR_UNKNOWN   /* there is no such account */
} nuvashalauncherAccountsError;

GQuark nuvashalauncher_accounts_error_quark (void);

/* How far ahead of its expiry a token stops being good: the game would
 * be handed a token that dies while it is still booting. */
#define nuvashalauncher_ACCOUNTS_TOKEN_MARGIN 60

typedef enum {
  nuvashalauncher_ACCOUNT_OFFLINE,   /* a name and nothing else */
  nuvashalauncher_ACCOUNT_MICROSOFT  /* signed in with a Microsoft account */
} nuvashalauncherAccountType;

/* One account. @id is the store key: the UUID of the
 * Minecraft profile for a Microsoft account, the name for an offline one,
 * which is what makes two offline accounts of one name one account.
 *
 * A token is not something to read: @expires_at is 0 when there is none
 * and @access_token is NULL when it is gone, which is the state a
 * Microsoft account is put in when its refresh failed and the user has to
 * sign in again. */
typedef struct {
  nuvashalauncherAccountType type;

  char *id;
  char *name;         /* player name passed to the game */
  char *uuid;         /* dashed and lowercase, as the game is given it */

  char *access_token; /* a Microsoft account only */
  gint64 expires_at;  /* unix seconds; 0 when there is no token */
  char *refresh_token;

  char *avatar_url;   /* when the profile published one */
  char *skin_model;   /* CLASSIC or SLIM */
  char *cape_id;      /* currently selected cape, or NULL */
} nuvashalauncherAccount;

void nuvashalauncher_account_free (nuvashalauncherAccount *account);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (nuvashalauncherAccount, nuvashalauncher_account_free)

/* (transfer full). The name has to be one java accepts as a player name:
 * 3 to 16 characters of letters, digits and underscores, or this fails
 * with nuvashalauncher_ACCOUNTS_ERROR_INVALID saying which of the two it is. */
nuvashalauncherAccount *nuvashalauncher_account_new_offline (const char *name, GError **error);

/* (transfer full) */
nuvashalauncherAccount *nuvashalauncher_account_copy (const nuvashalauncherAccount *account);

/* Boxed account type used by signals. */
#define nuvashalauncher_TYPE_ACCOUNT (nuvashalauncher_account_get_type ())
GType nuvashalauncher_account_get_type (void);

/* Human-readable account type: "Microsoft" or "Offline". */
const char *nuvashalauncher_account_type_name (nuvashalauncherAccountType type);

/* TRUE when the account can be played as at @now: an offline account
   always can, and a Microsoft account can while its access token has not
   come within nuvashalauncher_ACCOUNTS_TOKEN_MARGIN seconds of its expiry. */
gboolean nuvashalauncher_account_token_is_valid (const nuvashalauncherAccount *account, gint64 now);

/* The UUID java hands out for an offline player: the MD5 of
 * "OfflinePlayer:<name>" as a name based UUID, which means the version
 * nibble is 3 and the variant bits are the IETF ones. (transfer full) */
char *nuvashalauncher_account_offline_uuid (const char *name);

typedef struct _nuvashalauncherAccounts nuvashalauncherAccounts;

#define nuvashalauncher_TYPE_ACCOUNTS (nuvashalauncher_accounts_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncherAccounts, nuvashalauncher_accounts, nuvashalauncher, ACCOUNTS, GObject)

/* Emitted when the list, active account, or account tokens change. */
enum {
  nuvashalauncher_ACCOUNTS_SIGNAL_CHANGED,
  nuvashalauncher_ACCOUNTS_N_SIGNALS
};

/* The store of the user's own file. (transfer full) */
nuvashalauncherAccounts *nuvashalauncher_accounts_new (void);

/* Create a store backed by @filename. (transfer full) */
nuvashalauncherAccounts *nuvashalauncher_accounts_new_for_path (const char *filename);

/* The file the store keeps its accounts in. Never NULL. */
const char *nuvashalauncher_accounts_path (nuvashalauncherAccounts *self);

/* (transfer container) The accounts in the order they were added, as
   nuvashalauncherAccount* this store keeps. */
GPtrArray *nuvashalauncher_accounts_list (nuvashalauncherAccounts *self);

guint nuvashalauncher_accounts_count (nuvashalauncherAccounts *self);

/* Return the active account, or NULL when none is selected. */
nuvashalauncherAccount *nuvashalauncher_accounts_get_active (nuvashalauncherAccounts *self);

const char *nuvashalauncher_accounts_get_active_id (nuvashalauncherAccounts *self);

/* Makes @id the active account, which is the only one that can be.
   nuvashalauncher_ACCOUNTS_ERROR_UNKNOWN when there is no such account. */
gboolean nuvashalauncher_accounts_set_active (nuvashalauncherAccounts *self, const char *id,
                                  GError **error);

/* The account @id, or NULL. */
nuvashalauncherAccount *nuvashalauncher_accounts_find (nuvashalauncherAccounts *self, const char *id);

/* Adds an account and makes it the active one, since a user who just
   signed in meant to play as it. Adding the identity of an account that
   is already there replaces it, which is how a refreshed token or a
   changed profile is stored. */
gboolean nuvashalauncher_accounts_add (nuvashalauncherAccounts *self, const nuvashalauncherAccount *account,
                           GError **error);

/* Replace an existing account and keep it active. */
gboolean nuvashalauncher_accounts_update (nuvashalauncherAccounts *self, const nuvashalauncherAccount *account,
                              GError **error);

/* Removes an account. The active one goes, the first of what is left
   takes its place, and the store can end up with none. */
gboolean nuvashalauncher_accounts_remove (nuvashalauncherAccounts *self, const char *id,
                              GError **error);

G_END_DECLS

#endif /* nuvashalauncher_ACCOUNTS_H */
