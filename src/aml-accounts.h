/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlAccounts: the accounts a user can play as, and which one is active.
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
 *               aml-auth-microsoft.h. What is kept is the profile (name
 *               and UUID), the access token with the moment it stops
 *               being good and the refresh token that trades for a new
 *               one. No password is ever asked for or kept.
 *
 * Exactly one account is active, and it is the one the launcher runs the
 * game as. Tokens are never logged, never printed in an error and never
 * put anywhere but this file, which is written for nobody but the user:
 * mode 0600, written whole or not at all.
 *
 * The file is <user data dir>/AML/accounts.json. A file that is missing,
 * unreadable or nonsense leaves the store empty rather than failing, the
 * same as the settings file. */

#ifndef AML_ACCOUNTS_H
#define AML_ACCOUNTS_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define AML_ACCOUNTS_ERROR (aml_accounts_error_quark ())

typedef enum {
  AML_ACCOUNTS_ERROR_IO,      /* the file cannot be read or written */
  AML_ACCOUNTS_ERROR_INVALID, /* what it was asked to store makes no sense */
  AML_ACCOUNTS_ERROR_UNKNOWN   /* there is no such account */
} AmlAccountsError;

GQuark aml_accounts_error_quark (void);

/* How far ahead of its expiry a token stops being good: the game would
 * be handed a token that dies while it is still booting. */
#define AML_ACCOUNTS_TOKEN_MARGIN 60

typedef enum {
  AML_ACCOUNT_OFFLINE,   /* a name and nothing else */
  AML_ACCOUNT_MICROSOFT  /* signed in with a Microsoft account */
} AmlAccountType;

/* One account. @id is what the store keys it by: the UUID of the
 * Minecraft profile for a Microsoft account, the name for an offline one,
 * which is what makes two offline accounts of one name one account.
 *
 * A token is not something to read: @expires_at is 0 when there is none
 * and @access_token is NULL when it is gone, which is the state a
 * Microsoft account is put in when its refresh failed and the user has to
 * sign in again. */
typedef struct {
  AmlAccountType type;

  char *id;
  char *name;         /* what the game is told the player is called */
  char *uuid;         /* dashed and lowercase, as the game is given it */

  char *access_token; /* a Microsoft account only */
  gint64 expires_at;  /* unix seconds; 0 when there is no token */
  char *refresh_token;

  char *avatar_url;   /* when the profile published one */
} AmlAccount;

void aml_account_free (AmlAccount *account);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlAccount, aml_account_free)

/* (transfer full). The name has to be one java accepts as a player name:
 * 3 to 16 characters of letters, digits and underscores, or this fails
 * with AML_ACCOUNTS_ERROR_INVALID saying which of the two it is. */
AmlAccount *aml_account_new_offline (const char *name, GError **error);

/* (transfer full) */
AmlAccount *aml_account_copy (const AmlAccount *account);

/* What a user reads: "Microsoft" or "Offline". Never NULL. */
const char *aml_account_type_name (AmlAccountType type);

/* TRUE when the account can be played as at @now: an offline account
   always can, and a Microsoft account can while its access token has not
   come within AML_ACCOUNTS_TOKEN_MARGIN seconds of its expiry. */
gboolean aml_account_token_is_valid (const AmlAccount *account, gint64 now);

/* The UUID java hands out for an offline player: the MD5 of
 * "OfflinePlayer:<name>" as a name based UUID, which means the version
 * nibble is 3 and the variant bits are the IETF ones. (transfer full) */
char *aml_account_offline_uuid (const char *name);

typedef struct _AmlAccounts AmlAccounts;

#define AML_TYPE_ACCOUNTS (aml_accounts_get_type ())

G_DECLARE_FINAL_TYPE (AmlAccounts, aml_accounts, AML, ACCOUNTS, GObject)

/* "changed" is what a view listens to: the list, the active account or
   the tokens of one of them are not what they were. */
enum {
  AML_ACCOUNTS_SIGNAL_CHANGED,
  AML_ACCOUNTS_N_SIGNALS
};

/* The store of the user's own file. (transfer full) */
AmlAccounts *aml_accounts_new (void);

/* The same over another file, which is what a test writes into.
 * (transfer full) */
AmlAccounts *aml_accounts_new_for_path (const char *filename);

/* The file the store keeps its accounts in. Never NULL. */
const char *aml_accounts_path (AmlAccounts *self);

/* (transfer container) The accounts in the order they were added, as
   AmlAccount* this store keeps. */
GPtrArray *aml_accounts_list (AmlAccounts *self);

guint aml_accounts_count (AmlAccounts *self);

/* The active account, or NULL when there is none: launching then says so
   rather than playing as somebody nobody chose. */
AmlAccount *aml_accounts_get_active (AmlAccounts *self);

const char *aml_accounts_get_active_id (AmlAccounts *self);

/* Makes @id the active account, which is the only one that can be.
   AML_ACCOUNTS_ERROR_UNKNOWN when there is no such account. */
gboolean aml_accounts_set_active (AmlAccounts *self, const char *id,
                                  GError **error);

/* The account @id, or NULL. */
AmlAccount *aml_accounts_find (AmlAccounts *self, const char *id);

/* Adds an account and makes it the active one, since a user who just
   signed in meant to play as it. Adding the identity of an account that
   is already there replaces it, which is how a refreshed token or a
   changed profile is stored. */
gboolean aml_accounts_add (AmlAccounts *self, const AmlAccount *account,
                           GError **error);

/* The same as adding: what AML calls updating an account it already
   keeps is the same thing. */
gboolean aml_accounts_update (AmlAccounts *self, const AmlAccount *account,
                              GError **error);

/* Removes an account. The active one goes, the first of what is left
   takes its place, and the store can end up with none. */
gboolean aml_accounts_remove (AmlAccounts *self, const char *id,
                              GError **error);

G_END_DECLS

#endif /* AML_ACCOUNTS_H */