/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlInstancePage: the page of a single instance. Its header holds a
 * back button and a view switcher over the five pages of the instance:
 * Settings, Resource Packs, Shader Packs, Mods and Logs. */

#ifndef AML_INSTANCE_PAGE_H
#define AML_INSTANCE_PAGE_H

#include <adwaita.h>

#include "aml-instance-manager.h"
#include "aml-launcher.h"
#include "aml-settings.h"

G_BEGIN_DECLS

#define AML_TYPE_INSTANCE_PAGE (aml_instance_page_get_type ())

G_DECLARE_FINAL_TYPE (AmlInstancePage, aml_instance_page, AML, INSTANCE_PAGE,
                      AdwNavigationPage)

/* Builds the page for @instance. @launcher is the launcher of that same
 * instance, which the Logs page follows. The page keeps its own
 * references on @manager, @instance, @settings and @launcher until it is
 * destroyed. Like every widget it starts with a floating reference: push
 * it into an AdwNavigationView, which takes that reference. */
AmlInstancePage *aml_instance_page_new (AmlInstanceManager *manager,
                                        AmlInstance       *instance,
                                        AmlSettings        *settings,
                                        AmlLauncher       *launcher);

G_END_DECLS

#endif /* AML_INSTANCE_PAGE_H */
