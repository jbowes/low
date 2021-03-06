/*
 *  Low: a yum-like package manager
 *
 *  Copyright (C) 2008 - 2010 James Bowes <jbowes@repl.ca>
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
#include "low-debug.h"
#include "low-repo-sqlite.h"
#include "low-repo-set.h"

LowRepoSet *
low_repo_set_initialize_from_config (LowConfig *config, bool bind_dbs)
{
	unsigned int i;
	char **repo_names;
	LowRepoSet *repo_set = malloc (sizeof (LowRepoSet));

	repo_set->repos = g_hash_table_new (NULL, NULL);

	repo_names = low_config_get_repo_names (config);
	for (i = 0; i < g_strv_length (repo_names); i++) {
		char *id = repo_names[i];
		char *name =
			low_config_get_string (config, repo_names[i], "name");
		char *baseurl = low_config_get_string (config, repo_names[i],
						       "baseurl");
		char *mirror_list = low_config_get_string (config,
							   repo_names[i],
							   "mirrorlist");
		bool enabled = low_config_get_bool (config, repo_names[i],
						    "enabled");
		LowRepo *repo = low_repo_sqlite_initialize (id, name, baseurl,
							    mirror_list,
							    enabled,
							    bind_dbs);

		free (name);
		free (baseurl);
		free (mirror_list);

		/* failed to initialize (probably missing sqlite file) */
		if (repo == NULL) {
			low_repo_set_free (repo_set);
			repo_set = NULL;
			break;
		}

		g_hash_table_insert (repo_set->repos, id, repo);
	}
	g_strfreev (repo_names);

	return repo_set;
}

static void
low_repo_set_free_repo (gpointer key G_GNUC_UNUSED, gpointer value,
			gpointer user_data G_GNUC_UNUSED)
{
	LowRepo *repo = (LowRepo *) value;

	low_repo_sqlite_shutdown (repo);
}

void
low_repo_set_free (LowRepoSet *repo_set)
{
	g_hash_table_foreach (repo_set->repos, low_repo_set_free_repo, NULL);
	g_hash_table_unref (repo_set->repos);
	free (repo_set);
}

typedef struct _LowRepoSetForEachData {
	LowRepoSetFilter filter;
	LowRepoSetFunc func;
} LowRepoSetForEachData;

static void
low_repo_set_inner_for_each (gpointer key G_GNUC_UNUSED, gpointer value,
			     gpointer data)
{
	LowRepo *repo = (LowRepo *) value;
	LowRepoSetForEachData *for_each_data = (LowRepoSetForEachData *) data;

	if (for_each_data->filter == ALL ||
	    (for_each_data->filter == ENABLED && repo->enabled) ||
	    (for_each_data->filter == DISABLED && !repo->enabled)) {
		for_each_data->func (repo);
	}
}

void
low_repo_set_for_each (LowRepoSet *repo_set, LowRepoSetFilter filter,
		       LowRepoSetFunc func)
{
	LowRepoSetForEachData for_each_data;
	for_each_data.filter = filter;
	for_each_data.func = func;

	g_hash_table_foreach (repo_set->repos, low_repo_set_inner_for_each,
			      &for_each_data);
}

typedef LowPackageIter *(*LowRepoSetIterSearchFunc) (LowRepo *repo,
						     const void *search_data);

typedef struct _LowRepoSetPackageIter {
	LowPackageIter super;
	GHashTableIter *repo_iter;
	LowPackageIter *current_repo_iter;
	LowRepo *current_repo;
	const char *search_data;
	LowRepoSetIterSearchFunc search_func;
} LowRepoSetPackageIter;

static LowPackageIter *
low_repo_set_package_iter_next (LowPackageIter *iter)
{
	LowRepoSetPackageIter *iter_set = (LowRepoSetPackageIter *) iter;
	LowRepo *current_repo = iter_set->current_repo;
	LowPackageIter *current_repo_iter = iter_set->current_repo_iter;

	/* XXX When we have no repos. this is ugly. */
	if (current_repo == NULL) {
		free (iter_set->repo_iter);
		free (iter);
		return NULL;
	}

	current_repo_iter = low_package_iter_next (current_repo_iter);

	/* This should cover repos that return 0 packages from the iter */
	while (current_repo_iter == NULL && current_repo != NULL) {
		do {
			current_repo = NULL;
			g_hash_table_iter_next (iter_set->repo_iter, NULL,
						(gpointer) &current_repo);
		} while (current_repo != NULL && !current_repo->enabled);
		if (current_repo == NULL) {
			current_repo_iter = NULL;
			break;
		}

		low_debug ("On repo '%s'", current_repo->id);

		current_repo_iter =
			iter_set->search_func (current_repo,
					       iter_set->search_data);
		current_repo_iter = low_package_iter_next (current_repo_iter);
	}

	if (current_repo_iter == NULL) {
		free (iter_set->repo_iter);
		free (iter);
		return NULL;
	}

	iter_set->current_repo = current_repo;
	iter_set->current_repo_iter = current_repo_iter;

	iter->pkg = iter_set->current_repo_iter->pkg;

	return iter;
}

static void
low_repo_set_package_iter_free (LowPackageIter *iter)
{
	LowRepoSetPackageIter *iter_set = (LowRepoSetPackageIter *) iter;

	low_package_iter_free (iter_set->current_repo_iter);
	free (iter_set->repo_iter);
	free (iter);
}

static LowPackageIter *
low_repo_set_package_iter_new (LowRepoSet *repo_set,
			       LowRepoSetIterSearchFunc search_func,
			       const void *search_data)
{
	LowRepoSetPackageIter *iter = malloc (sizeof (LowRepoSetPackageIter));
	iter->super.next_func = low_repo_set_package_iter_next;
	iter->super.free_func = low_repo_set_package_iter_free;
	iter->super.pkg = NULL;
	iter->repo_iter = malloc (sizeof (GHashTableIter));
	g_hash_table_iter_init (iter->repo_iter, repo_set->repos);

	do {
		iter->current_repo = NULL;
		g_hash_table_iter_next (iter->repo_iter, NULL,
					(gpointer) &(iter->current_repo));
	} while (iter->current_repo != NULL && !iter->current_repo->enabled);

	iter->search_func = search_func;
	iter->search_data = search_data;

	/* XXX For an empty hashtable. kind of ugly. */
	if (iter->current_repo != NULL) {
		low_debug ("On repo '%s'", iter->current_repo->id);
		iter->current_repo_iter =
			iter->search_func (iter->current_repo, search_data);
	}

	return (LowPackageIter *) iter;
}

LowPackageIter *
low_repo_set_list_all (LowRepoSet *repo_set)
{
	/*
	 * XXX list_all doesn't take any args, so we get off by passing NULL.
	 *     this is still ugly though.
	 */
	return low_repo_set_package_iter_new (repo_set,
					      (LowRepoSetIterSearchFunc)
					      low_repo_sqlite_list_all, NULL);
}

LowPackageIter *
low_repo_set_list_by_name (LowRepoSet *repo_set, const char *name)
{
	return low_repo_set_package_iter_new (repo_set,
					      (LowRepoSetIterSearchFunc)
					      low_repo_sqlite_list_by_name,
					      name);
}

LowPackageIter *
low_repo_set_search_provides (LowRepoSet *repo_set,
			      const LowPackageDependency *provides)
{
	return low_repo_set_package_iter_new (repo_set,
					      (LowRepoSetIterSearchFunc)
					      low_repo_sqlite_search_provides,
					      provides);
}

LowPackageIter *
low_repo_set_search_requires (LowRepoSet *repo_set,
			      const LowPackageDependency *requires)
{
	return low_repo_set_package_iter_new (repo_set,
					      (LowRepoSetIterSearchFunc)
					      low_repo_sqlite_search_requires,
					      requires);
}

LowPackageIter *
low_repo_set_search_conflicts (LowRepoSet *repo_set,
			       const LowPackageDependency *conflicts)
{
	return low_repo_set_package_iter_new (repo_set,
					      (LowRepoSetIterSearchFunc)
					      low_repo_sqlite_search_conflicts,
					      conflicts);
}

LowPackageIter *
low_repo_set_search_obsoletes (LowRepoSet *repo_set,
			       const LowPackageDependency *obsoletes)
{
	return low_repo_set_package_iter_new (repo_set,
					      (LowRepoSetIterSearchFunc)
					      low_repo_sqlite_search_obsoletes,
					      obsoletes);
}

LowPackageIter *
low_repo_set_search_files (LowRepoSet *repo_set, const char *file)
{
	return low_repo_set_package_iter_new (repo_set,
					      (LowRepoSetIterSearchFunc)
					      low_repo_sqlite_search_files,
					      file);
}

LowPackageIter *
low_repo_set_search_details (LowRepoSet *repo_set, const char *querystr)
{
	return low_repo_set_package_iter_new (repo_set,
					      (LowRepoSetIterSearchFunc)
					      low_repo_sqlite_search_details,
					      querystr);
}

/* vim: set ts=8 sw=8 noet: */
