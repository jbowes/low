/*
 *  Low: a yum-like package manager
 *
 *  Copyright (C) 2008 James Bowes <jbowes@dangerouslyinc.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 *  02110-1301  USA
 */

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "low-debug.h"
#include "low-transaction.h"
#include "low-repo-rpmdb.h"

/**
 * \page depsolver The Depedency Resolution Algorithm
 *
 * This page documents the dependency resolution algorithm used in Low
 * (as copied from YUM).
 *
 * \section algorithm The Algorithm
 * - WHILE there are unresolved dependencies DO:
 *   - FOR EACH package to be installed DO:
 *     - FOR EACH requires of the package DO:
 *       - IF NOT requires provided by installed packages
 *         OR NOT requires provided by packages in the transaction DO:
 * 	     - Add requires to unresolved requires.
 */

typedef enum _LowTransactionStatus {
	LOW_TRANSACTION_NO_CHANGE,
	LOW_TRANSACTION_PACKAGES_ADDED,
	LOW_TRANSACTION_UNRESOLVABLE
} LowTransactionStatus;

LowTransaction *
low_transaction_new (LowRepo *rpmdb, LowRepoSet *repos) {
	LowTransaction *trans = malloc (sizeof (LowTransaction));

	trans->rpmdb = rpmdb;
	trans->repos = repos;

	trans->install = NULL;
	trans->update = NULL;
	trans->remove = NULL;

	trans->unresolved = NULL;

	return trans;
}

static int
low_transaction_pkg_compare_func (gconstpointer data1, gconstpointer data2)
{
	const LowPackage *pkg1 = (LowPackage *) data1;
	const LowPackage *pkg2 = (LowPackage *) data2;

	if (!strcmp (pkg1->name, pkg2->name) &&
	    !strcmp (pkg1->version, pkg2->version) &&
	    !strcmp (pkg1->release, pkg2->release) &&
	    !strcmp (pkg1->arch, pkg2->arch) &&
	    (!(pkg1->epoch || pkg2->epoch) ||
	     !strcmp (pkg1->epoch, pkg2->epoch))) {
		return 0;
	} else {
		return 1;
	}

}

gboolean
low_transaction_add_install (LowTransaction *trans, LowPackage *to_install)
{
	if (!g_slist_find_custom (trans->install, to_install,
				  low_transaction_pkg_compare_func)) {
		low_debug_pkg ("Adding for install", to_install);
		trans->install = g_slist_append (trans->install, to_install);

		return TRUE;
	} else {
		low_debug_pkg ("Not adding already added pkg for install",
			       to_install);
		/* XXX not the right place for this */
		low_package_unref (to_install);

		return FALSE;
	}
}

void
low_transaction_add_update (LowTransaction *trans, LowPackage *to_update)
{
	low_debug_pkg ("Adding for update", to_update);

	trans->update = g_slist_append (trans->update, to_update);
}

gboolean
low_transaction_add_remove (LowTransaction *trans, LowPackage *to_remove)
{
	if (!g_slist_find_custom (trans->remove, to_remove,
				  low_transaction_pkg_compare_func)) {
		low_debug_pkg ("Adding for removal", to_remove);
		trans->remove = g_slist_append (trans->remove, to_remove);

		return TRUE;
	} else {
		low_debug_pkg ("Not adding already added pkg for removal",
			       to_remove);

		/* XXX not the right place for this */
		low_package_unref (to_remove);

		return FALSE;
	}
}

/**
 * Check if a requires is in a list of provides
 */
static gboolean
low_transaction_dep_in_deplist (const LowPackageDependency *needle,
				LowPackageDependency **haystack)
{
	int i;

	for (i = 0; haystack[i] != NULL; i++) {
		if (!strcmp (needle->name, haystack[i]->name)) {
			return TRUE;
		}
	}

	return FALSE;
}

/**
 * Check if a requires is in a list of files
 */
static gboolean
low_transaction_dep_in_filelist (const char *needle, char **haystack)
{
	int i;

	for (i = 0; haystack[i] != NULL; i++) {
		if (!strcmp (needle, haystack[i])) {
			return TRUE;
		}
	}

	return FALSE;
}

static LowTransactionStatus
low_transaction_check_removal (LowTransaction *trans, LowPackage *pkg)
{
	LowTransactionStatus status = LOW_TRANSACTION_OK;
	LowPackageDependency **provides;
	char **files;
	int i;

	low_debug_pkg ("Checking removal of", pkg);

	provides = low_package_get_provides (pkg);
	files = low_package_get_files (pkg);

	for (i = 0; provides[i] != NULL; i++) {
		LowPackageIter *iter;

		low_debug ("Checking provides %s", provides[i]->name);

		iter = low_repo_rpmdb_search_requires (trans->rpmdb,
						       provides[i]->name);
		while (iter = low_package_iter_next (iter), iter != NULL) {
			LowPackage *pkg = iter->pkg;

			low_debug_pkg ("Adding for removal", pkg);
			if (low_transaction_add_remove (trans, pkg)) {
				status = LOW_TRANSACTION_PACKAGES_ADDED;
			}
		}
	}

	for (i = 0; files[i] != NULL; i++) {
		LowPackageIter *iter;

		low_debug ("Checking file %s", files[i]);

		iter = low_repo_rpmdb_search_requires (trans->rpmdb,
						       files[i]);
		while (iter = low_package_iter_next (iter), iter != NULL) {
			LowPackage *pkg = iter->pkg;

			low_debug_pkg ("Adding for removal", pkg);
			if (low_transaction_add_remove (trans, pkg)) {
				status = LOW_TRANSACTION_PACKAGES_ADDED;
			}
		}
	}

	low_package_dependency_list_free (provides);
	g_strfreev (files);

	return status;
}
static LowTransactionStatus
low_transaction_check_package_requires (LowTransaction *trans, LowPackage *pkg)
{
	LowTransactionStatus status;
	LowPackageDependency **requires;
	LowPackageDependency **provides;
	char **files;
	int i;

	low_debug_pkg ("Checking requires for", pkg);

	requires = low_package_get_requires (pkg);
	provides = low_package_get_provides (pkg);
	files = low_package_get_files (pkg);

	for (i = 0; requires[i] != NULL; i++) {
		LowPackageIter *providing;

		if (low_transaction_dep_in_deplist (requires[i], provides)
		    || low_transaction_dep_in_filelist (requires[i]->name,
						        files)) {
		    low_debug ("Self provided requires %s, skipping",
			       requires[i]->name);
		    continue;
		}
		low_debug ("Checking requires %s", requires[i]->name);

		providing =
			low_repo_rpmdb_search_provides (trans->rpmdb,
							requires[i]->name);

		providing = low_package_iter_next (providing);
		if (providing != NULL) {
			low_debug_pkg ("Provided by", providing->pkg);
			low_package_unref (providing->pkg);

			/* XXX we just need a free function */
			while (providing = low_package_iter_next (providing),
				   providing != NULL) {
					low_package_unref (providing->pkg);
			}
			continue;
		/* Check files if appropriate */
		} else if (requires[i]->name[0] == '/') {
			providing =
				low_repo_rpmdb_search_files (trans->rpmdb,
							     requires[i]->name);

			providing = low_package_iter_next (providing);
			if (providing != NULL) {
				low_debug_pkg ("Provided by", providing->pkg);
				low_package_unref (providing->pkg);

				/* XXX we just need a free function */
				while (providing = low_package_iter_next (providing),
					   providing != NULL) {
						low_package_unref (providing->pkg);
				}
				continue;
			}
		}

		/* Check available packages */
		providing = low_repo_set_search_provides (trans->repos,
							  requires[i]->name);

		providing = low_package_iter_next (providing);
		if (providing != NULL) {
			low_debug_pkg ("Provided by", providing->pkg);
			/* XXX this might be an update, as well */
			if (low_transaction_add_install (trans,
							 providing->pkg)) {
				status = LOW_TRANSACTION_PACKAGES_ADDED;
			}
			/* XXX we just need a free function */
			while (providing = low_package_iter_next (providing),
				   providing != NULL) {
					low_package_unref (providing->pkg);
			}

			continue;
		/* Check files if appropriate */
		} else if (requires[i]->name[0] == '/') {
			providing =
				low_repo_set_search_files (trans->repos,
							   requires[i]->name);

			providing = low_package_iter_next (providing);
			if (providing != NULL) {
				low_debug_pkg ("Provided by", providing->pkg);
				if (low_transaction_add_install (trans, providing->pkg)) {
					status = LOW_TRANSACTION_PACKAGES_ADDED;
				}
				/* XXX we just need a free function */
				while (providing = low_package_iter_next (providing),
					   providing != NULL) {
						low_package_unref (providing->pkg);
				}

				continue;
			}

		}

		low_debug ("%s not provided by installed pkg",
			   requires[i]->name);
		return LOW_TRANSACTION_UNRESOLVABLE;
	}

	low_package_dependency_list_free (provides);
	low_package_dependency_list_free (requires);
	g_strfreev (files);

	return LOW_TRANSACTION_OK;
}

static LowTransactionStatus
low_transaction_check_all_requires (LowTransaction *trans)
{
	LowTransactionStatus status;
	GSList *cur = trans->install;

	while (cur != NULL) {
		LowPackage *pkg = (LowPackage *) cur->data;

		status = low_transaction_check_package_requires (trans, pkg);

		if (status == LOW_TRANSACTION_UNRESOLVABLE) {
			low_debug_pkg ("Adding to unresolved", pkg);
			trans->unresolved = g_slist_append (trans->unresolved,
							    pkg);
			trans->install = g_slist_remove (trans->install, pkg);
			return status;
		}

		cur = cur->next;
	}

	cur = trans->remove;
	while (cur != NULL) {
		LowPackage *pkg = (LowPackage *) cur->data;

		status = low_transaction_check_removal (trans, pkg);

		cur = cur->next;
	}

	return LOW_TRANSACTION_NO_CHANGE;
}

static LowPackage *
low_transaction_search_provides (GSList *list, char *query)
{
	/*
	 * XXX would it be faster to search the repos then compare against
	 *     our transaction?
	 */
	GSList *cur;

	for (cur = list; cur != NULL; cur = cur->next) {
		LowPackageDependency **provides =
			low_package_get_provides (cur->data);
		int i;

		for (i = 0; provides[i] != NULL; i++) {
			if (!strcmp (query, provides[i]->name)) {
				low_package_dependency_list_free (provides);
				return cur->data;
			}
		}

		low_package_dependency_list_free (provides);

	}

	return NULL;
}

static LowTransactionStatus
low_transaction_check_all_conflicts (LowTransaction *trans)
{
	LowTransactionStatus status = LOW_TRANSACTION_NO_CHANGE;
	GSList *cur = trans->install;

	while (cur != NULL) {
		LowPackage *pkg = (LowPackage *) cur->data;
		LowPackageDependency **provides =
			low_package_get_provides (pkg);
		LowPackageDependency **conflicts =
			low_package_get_conflicts (pkg);
		int i;

		low_debug_pkg ("Checking for installed pkgs that conflict",
			       pkg);
		for (i = 0; provides[i] != NULL; i++) {
			LowPackageIter *iter;
			iter = low_repo_rpmdb_search_conflicts (trans->rpmdb,
								provides[i]->name);

			iter = low_package_iter_next (iter);
			if (iter != NULL) {
				low_debug_pkg ("Conflicted by", iter->pkg);
				low_package_unref (iter->pkg);

				/* XXX we just need a free function */
				while (iter = low_package_iter_next (iter),
				       iter != NULL) {
					low_package_unref (iter->pkg);
				}
				status = LOW_TRANSACTION_UNRESOLVABLE;
				break;
			}

		}

		for (i = 0; conflicts[i] != NULL; i++) {
			LowPackageIter *iter;
			iter = low_repo_rpmdb_search_provides (trans->rpmdb,
							       conflicts[i]->name);

			iter = low_package_iter_next (iter);
			if (iter != NULL) {
				low_debug_pkg ("Conflicts with", iter->pkg);
				low_package_unref (iter->pkg);

				/* XXX we just need a free function */
				while (iter = low_package_iter_next (iter),
				       iter != NULL) {
					low_package_unref (iter->pkg);
				}
				status = LOW_TRANSACTION_UNRESOLVABLE;
				break;
			}

		}

		/*
		 * We only need to search provides here, because we'll look
		 * at the other pkg anyway.
		 */
		low_debug_pkg ("Checking for other installing pkgs that conflict",
			       pkg);

		for (i = 0; conflicts[i] != NULL; i++) {
			LowPackage *conflicting =
				low_transaction_search_provides (trans->install,
								 conflicts[i]->name);
			if (conflicting) {
				low_debug_pkg ("Conflicted by installing",
					   conflicting);

				low_debug_pkg ("Adding to unresolved",
					       conflicting);
				trans->unresolved =
					g_slist_append (trans->unresolved,
							conflicting);
				trans->install =
					g_slist_remove (trans->install,
							conflicting);


				status = LOW_TRANSACTION_UNRESOLVABLE;
				break;
			}

		}

		low_package_dependency_list_free (provides);
		low_package_dependency_list_free (conflicts);

		if (status == LOW_TRANSACTION_UNRESOLVABLE) {
			low_debug_pkg ("Adding to unresolved", pkg);
			trans->unresolved = g_slist_append (trans->unresolved,
							    pkg);
			trans->install = g_slist_remove (trans->install, pkg);
			return status;
		}


		cur = cur->next;
	}

	return status;
}

LowTransactionResult
low_transaction_resolve (LowTransaction *trans G_GNUC_UNUSED)
{
	LowTransactionStatus status;

	struct timeval start;
	struct timeval end;

	low_debug ("Resolving transaction");
	gettimeofday (&start, NULL);

	while (TRUE) {
		status = low_transaction_check_all_conflicts (trans);
		if (status == LOW_TRANSACTION_UNRESOLVABLE) {
			low_debug ("Unresolvable transaction");
			return LOW_TRANSACTION_UNRESOLVED;
		}

		status = low_transaction_check_all_requires (trans);
		if (status == LOW_TRANSACTION_UNRESOLVABLE) {
			low_debug ("Unresolvable transaction");
			return LOW_TRANSACTION_UNRESOLVED;
		}

		break;
	}

	gettimeofday (&end, NULL);
	low_debug ("Transaction resolved successfully in %.2fs",
		   (int) (end.tv_sec - start.tv_sec) +
		   ((float) (end.tv_usec - start.tv_usec) / 1000000));
	return LOW_TRANSACTION_OK;
}

void
low_transaction_free (LowTransaction *trans)
{
	g_slist_foreach (trans->install, (GFunc) low_package_unref, NULL);
	g_slist_foreach (trans->update, (GFunc) low_package_unref, NULL);
	g_slist_foreach (trans->remove, (GFunc) low_package_unref, NULL);

	g_slist_foreach (trans->unresolved, (GFunc) low_package_unref, NULL);

	g_slist_free (trans->install);
	g_slist_free (trans->update);
	g_slist_free (trans->remove);

	g_slist_free (trans->unresolved);

	free (trans);
}

/* vim: set ts=8 sw=8 noet: */
