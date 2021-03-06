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
#include <fcntl.h>
#include <string.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmdb.h>
#include <glib.h>
#include "low-debug.h"
#include "low-repo-rpmdb.h"

typedef struct _LowRepoRpmdb {
	LowRepo super;
	rpmdb db;
	GHashTable *table;
} LowRepoRpmdb;

/* XXX clean these up */
typedef bool (*LowPackageIterFilterFn) (LowPackage *pkg, gpointer data);
typedef void (*LowPackageIterFilterDataFree) (gpointer data);

typedef struct _LowPackageIterRpmdb {
	LowPackageIter super;
	rpmdbMatchIterator rpm_iter;
	LowPackageIterFilterFn func;
	gpointer filter_data;
	LowPackageIterFilterDataFree filter_data_free_func;
} LowPackageIterRpmdb;

LowPackageDetails *low_rpmdb_package_get_details (LowPackage *pkg);

LowPackageDependency **low_rpmdb_package_get_provides (LowPackage *pkg);
LowPackageDependency **low_rpmdb_package_get_requires (LowPackage *pkg);
LowPackageDependency **low_rpmdb_package_get_conflicts (LowPackage *pkg);
LowPackageDependency **low_rpmdb_package_get_obsoletes (LowPackage *pkg);

char **low_rpmdb_package_get_files (LowPackage *pkg);

LowRepo *
low_repo_rpmdb_initialize (void)
{
	LowRepoRpmdb *repo = malloc (sizeof (LowRepoRpmdb));

	repo->super.id = strdup ("installed");
	repo->super.name = strdup ("Installed Packages");
	repo->super.enabled = true;

	rpmReadConfigFiles (NULL, NULL);
	if (rpmdbOpen ("", &repo->db, O_RDONLY, 0644) != 0) {
		fprintf (stderr, "Cannot open rpm database\n");
		exit (1);
	}

	repo->table = NULL;

	return (LowRepo *) repo;
}

void
low_repo_rpmdb_shutdown (LowRepo *repo)
{
	LowRepoRpmdb *repo_rpmdb = (LowRepoRpmdb *) repo;

	rpmdbClose (repo_rpmdb->db);
	rpmFreeRpmrc ();

	free (repo->id);
	free (repo->name);

	if (repo_rpmdb->table) {
		g_hash_table_destroy (repo_rpmdb->table);
	}

	free (repo);
}

static void
low_package_iter_rpmdb_free (LowPackageIter *iter)
{
	LowPackageIterRpmdb *iter_rpmdb = (LowPackageIterRpmdb *) iter;

	rpmdbFreeIterator (iter_rpmdb->rpm_iter);

	if (iter_rpmdb->filter_data_free_func) {
		gpointer data = iter_rpmdb->filter_data;
		iter_rpmdb->filter_data_free_func (data);
	}

	free (iter);
}

static uint
id_hash_func (gconstpointer key)
{
	return *((uint *) key);
}

static bool
id_equal_func (gconstpointer key1, gconstpointer key2)
{
	if (!strncmp (key1, key2, 16)) {
		return true;
	} else {
		return false;
	}
}

static LowPackage *
low_package_rpmdb_new_from_header (Header header, LowRepo *repo)
{
	LowPackage *pkg;
	rpmtd name;
	rpmtd id;

	rpmtd version;
	rpmtd release;
	rpmtd epoch;
	rpmtd arch;
	rpmtd size;

	LowRepoRpmdb *repo_rpmdb = (LowRepoRpmdb *) repo;

	if (!repo_rpmdb->table) {
		low_debug ("initializing hash table\n");
		repo_rpmdb->table =
			g_hash_table_new_full (id_hash_func,
					       (GEqualFunc) id_equal_func,
					       NULL, (GDestroyNotify)
					       low_package_unref);
	}

	name = rpmtdNew ();

	headerGet (header, RPMTAG_NAME, name, HEADERGET_MINMEM);

	/* We don't care about the gpg keys (plus they have missing fields */
	if (!strcmp (name->data, "gpg-pubkey")) {
		rpmtdFreeData (name);
		rpmtdFree (name);

		return NULL;
	}

	id = rpmtdNew ();

	headerGet (header, RPMTAG_PKGID, id, HEADERGET_MINMEM);

	pkg = g_hash_table_lookup (repo_rpmdb->table, id->data);
	if (pkg) {
		rpmtdFreeData (name);
		rpmtdFreeData (id);

		rpmtdFree (name);
		rpmtdFree (id);

		low_package_ref (pkg);
		return pkg;
	}
	low_debug ("CACHE MISS - %s", (char *) name->data);

	version = rpmtdNew ();
	release = rpmtdNew ();
	epoch = rpmtdNew ();
	arch = rpmtdNew ();
	size = rpmtdNew ();

	headerGet (header, RPMTAG_EPOCH, epoch, HEADERGET_MINMEM);
	headerGet (header, RPMTAG_VERSION, version, HEADERGET_MINMEM);
	headerGet (header, RPMTAG_RELEASE, release, HEADERGET_MINMEM);
	headerGet (header, RPMTAG_ARCH, arch, HEADERGET_MINMEM);

	headerGet (header, RPMTAG_SIZE, size, HEADERGET_MINMEM);

	pkg = malloc (sizeof (LowPackage));

	pkg->id = malloc (sizeof (char) * 16);
	pkg->id = memcpy (pkg->id, id->data, 16);

	g_hash_table_insert (repo_rpmdb->table, pkg->id, pkg);
	low_package_ref_init (pkg);
	low_package_ref (pkg);

	pkg->name = strdup (name->data);

	pkg->epoch = NULL;
	if (epoch->type != RPM_NULL_TYPE) {
		pkg->epoch = g_strdup_printf ("%lu", rpmtdGetNumber (epoch));
	}

	pkg->version = strdup (version->data);
	pkg->release = strdup (release->data);
	pkg->arch = low_arch_from_str (arch->data);

	pkg->size = rpmtdGetNumber (size);
	pkg->repo = repo;

	/* installed packages can't be downloaded. */
	pkg->location_href = NULL;
	pkg->digest = NULL;
	pkg->digest_type = DIGEST_NONE;

	pkg->provides = NULL;
	pkg->requires = NULL;
	pkg->conflicts = NULL;
	pkg->obsoletes = NULL;

	pkg->get_details = low_rpmdb_package_get_details;

	pkg->get_provides = low_rpmdb_package_get_provides;
	pkg->get_requires = low_rpmdb_package_get_requires;
	pkg->get_conflicts = low_rpmdb_package_get_conflicts;
	pkg->get_obsoletes = low_rpmdb_package_get_obsoletes;

	pkg->get_files = low_rpmdb_package_get_files;

	rpmtdFreeData (id);
	rpmtdFreeData (name);
	rpmtdFreeData (version);
	rpmtdFreeData (release);
	rpmtdFreeData (epoch);
	rpmtdFreeData (arch);
	rpmtdFreeData (size);

	rpmtdFree (id);
	rpmtdFree (name);
	rpmtdFree (version);
	rpmtdFree (release);
	rpmtdFree (epoch);
	rpmtdFree (arch);
	rpmtdFree (size);

	return pkg;
}

static LowPackageIter *
low_package_iter_rpmdb_next (LowPackageIter *iter)
{
	LowPackageIterRpmdb *iter_rpmdb = (LowPackageIterRpmdb *) iter;
	Header header = rpmdbNextIterator (iter_rpmdb->rpm_iter);

	if (header == NULL) {
		low_package_iter_rpmdb_free (iter);
		return NULL;
	}

	iter->pkg = low_package_rpmdb_new_from_header (header, iter->repo);

	/* Ignore the gpg-pubkeys */
	while (iter->pkg == NULL && header) {
		header = rpmdbNextIterator (iter_rpmdb->rpm_iter);
		iter->pkg = low_package_rpmdb_new_from_header (header,
							       iter->repo);
	}
	if (iter_rpmdb->func != NULL) {
		/* move on to the next rpm if this one fails the filter */
		if (!iter_rpmdb->func (iter->pkg, iter_rpmdb->filter_data)) {
			low_package_unref (iter->pkg);
			return low_package_iter_next (iter);
		}
	}

	return iter;
}

static LowPackageIter *
low_repo_rpmdb_search (LowRepo *repo, int32_t tag, const char *querystr)
{
	LowRepoRpmdb *repo_rpmdb = (LowRepoRpmdb *) repo;
	LowPackageIterRpmdb *iter = malloc (sizeof (LowPackageIterRpmdb));
	iter->super.repo = repo;
	iter->super.next_func = low_package_iter_rpmdb_next;
	iter->super.free_func = low_package_iter_rpmdb_free;
	iter->super.pkg = NULL;

	iter->func = NULL;
	iter->filter_data_free_func = NULL;

	iter->rpm_iter = rpmdbInitIterator (repo_rpmdb->db, tag, querystr, 0);
	return (LowPackageIter *) iter;
}

LowPackageIter *
low_repo_rpmdb_list_all (LowRepo *repo)
{
	return low_repo_rpmdb_search (repo, 0, NULL);
}

LowPackageIter *
low_repo_rpmdb_list_by_name (LowRepo *repo, const char *name)
{
	return low_repo_rpmdb_search (repo, RPMTAG_NAME, name);
}

typedef struct _DepFilterData {
	LowPackageDependency *dep;
	LowPackageGetDependency dep_func;
} DepFilterData;

static void
dep_filter_data_free_fn (gpointer data)
{
	DepFilterData *filter_data = (DepFilterData *) data;
	low_package_dependency_free (filter_data->dep);
	free (filter_data);
}

static bool
low_repo_rpmdb_search_dep_filter_fn (LowPackage *pkg, gpointer data)
{
	DepFilterData *filter_data = (DepFilterData *) data;
	bool res = false;
	LowPackageDependency **deps = filter_data->dep_func (pkg);
	int i;

	for (i = 0; deps[i] != NULL; i++) {
		if (low_package_dependency_satisfies (filter_data->dep,
						      deps[i])) {
			res = true;
			break;
		}
	}

	return res;
}

static LowPackageIter *
low_repo_rpmdb_search_dep (LowRepo *repo, int32_t tag,
			   const LowPackageDependency *dep,
			   LowPackageGetDependency dep_func)
{
	DepFilterData *data = malloc (sizeof (DepFilterData));
	LowRepoRpmdb *repo_rpmdb = (LowRepoRpmdb *) repo;
	LowPackageIterRpmdb *iter = malloc (sizeof (LowPackageIterRpmdb));
	iter->super.repo = repo;
	iter->super.next_func = low_package_iter_rpmdb_next;
	iter->super.free_func = low_package_iter_rpmdb_free;
	iter->super.pkg = NULL;

	iter->func = low_repo_rpmdb_search_dep_filter_fn;
	iter->filter_data_free_func = dep_filter_data_free_fn;
	iter->filter_data = (gpointer) data;

	data->dep = low_package_dependency_new (dep->name, dep->sense,
						dep->evr);
	data->dep_func = dep_func;

	iter->rpm_iter = rpmdbInitIterator (repo_rpmdb->db, tag, dep->name, 0);
	return (LowPackageIter *) iter;
}

LowPackageIter *
low_repo_rpmdb_search_provides (LowRepo *repo,
				const LowPackageDependency *provides)
{
	return low_repo_rpmdb_search_dep (repo, RPMTAG_PROVIDENAME, provides,
					  low_package_get_provides);
}

LowPackageIter *
low_repo_rpmdb_search_requires (LowRepo *repo,
				const LowPackageDependency *requires)
{
	return low_repo_rpmdb_search_dep (repo, RPMTAG_REQUIRENAME, requires,
					  low_package_get_requires);
}

LowPackageIter *
low_repo_rpmdb_search_conflicts (LowRepo *repo,
				 const LowPackageDependency *conflicts)
{
	return low_repo_rpmdb_search_dep (repo, RPMTAG_CONFLICTNAME, conflicts,
					  low_package_get_conflicts);
}

LowPackageIter *
low_repo_rpmdb_search_obsoletes (LowRepo *repo,
				 const LowPackageDependency *obsoletes)
{
	/* XXX This seems to be broken in RPM itself. */
	return low_repo_rpmdb_search_dep (repo, RPMTAG_OBSOLETENAME, obsoletes,
					  low_package_get_obsoletes);
}

LowPackageIter *
low_repo_rpmdb_search_files (LowRepo *repo, const char *file)
{
	return low_repo_rpmdb_search (repo, RPMTAG_BASENAMES, file);
}

static bool
low_repo_rpmdb_search_details_filter_fn (LowPackage *pkg, gpointer data)
{
	char *querystr = (char *) data;

	/* url can be NULL, so check first. */
	if (strstr (pkg->name, querystr)) {
		return true;
	} else {
		bool res;
		LowPackageDetails *details = low_package_get_details (pkg);

		if (strstr (details->summary, querystr) ||
		    strstr (details->description, querystr) ||
		    (details->url != NULL && strstr (details->url, querystr))) {
			res = true;
		} else {
			res = false;
		}

		low_package_details_free (details);

		return res;
	}
}

LowPackageIter *
low_repo_rpmdb_search_details (LowRepo *repo, const char *querystr)
{
	LowRepoRpmdb *repo_rpmdb = (LowRepoRpmdb *) repo;
	LowPackageIterRpmdb *iter = malloc (sizeof (LowPackageIterRpmdb));
	iter->super.repo = repo;
	iter->super.next_func = low_package_iter_rpmdb_next;
	iter->super.free_func = low_package_iter_rpmdb_free;
	iter->super.pkg = NULL;

	iter->func = low_repo_rpmdb_search_details_filter_fn;
	iter->filter_data_free_func = NULL;
	iter->filter_data = (gpointer) querystr;

	iter->rpm_iter = rpmdbInitIterator (repo_rpmdb->db, 0, NULL, 0);
	return (LowPackageIter *) iter;
}

union rpm_entry {
	void *p;
	char *string;
	char **list;
	uint32_t *int_list;
	uint32_t *flags;
	uint32_t *integer;
};

static LowPackageDependencySense
rpm_to_low_dependency_sense (uint32_t flag)
{
	switch (flag & (RPMSENSE_LESS | RPMSENSE_EQUAL | RPMSENSE_GREATER)) {
		case RPMSENSE_LESS:
			return DEPENDENCY_SENSE_LT;
		case RPMSENSE_LESS | RPMSENSE_EQUAL:
			return DEPENDENCY_SENSE_LE;
		case RPMSENSE_EQUAL:
			return DEPENDENCY_SENSE_EQ;
		case RPMSENSE_GREATER | RPMSENSE_EQUAL:
			return DEPENDENCY_SENSE_GE;
		case RPMSENSE_GREATER:
			return DEPENDENCY_SENSE_GT;
		default:
			return DEPENDENCY_SENSE_NONE;
	}

	return DEPENDENCY_SENSE_NONE;
}

static LowPackageDependency **
low_repo_rpmdb_get_deps (LowRepo *repo, LowPackage *pkg, uint32_t name_tag,
			 uint32_t flag_tag, uint32_t version_tag)
{
	LowRepoRpmdb *repo_rpmdb = (LowRepoRpmdb *) repo;
	rpmdbMatchIterator iter;
	Header header;
	LowPackageDependency **deps;
	rpmtd name = rpmtdNew ();
	rpmtd flag = rpmtdNew ();
	rpmtd version = rpmtdNew ();
	uint i;

	char **names;
	int *flags;
	char **versions;

	iter = rpmdbInitIterator (repo_rpmdb->db, RPMTAG_PKGID, pkg->id, 16);
	header = rpmdbNextIterator (iter);

	headerGet (header, name_tag, name, HEADERGET_MINMEM);
	headerGet (header, flag_tag, flag, HEADERGET_MINMEM);
	headerGet (header, version_tag, version, HEADERGET_MINMEM);

	names = name->data;
	flags = flag->data;
	versions = version->data;

	deps = malloc (sizeof (char *) * (name->count + 1));
	for (i = 0; i < name->count; i++) {
		LowPackageDependencySense sense =
			rpm_to_low_dependency_sense (flags[i]);
		deps[i] = low_package_dependency_new (names[i], sense,
						      versions[i]);
	}
	deps[name->count] = NULL;

	rpmtdFreeData (name);
	rpmtdFreeData (flag);
	rpmtdFreeData (version);

	rpmtdFree (name);
	rpmtdFree (flag);
	rpmtdFree (version);

	rpmdbFreeIterator (iter);

	return deps;
}

LowPackageDetails *
low_rpmdb_package_get_details (LowPackage *pkg)
{
	LowRepoRpmdb *repo_rpmdb = (LowRepoRpmdb *) pkg->repo;
	rpmdbMatchIterator iter;
	Header header;
	LowPackageDetails *details = malloc (sizeof (LowPackageDetails));

	rpmtd summary = rpmtdNew ();
	rpmtd description = rpmtdNew ();
	rpmtd url = rpmtdNew ();
	rpmtd license = rpmtdNew ();

	iter = rpmdbInitIterator (repo_rpmdb->db, RPMTAG_PKGID, pkg->id, 16);
	header = rpmdbNextIterator (iter);

	headerGet (header, RPMTAG_SUMMARY, summary, HEADERGET_MINMEM);
	headerGet (header, RPMTAG_DESCRIPTION, description, HEADERGET_MINMEM);
	headerGet (header, RPMTAG_URL, url, HEADERGET_MINMEM);
	headerGet (header, RPMTAG_LICENSE, license, HEADERGET_MINMEM);

	details->summary = strdup (summary->data);
	details->description = strdup (description->data);
	details->url = url->data ? strdup (url->data) : NULL;
	details->license = strdup (license->data);

	rpmtdFreeData (summary);
	rpmtdFreeData (description);
	rpmtdFreeData (url);
	rpmtdFreeData (license);

	rpmtdFree (summary);
	rpmtdFree (description);
	rpmtdFree (url);
	rpmtdFree (license);

	rpmdbFreeIterator (iter);

	return details;
}

LowPackageDependency **
low_rpmdb_package_get_provides (LowPackage *pkg)
{
	if (!pkg->provides) {
		pkg->provides = low_repo_rpmdb_get_deps (pkg->repo, pkg,
							 RPMTAG_PROVIDENAME,
							 RPMTAG_PROVIDEFLAGS,
							 RPMTAG_PROVIDEVERSION);
	}
	return pkg->provides;
}

LowPackageDependency **
low_rpmdb_package_get_requires (LowPackage *pkg)
{
	if (!pkg->requires) {
		pkg->requires = low_repo_rpmdb_get_deps (pkg->repo, pkg,
							 RPMTAG_REQUIRENAME,
							 RPMTAG_REQUIREFLAGS,
							 RPMTAG_REQUIREVERSION);
	}

	return pkg->requires;
}

LowPackageDependency **
low_rpmdb_package_get_conflicts (LowPackage *pkg)
{
	if (!pkg->conflicts) {
		pkg->conflicts = low_repo_rpmdb_get_deps (pkg->repo, pkg,
							  RPMTAG_CONFLICTNAME,
							  RPMTAG_CONFLICTFLAGS,
							  RPMTAG_CONFLICTVERSION);
	}

	return pkg->conflicts;
}

LowPackageDependency **
low_rpmdb_package_get_obsoletes (LowPackage *pkg)
{
	if (!pkg->obsoletes) {
		pkg->obsoletes = low_repo_rpmdb_get_deps (pkg->repo, pkg,
							  RPMTAG_OBSOLETENAME,
							  RPMTAG_OBSOLETEFLAGS,
							  RPMTAG_OBSOLETEVERSION);
	}

	return pkg->obsoletes;
}

char **
low_rpmdb_package_get_files (LowPackage *pkg)
{
	LowRepoRpmdb *repo_rpmdb = (LowRepoRpmdb *) pkg->repo;
	rpmdbMatchIterator iter;
	Header header;
	char **files;
	rpmtd indexes = rpmtdNew ();
	rpmtd dir = rpmtdNew ();
	rpmtd name = rpmtdNew ();
	uint i;

	int *dir_index;
	char **dir_list;
	char **name_list;

	iter = rpmdbInitIterator (repo_rpmdb->db, RPMTAG_PKGID, pkg->id, 16);
	header = rpmdbNextIterator (iter);

	headerGet (header, RPMTAG_DIRINDEXES, indexes, HEADERGET_MINMEM);
	headerGet (header, RPMTAG_DIRNAMES, dir, HEADERGET_MINMEM);
	headerGet (header, RPMTAG_BASENAMES, name, HEADERGET_MINMEM);

	dir_index = indexes->data;
	dir_list = dir->data;
	name_list = name->data;

	files = malloc (sizeof (char *) * (name->count + 1));
	for (i = 0; i < name->count; i++) {
		files[i] = g_strdup_printf ("%s%s", dir_list[dir_index[i]],
					    name_list[i]);
	}
	files[name->count] = NULL;

	rpmtdFreeData (indexes);
	rpmtdFreeData (dir);
	rpmtdFreeData (name);

	rpmtdFree (indexes);
	rpmtdFree (dir);
	rpmtdFree (name);

	rpmdbFreeIterator (iter);

	return files;
}

/* XXX hack */
rpmdb
low_repo_rpmdb_get_db (LowRepo *repo)
{
	LowRepoRpmdb *repo_rpmdb = (LowRepoRpmdb *) repo;
	return repo_rpmdb->db;
}

/* vim: set ts=8 sw=8 noet: */
