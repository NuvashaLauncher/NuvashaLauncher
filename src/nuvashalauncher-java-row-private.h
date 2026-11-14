/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_JAVA_ROW_PRIVATE_H
#define nuvashalauncher_JAVA_ROW_PRIVATE_H

#include "nuvashalauncher-java-row.h"

struct _NuvashaLauncherJavaRow
{
  AdwComboRow parent_instance;

  /* Cancelled on reload and on dispose. */
  GCancellable *cancellable;

  /* The detected installations, GPtrArray<NuvashaLauncherJava*> owned by the row. */
  GPtrArray *javas;

  /* Model contents: the automatic runtime is index 0 with a NULL path;
   * the remaining entries are detected/custom runtimes and a trailing entry
   * that asks for another runtime instead of naming one. */
  GPtrArray *ids;   /* strdupped paths; the automatic entry has a NULL id */
  GPtrArray *names; /* the titles shown in the model */

  guint add_index;
  guint restore;

  /* Runtime represented by the automatic entry, when available.
   * That runtime is named in the first entry and left out of the list below
   * so it is not shown twice. */
  int required_major;

  /* Selection the instance asked for; NULL is the automatic entry. */
  char *wanted;
  gboolean probe_started;

  /* Custom Java probe result, including failed probes with only a path. */
  NuvashaLauncherJava *probed;

  gboolean ready;
  gboolean disposed;
  gboolean automatic_selection;
  gboolean rebuilding;
  GtkButton *retry;
  GtkButton *add_button;

  /* The request the row is waiting for. Borrowed from the JavaAsk that
     started it: that one releases it when the answer arrives, and a
     newer request replaces it before an older answer can arrive. */
  NuvashaLauncherTaskScope *operation;
};

/* One Java request the row started: the row, which can be disposed while
   the answer is on its way, and the handle that answer is to release. */
typedef struct
{
  NuvashaLauncherJavaRow *row;
  NuvashaLauncherTaskScope *operation;
} JavaAsk;

void nuvashalauncher_java_row_ask_free (JavaAsk *ask);
void nuvashalauncher_java_row_fill (NuvashaLauncherJavaRow *self);
void nuvashalauncher_java_row_show_error (NuvashaLauncherJavaRow *self,
                                          const char             *message);

void nuvashalauncher_java_row_detect_finished (GObject      *source,
                                               GAsyncResult *result,
                                               gpointer      user_data);
void nuvashalauncher_java_row_custom_loaded (GObject      *source,
                                             GAsyncResult *result,
                                             gpointer      user_data);
void nuvashalauncher_java_row_on_retry_clicked (GtkButton *button,
                                                gpointer   user_data);
void nuvashalauncher_java_row_on_add_clicked (GtkButton *button,
                                              gpointer   user_data);
void nuvashalauncher_java_row_on_picked (GObject    *object,
                                         GParamSpec *pspec,
                                         gpointer    user_data);

#endif /* nuvashalauncher_JAVA_ROW_PRIVATE_H */
