/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* LeppoisaAccounts: the accounts a user can play as, and which one is active.
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
 *               leppoisa-auth-microsoft.h. What is kept is the profile (name
 *               and UUID), the access token with the moment it stops
 *               being good and the refresh token that trades for a new
 *               one. No password is ever asked for or kept.
 *
 * Exactly one account is active, and it is the one the launcher runs the
 * game as. Tokens are never logged, never printed in an error and never
 * put anywhere but this file, which is written for nobody but the user:
 * mode 0600, written whole or not at all.
 *
 * The file is <user data dir>/leppoisa/accounts.json. A file that is missing,
 * unreadable or nonsense leaves the store empty rather than failing, the
 * same as the settings file. */

#ifndef leppoisa_ACCOUNTS_H
#define leppoisa_ACCOUNTS_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define leppoisa_ACCOUNTS_ERROR (leppoisa_accounts_error_quark ())

typedef enum {
  leppoisa_ACCOUNTS_ERROR_IO,      /* the file cannot be read or written */
  leppoisa_ACCOUNTS_ERROR_INVALID, /* what it was asked to store makes no sense */
  leppoisa_ACCOUNTS_ERROR_UNKNOWN   /* there is no such account */
} LeppoisaAccountsError;

GQuark leppoisa_accounts_error_quark (void);

/* How far ahead of its expiry a token stops being good: the game would
 * be handed a token that dies while it is still booting. */
#define leppoisa_ACCOUNTS_TOKEN_MARGIN 60

typedef enum {
  leppoisa_ACCOUNT_OFFLINE,   /* a name and nothing else */
  leppoisa_ACCOUNT_MICROSOFT  /* signed in with a Microsoft account */
} LeppoisaAccountType;

/* One account. @id is what the store keys it by: the UUID of the
 * Minecraft profile for a Microsoft account, the name for an offline one,
 * which is what makes two offline accounts of one name one account.
 *
 * A token is not something to read: @expires_at is 0 when there is none
 * and @access_token is NULL when it is gone, which is the state a
 * Microsoft account is put in when its refresh failed and the user has to
 * sign in again. */
typedef struct {
  LeppoisaAccountType type;

  char *id;
  char *name;         /* what the game is told the player is called */
  char *uuid;         /* dashed and lowercase, as the game is given it */

  char *access_token; /* a Microsoft account only */
  gint64 expires_at;  /* unix seconds; 0 when there is no token */
  char *refresh_token;

  char *avatar_url;   /* when the profile published one */
} LeppoisaAccount;

void leppoisa_account_free (LeppoisaAccount *account);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (LeppoisaAccount, leppoisa_account_free)

/* (transfer full). The name has to be one java accepts as a player name:
 * 3 to 16 characters of letters, digits and underscores, or this fails
 * with leppoisa_ACCOUNTS_ERROR_INVALID saying which of the two it is. */
LeppoisaAccount *leppoisa_account_new_offline (const char *name, GError **error);

/* (transfer full) */
LeppoisaAccount *leppoisa_account_copy (const LeppoisaAccount *account);

/* An account as a boxed type, which is what makes it something a signal
 * can carry: the receiver is handed a copy of its own. */
#define leppoisa_TYPE_ACCOUNT (leppoisa_account_get_type ())
GType leppoisa_account_get_type (void);

/* What a user reads: "Microsoft" or "Offline". Never NULL. */
const char *leppoisa_account_type_name (LeppoisaAccountType type);

/* TRUE when the account can be played as at @now: an offline account
   always can, and a Microsoft account can while its access token has not
   come within leppoisa_ACCOUNTS_TOKEN_MARGIN seconds of its expiry. */
gboolean leppoisa_account_token_is_valid (const LeppoisaAccount *account, gint64 now);

/* The UUID java hands out for an offline player: the MD5 of
 * "OfflinePlayer:<name>" as a name based UUID, which means the version
 * nibble is 3 and the variant bits are the IETF ones. (transfer full) */
char *leppoisa_account_offline_uuid (const char *name);

typedef struct _LeppoisaAccounts LeppoisaAccounts;

#define leppoisa_TYPE_ACCOUNTS (leppoisa_accounts_get_type ())

G_DECLARE_FINAL_TYPE (LeppoisaAccounts, leppoisa_accounts, leppoisa, ACCOUNTS, GObject)

/* "changed" is what a view listens to: the list, the active account or
   the tokens of one of them are not what they were. */
enum {
  leppoisa_ACCOUNTS_SIGNAL_CHANGED,
  leppoisa_ACCOUNTS_N_SIGNALS
};

/* The store of the user's own file. (transfer full) */
LeppoisaAccounts *leppoisa_accounts_new (void);

/* The same over another file, which is what a test writes into.
 * (transfer full) */
LeppoisaAccounts *leppoisa_accounts_new_for_path (const char *filename);

/* The file the store keeps its accounts in. Never NULL. */
const char *leppoisa_accounts_path (LeppoisaAccounts *self);

/* (transfer container) The accounts in the order they were added, as
   LeppoisaAccount* this store keeps. */
GPtrArray *leppoisa_accounts_list (LeppoisaAccounts *self);

guint leppoisa_accounts_count (LeppoisaAccounts *self);

/* The active account, or NULL when there is none: launching then says so
   rather than playing as somebody nobody chose. */
LeppoisaAccount *leppoisa_accounts_get_active (LeppoisaAccounts *self);

const char *leppoisa_accounts_get_active_id (LeppoisaAccounts *self);

/* Makes @id the active account, which is the only one that can be.
   leppoisa_ACCOUNTS_ERROR_UNKNOWN when there is no such account. */
gboolean leppoisa_accounts_set_active (LeppoisaAccounts *self, const char *id,
                                  GError **error);

/* The account @id, or NULL. */
LeppoisaAccount *leppoisa_accounts_find (LeppoisaAccounts *self, const char *id);

/* Adds an account and makes it the active one, since a user who just
   signed in meant to play as it. Adding the identity of an account that
   is already there replaces it, which is how a refreshed token or a
   changed profile is stored. */
gboolean leppoisa_accounts_add (LeppoisaAccounts *self, const LeppoisaAccount *account,
                           GError **error);

/* The same as adding: what leppoisa calls updating an account it already
   keeps is the same thing. */
gboolean leppoisa_accounts_update (LeppoisaAccounts *self, const LeppoisaAccount *account,
                              GError **error);

/* Removes an account. The active one goes, the first of what is left
   takes its place, and the store can end up with none. */
gboolean leppoisa_accounts_remove (LeppoisaAccounts *self, const char *id,
                              GError **error);

G_END_DECLS

#endif /* leppoisa_ACCOUNTS_H */