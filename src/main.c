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

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include <bzlib.h>
#include <glib.h>
#include <rpm/rpmdb.h>
#include <zlib.h>

#include "config.h"

#include "low-arch.h"
#include "low-debug.h"
#include "low-config.h"
#include "low-package.h"
#include "low-repo-rpmdb.h"
#include "low-repo-set.h"
#include "low-repo-sqlite.h"
#include "low-repomd-parser.h"
#include "low-repoxml-parser.h"
#include "low-transaction.h"
#include "low-util.h"
#include "low-download.h"
#include "low-mirror-list.h"
#include "low-delta-parser.h"
#include "low-parse-options.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define LOCAL_CACHE "/var/cache/yum"

static void show_help (const char *command);
static int usage (void);

static void
print_size (size_t size)
{
	float tmp_size = size;
	if (tmp_size < 1023) {
		printf ("%.0f bytes\n", tmp_size);
		return;
	}

	tmp_size = tmp_size / 1024;
	if (tmp_size < 1023) {
		printf ("%.1f KB\n", tmp_size);
		return;
	}

	tmp_size = tmp_size / 1024;
	if (tmp_size < 1023) {
		printf ("%.1f MB\n", tmp_size);
		return;
	}

	tmp_size = tmp_size / 1024;
	printf ("%.1f GB\n", tmp_size);
	return;
}

static void
wrap_and_print (const char *text)
{
	int i;
	char **wrapped = low_util_word_wrap (text, 79 - 14);

	if (wrapped[0] != NULL) {
		puts (wrapped[0]);
	}

	for (i = 1; wrapped[i] != NULL; i++) {
		printf ("              %s\n", wrapped[i]);
	}

	g_strfreev (wrapped);
}

static void
print_dependency (const LowPackageDependency *dep)
{
	fputs (dep->name, stdout);
	if (dep->sense != DEPENDENCY_SENSE_NONE) {
		switch (dep->sense) {
			case DEPENDENCY_SENSE_EQ:
				fputs (" = ", stdout);
				break;
			case DEPENDENCY_SENSE_LT:
				fputs (" < ", stdout);
				break;
			case DEPENDENCY_SENSE_LE:
				fputs (" <= ", stdout);
				break;
			case DEPENDENCY_SENSE_GT:
				fputs (" > ", stdout);
				break;
			case DEPENDENCY_SENSE_GE:
				fputs (" >= ", stdout);
				break;
			case DEPENDENCY_SENSE_NONE:
			default:
				break;
		}

		fputs (dep->evr, stdout);
	}
	putchar ('\n');
}

static void
print_dependencies (const char *dep_name, LowPackageDependency **deps)
{
	int i;

	printf ("%-12s:", dep_name);

	if (deps[0] == NULL) {
		putchar ('\n');
		return;
	}

	putchar (' ');
	print_dependency (deps[0]);
	for (i = 1; deps[i] != NULL; i++) {
		fputs ("              ", stdout);
		print_dependency (deps[i]);
	}
}

static void
print_files (char **files)
{
	int i;

	printf ("Files       :");

	if (files[0] == NULL) {
		printf ("\n");
		return;
	}

	printf (" %s\n", files[0]);
	for (i = 1; files[i] != NULL; i++) {
		printf ("              %s\n", files[i]);
	}
}

static const char *
digest_type_to_string (LowDigestType type)
{
	switch (type) {
		case DIGEST_MD5:
			return "MD5";
		case DIGEST_SHA1:
			return "SHA1";
		case DIGEST_SHA256:
			return "SHA256";
		case DIGEST_NONE:
			return "NONE";
		case DIGEST_UNKNOWN:
		default:
			return "UNKNOWN";
	}
}

static void
print_package (LowPackage *pkg, bool show_all)
{
	LowPackageDetails *details = low_package_get_details (pkg);

	printf ("Name        : %s\n", pkg->name);
	printf ("Arch        : %s\n", low_arch_to_str (pkg->arch));
	printf ("Version     : %s\n", pkg->version);
	printf ("Release     : %s\n", pkg->release);

	printf ("Size        : ");
	print_size (pkg->size);

	printf ("Repo        : %s\n", pkg->repo->id);

	printf ("Summary     : ");
	wrap_and_print (details->summary);

	printf ("URL         : %s\n", details->url ? details->url : "");
	printf ("License     : %s\n", details->license);

	printf ("Description : ");
	wrap_and_print (details->description);

	low_package_details_free (details);

	if (show_all) {
		LowPackageDependency **deps;
		char **files;

		if (pkg->digest != NULL) {
			printf ("Digest Type : %s\n",
				digest_type_to_string (pkg->digest_type));
			printf ("Digest      : %s\n", pkg->digest);
		}

		deps = low_package_get_provides (pkg);
		print_dependencies ("Provides", deps);

		deps = low_package_get_requires (pkg);
		print_dependencies ("Requires", deps);

		deps = low_package_get_conflicts (pkg);
		print_dependencies ("Conflicts", deps);
		low_package_dependency_list_free (deps);

		deps = low_package_get_obsoletes (pkg);
		print_dependencies ("Obsoletes", deps);
		low_package_dependency_list_free (deps);

		files = low_package_get_files (pkg);
		print_files (files);
		g_strfreev (files);

	}

	printf ("\n");
}

static void
print_all_packages (LowPackageIter *iter, bool show_all)
{
	while (iter = low_package_iter_next (iter), iter != NULL) {
		LowPackage *pkg = iter->pkg;
		print_package (pkg, show_all);

		low_package_unref (pkg);
	}
}

static bool
initialize_repos (LowRepo **repo_rpmdb, LowRepoSet **repos)
{
	LowConfig *config;

	*repo_rpmdb = low_repo_rpmdb_initialize ();
	config = low_config_initialize (*repo_rpmdb);

	*repos = low_repo_set_initialize_from_config (config, true);

	low_config_free (config);

	if (!repos) {
		low_repo_rpmdb_shutdown (*repo_rpmdb);
		return false;
	}

	return true;
}

bool show_all = false;

LowOption info_options[] = {
	{OPTION_BOOL, 'a', "all", &show_all, NULL,
		"Show all package info"},
	LOW_OPTION_END
};

static int
command_info (int argc G_GNUC_UNUSED, const char *argv[])
{
	LowRepo *repo_rpmdb;
	LowRepoSet *repos;
	LowPackageIter *iter;

	if (!initialize_repos (&repo_rpmdb, &repos)) {
		return EXIT_FAILURE;
	}

	iter = low_repo_rpmdb_list_by_name (repo_rpmdb, argv[0]);
	print_all_packages (iter, show_all);

	iter = low_repo_set_list_by_name (repos, argv[0]);
	print_all_packages (iter, show_all);

	low_repo_set_free (repos);
	low_repo_rpmdb_shutdown (repo_rpmdb);

	return EXIT_SUCCESS;
}

static void
print_package_short (LowPackage *pkg)
{
	char *name_arch = g_strdup_printf ("%s.%s", pkg->name,
					   low_arch_to_str (pkg->arch));
	char *version_release = g_strdup_printf ("%s-%s", pkg->version,
						 pkg->release);

	printf ("%-41.41s %-23.23s %s\n", name_arch, version_release,
		pkg->repo->id);

	free (name_arch);
	free (version_release);
}

static void
print_all_packages_short (LowPackageIter *iter)
{
	while (iter = low_package_iter_next (iter), iter != NULL) {
		LowPackage *pkg = iter->pkg;
		print_package_short (pkg);

		low_package_unref (pkg);
	}
}

static int
package_compare_fn (gconstpointer data1, gconstpointer data2)
{
	LowTransactionMember *member1 = (LowTransactionMember *) data1;
	LowTransactionMember *member2 = (LowTransactionMember *) data2;

	return strcmp (member1->pkg->name, member2->pkg->name);
}

static void
print_transaction_part (GHashTable *hash)
{
	GList *list = g_hash_table_get_values (hash);
	list = g_list_sort (list, package_compare_fn);
	while (list != NULL) {
		LowTransactionMember *member = list->data;
		print_package_short (member->pkg);
		list = list->next;
	}
}

static void
compute_updates (LowTransaction *trans, LowRepo *repo_rpmdb)
{
	int i = 0;
	char spinner[] = { '-', '\\', '|', '/' };
	LowPackageIter *iter = low_repo_rpmdb_list_all (repo_rpmdb);

	while (iter = low_package_iter_next (iter), iter != NULL) {
		low_transaction_add_update (trans, iter->pkg);

		if (i % 100 == 0) {
			printf ("\rComputing updates... %c",
				spinner[i / 100 % 4]);
			fflush (stdout);
		}
		i++;
	}
	printf ("\rComputing updates... Done\n");
}

static void
transaction_callback (int progress, gpointer data)
{
	int counter = (*((int *) data))++;
	char spinner[] = { '-', '\\', '|', '/' };

	if (progress == -1) {
		printf ("\rResolving transaction... Done\n");
	} else {
		printf ("\rResolving Transaction... %c", spinner[counter % 4]);
		fflush (stdout);
	}
}

static int
print_updates (LowRepo *repo_rpmdb, LowConfig *config)
{
	LowRepoSet *repos;
	LowTransaction *trans;
	int counter = 0;
	bool found_updates = false;

	repos = low_repo_set_initialize_from_config (config, true);

	trans = low_transaction_new (repo_rpmdb, repos, transaction_callback,
				     &counter);

	compute_updates (trans, repo_rpmdb);

	if (g_hash_table_size (trans->update) != 0) {
		print_transaction_part (trans->update);
		found_updates = true;
	}

	/* XXX need to sort this in with updates for printing */
	/* For installonly packages. ie the kernel */
	if (g_hash_table_size (trans->install) != 0) {
		print_transaction_part (trans->install);
		found_updates = true;
	}

	if (!found_updates) {
		printf ("No updates available.\n");
	}

	low_transaction_free (trans);
	low_repo_set_free (repos);
	low_config_free (config);
	low_repo_rpmdb_shutdown (repo_rpmdb);

	return EXIT_SUCCESS;
}

static int
command_list (int argc, const char *argv[])
{
	LowRepo *repo_rpmdb;
	LowPackageIter *iter;
	LowConfig *config;

	repo_rpmdb = low_repo_rpmdb_initialize ();
	config = low_config_initialize (repo_rpmdb);

	if (argc == 1 && strcmp (argv[0], "updates") == 0) {
		return print_updates (repo_rpmdb, config);
	}

	if (argc == 0 || !strcmp (argv[0], "installed") ||
	    !strcmp (argv[0], "all")) {
		iter = low_repo_rpmdb_list_all (repo_rpmdb);
		print_all_packages_short (iter);
	}
	if (argc == 0 || !strcmp (argv[0], "all") ||
	    !strcmp (argv[0], "available")) {
		LowRepoSet *repos =
			low_repo_set_initialize_from_config (config, true);

		iter = low_repo_set_list_all (repos);
		print_all_packages_short (iter);

		low_repo_set_free (repos);
	} else {
		LowRepoSet *repos;

		iter = low_repo_rpmdb_list_by_name (repo_rpmdb, argv[0]);
		print_all_packages_short (iter);

		repos = low_repo_set_initialize_from_config (config, true);

		iter = low_repo_set_list_by_name (repos, argv[0]);
		print_all_packages_short (iter);

		low_repo_set_free (repos);
	}

	low_config_free (config);
	low_repo_rpmdb_shutdown (repo_rpmdb);

	return EXIT_SUCCESS;
}

static int
command_search (int argc G_GNUC_UNUSED, const char *argv[])
{
	LowRepo *repo_rpmdb;
	LowRepoSet *repos;
	LowPackageIter *iter;
	const char *querystr = argv[0];

	if (!initialize_repos (&repo_rpmdb, &repos)) {
		return EXIT_FAILURE;
	}

	iter = low_repo_rpmdb_search_details (repo_rpmdb, querystr);
	print_all_packages_short (iter);

	iter = low_repo_set_search_details (repos, querystr);
	print_all_packages_short (iter);

	low_repo_set_free (repos);
	low_repo_rpmdb_shutdown (repo_rpmdb);

	return EXIT_SUCCESS;
}

#define FORMAT_STRING "%-30.30s  %-35.35s  %s\n"

static void
print_repo (LowRepo *repo)
{
	printf (FORMAT_STRING, repo->id, repo->name,
		repo->enabled ? "enabled" : "disabled");
}

static int
command_repolist (int argc, const char *argv[])
{
	LowRepo *repo_rpmdb;
	LowRepoSet *repos;
	LowRepoSetFilter filter = ALL;

	if (!initialize_repos (&repo_rpmdb, &repos)) {
		return EXIT_FAILURE;
	}

	if (argc == 1) {
		if (!strcmp (argv[0], "all")) {
			filter = ALL;
		} else if (!strcmp (argv[0], "enabled")) {
			filter = ENABLED;
		} else if (!strcmp (argv[0], "disabled")) {
			filter = DISABLED;
		} else {
			printf ("Unknown repo type: %s\n", argv[0]);
			exit (1);
		}
	}

	printf (FORMAT_STRING, "repo id", "repo name", "status");

	print_repo (repo_rpmdb);

	low_repo_set_for_each (repos, filter, print_repo);

	low_repo_set_free (repos);
	low_repo_rpmdb_shutdown (repo_rpmdb);

	return EXIT_SUCCESS;
}

static int
command_whatprovides (int argc G_GNUC_UNUSED, const char *argv[])
{
	LowRepo *repo_rpmdb;
	LowRepoSet *repos;
	LowPackageIter *iter;
	LowPackageDependency *provides =
		low_package_dependency_new_from_string (argv[0]);

	if (!initialize_repos (&repo_rpmdb, &repos)) {
		return EXIT_FAILURE;
	}

	iter = low_repo_rpmdb_search_provides (repo_rpmdb, provides);
	print_all_packages_short (iter);

	if (provides->name[0] == '/') {
		iter = low_repo_rpmdb_search_files (repo_rpmdb, provides->name);
		print_all_packages_short (iter);
	}

	iter = low_repo_set_search_provides (repos, provides);
	print_all_packages_short (iter);

	if (provides->name[0] == '/') {
		iter = low_repo_set_search_files (repos, provides->name);
		print_all_packages_short (iter);
	}

	low_repo_set_free (repos);
	low_repo_rpmdb_shutdown (repo_rpmdb);
	low_package_dependency_free (provides);

	return EXIT_SUCCESS;
}

static int
command_whatrequires (int argc G_GNUC_UNUSED, const char *argv[])
{
	LowRepo *repo_rpmdb;
	LowRepoSet *repos;
	LowPackageIter *iter;
	LowPackageDependency *requires =
		low_package_dependency_new_from_string (argv[0]);

	if (!initialize_repos (&repo_rpmdb, &repos)) {
		return EXIT_FAILURE;
	}

	iter = low_repo_rpmdb_search_requires (repo_rpmdb, requires);
	print_all_packages_short (iter);

	iter = low_repo_set_search_requires (repos, requires);
	print_all_packages_short (iter);

	low_repo_set_free (repos);
	low_repo_rpmdb_shutdown (repo_rpmdb);
	low_package_dependency_free (requires);

	return EXIT_SUCCESS;
}

static int
command_whatconflicts (int argc G_GNUC_UNUSED, const char *argv[])
{
	LowRepo *repo_rpmdb;
	LowRepoSet *repos;
	LowPackageIter *iter;
	LowPackageDependency *conflicts =
		low_package_dependency_new_from_string (argv[0]);

	if (!initialize_repos (&repo_rpmdb, &repos)) {
		return EXIT_FAILURE;
	}

	iter = low_repo_rpmdb_search_conflicts (repo_rpmdb, conflicts);
	print_all_packages_short (iter);

	iter = low_repo_set_search_conflicts (repos, conflicts);
	print_all_packages_short (iter);

	low_repo_set_free (repos);
	low_repo_rpmdb_shutdown (repo_rpmdb);
	low_package_dependency_free (conflicts);

	return EXIT_SUCCESS;
}

static int
command_whatobsoletes (int argc G_GNUC_UNUSED, const char *argv[])
{
	LowRepo *repo_rpmdb;
	LowRepoSet *repos;
	LowPackageIter *iter;
	LowPackageDependency *obsoletes =
		low_package_dependency_new_from_string (argv[0]);

	if (!initialize_repos (&repo_rpmdb, &repos)) {
		return EXIT_FAILURE;
	}

	iter = low_repo_rpmdb_search_obsoletes (repo_rpmdb, obsoletes);
	print_all_packages_short (iter);

	iter = low_repo_set_search_obsoletes (repos, obsoletes);
	print_all_packages_short (iter);

	low_repo_set_free (repos);
	low_repo_rpmdb_shutdown (repo_rpmdb);
	low_package_dependency_free (obsoletes);

	return EXIT_SUCCESS;
}

static const char *
get_file_basename (const char *location_href)
{
	const char *filename = rindex (location_href, '/');
	if (filename == NULL) {
		filename = location_href;
	} else {
		filename++;
	}

	return filename;
}

static char *
create_package_filepath (LowPackage *pkg)
{
	const char *filename = get_file_basename (pkg->location_href);
	char *local_file = g_strdup_printf ("%s/%s/packages/%s",
					    LOCAL_CACHE, pkg->repo->id,
					    filename);

	return local_file;
}

static int
digit_count (int num)
{
	int i = 1;

	while ((num /= 10) > 0) {
		i++;
	}

	return i;
}

#define TERM_WIDTH 80

static void
print_file (const char *file, uint size_chars)
{
	if (strlen (file) + size_chars + 12 > TERM_WIDTH) {
		printf ("%.*s...", TERM_WIDTH - (size_chars + 12 + 3), file);
	} else {
		printf ("%-*s", TERM_WIDTH - (size_chars + 12), file);
	}
}

static int
download_callback (void *clientp, double dltotal, double dlnow,
		   double ultotal G_GNUC_UNUSED, double ulnow G_GNUC_UNUSED)
{
	const char *file = clientp;

	float tmp_now = dlnow;
	float tmp_total = dltotal;

	uint digits;

	if (dlnow > dltotal || dltotal == 0) {
		return 0;
	}

	fputs ("\rdownloading ", stdout);

	if (tmp_total < 1023) {
		digits = digit_count (tmp_total);
		print_file (file, 2 * digits + 4);
		printf (" %*.0fB/%.0fB", digits, tmp_now, tmp_total);
		fflush (stdout);
		return 0;
	}

	tmp_now /= 1024;
	tmp_total /= 1024;
	if (tmp_total < 1023) {
		digits = digit_count (tmp_total) + 2;
		print_file (file, 2 * digits + 6);
		printf (" %*.1fKB/%.1fKB", digits, tmp_now, tmp_total);
		fflush (stdout);
		return 0;
	}

	tmp_now /= 1024;
	tmp_total /= 1024;
	if (tmp_total < 1023) {
		digits = digit_count (tmp_total) + 2;
		print_file (file, 2 * digits + 6);
		printf (" %*.1fMB/%.1fMB", digits, tmp_now, tmp_total);
		fflush (stdout);
		return 0;
	}

	tmp_now /= 1024;
	tmp_total /= 1024;
	digits = digit_count (tmp_total) + 2;
	print_file (file, 2 * digits + 6);
	printf (" %*.1fGB/%.1fGB", digits, tmp_now, tmp_total);
	fflush (stdout);
	return 0;
}

static bool
download_package (LowPackage *pkg)
{
	int res;
	LowMirrorList *mirrors = low_repo_sqlite_get_mirror_list (pkg->repo);

	char *local_file = create_package_filepath (pkg);
	const char *filename = get_file_basename (pkg->location_href);
	char *dirname = g_strdup_printf ("%s/%s/packages", LOCAL_CACHE,
					 pkg->repo->id);

	if (!g_file_test (dirname, G_FILE_TEST_EXISTS)) {
		mkdir (dirname, 0755);
	}
	free (dirname);

	res = low_download_if_missing (mirrors, pkg->location_href, local_file,
				       filename, pkg->digest, pkg->digest_type,
				       pkg->size, download_callback);
	free (local_file);

	return res == 0;
}

static char *
create_delta_filepath (LowRepo *repo, LowPackageDelta *pkg_delta)
{
	const char *filename = get_file_basename (pkg_delta->filename);
	char *local_file = g_strdup_printf ("%s/%s/deltas/%s",
					    LOCAL_CACHE, repo->id,
					    filename);

	return local_file;
}

static bool
download_delta (LowRepo *repo, LowPackageDelta *pkg_delta)
{
	int res;
	LowMirrorList *mirrors = low_repo_sqlite_get_mirror_list (repo);

	const char *filename = get_file_basename (pkg_delta->filename);
	char *local_file = create_delta_filepath (repo, pkg_delta);
	char *dirname = g_strdup_printf ("%s/%s/deltas", LOCAL_CACHE,
					 repo->id);

	if (!g_file_test (dirname, G_FILE_TEST_EXISTS)) {
		mkdir (dirname, 0755);
	}
	free (dirname);

	res = low_download_if_missing (mirrors, pkg_delta->filename, local_file,
				       filename, pkg_delta->digest,
				       pkg_delta->digest_type, pkg_delta->size,
				       download_callback);
	free (local_file);

	return res == 0;
}

static bool
verify_delta (const char *sequence, const char *arch)
{
	int res;
	char *command =
		g_strdup_printf ("/usr/bin/applydeltarpm -a %s -C -s %s",
				 arch, sequence);

	if (g_spawn_command_line_sync (command, NULL, NULL, &res, NULL)) {
		free (command);
		return res == 0;
	}

	free (command);
	return false;
}

static bool
apply_delta (LowPackageDelta *pkg_delta, LowPackage *new_pkg)
{
	int res;
	char *delta_file = create_delta_filepath (new_pkg->repo, pkg_delta);
	char *rpm_file = create_package_filepath (new_pkg);
	char *command = g_strdup_printf ("/usr/bin/applydeltarpm -a %s %s %s",
					 pkg_delta->arch, delta_file, rpm_file);

	printf ("Rebuilding %s\n", get_file_basename (rpm_file));
	if (g_spawn_command_line_sync (command, NULL, NULL, &res, NULL)) {
		free (command);
		return res == 0;
	}

	free (command);
	return false;
}

static bool
construct_delta (LowPackage *new_pkg, LowPackage *old_pkg)
{
	LowPackageDelta *pkg_delta;
	LowDelta *delta;

	delta = low_repo_sqlite_get_delta (new_pkg->repo);
	if (delta == NULL) {
		return false;
	}

	pkg_delta = low_delta_find_delta (delta, new_pkg, old_pkg);
	if (pkg_delta == NULL) {
		return false;
	}

	if (!download_delta (new_pkg->repo, pkg_delta)) {
		return false;
	}

	if (!verify_delta (pkg_delta->sequence, pkg_delta->arch)) {
		return false;
	}

	if (!apply_delta (pkg_delta, new_pkg)) {
		return false;
	}

	return true;
}

static int
command_download (int argc G_GNUC_UNUSED, const char *argv[])
{
	LowRepo *repo_rpmdb;
	LowRepoSet *repos;
	LowPackageIter *iter;
	int found_pkg;
	int ret = EXIT_SUCCESS;

	if (!initialize_repos (&repo_rpmdb, &repos)) {
		return EXIT_FAILURE;
	}

	iter = low_repo_set_list_by_name (repos, argv[0]);
	found_pkg = 0;
	while (iter = low_package_iter_next (iter), iter != NULL) {
		LowPackage *pkg = iter->pkg;
		found_pkg = 1;
		if (!download_package (pkg)) {
			printf ("Unable to download %s\n", pkg->name);
		}
		low_package_unref (pkg);
	}

	if (!found_pkg) {
		printf ("No such package: %s\n", argv[0]);
		ret = EXIT_FAILURE;
	}

	low_repo_set_free (repos);
	low_repo_rpmdb_shutdown (repo_rpmdb);

	return ret;
}

static void
print_transaction (LowTransaction *trans)
{
	uint update_size = g_hash_table_size (trans->update);
	uint install_size = g_hash_table_size (trans->install);
	uint remove_size = g_hash_table_size (trans->remove);

	if (update_size > 0) {
		printf ("Update:\n");
		print_transaction_part (trans->update);
	}

	if (install_size > 0) {
		printf ("\nInstall:\n");
		print_transaction_part (trans->install);
	}

	if (remove_size > 0) {
		printf ("\nRemove:\n");
		print_transaction_part (trans->remove);
	}

	printf ("\nSummary:\n");
	if (update_size > 0) {
		printf ("Update: %d\n", update_size);
	}
	if (install_size > 0) {
		printf ("Install: %d\n", install_size);
	}
	if (remove_size > 0) {
		printf ("Remove: %d\n", remove_size);
	}
}

static void
print_transaction_problems (LowTransaction *trans)
{
	printf ("Error resolving transaction\n");
	printf ("The following packages had errors:\n");
	print_transaction_part (trans->unresolved);
}

static bool
prompt_confirmed (void)
{
	char input;

	printf ("\nRun transaction? [y/N] ");

	input = getchar ();

	if (input == 'y' || input == 'Y') {
		return true;
	}

	return false;
}

static bool
download_required_packages (LowTransaction *trans)
{
	GList *list;

	bool successful = true;

	list = g_hash_table_get_values (trans->install);
	while (list != NULL) {
		LowTransactionMember *member = list->data;
		if (!download_package (member->pkg)) {
			printf ("Unable to download %s\n", member->pkg->name);
			successful = false;
		}
		list = list->next;
	}

	list = g_hash_table_get_values (trans->update);
	while (list != NULL) {
		LowTransactionMember *member = list->data;
		LowPackage *pkg = member->pkg;

		char *local_file = create_package_filepath (member->pkg);
		if (low_download_is_missing (local_file, pkg->digest,
					     pkg->digest_type, pkg->size) &&
		    !construct_delta (member->pkg, member->related_pkg)) {
			if (!download_package (member->pkg)) {
				printf ("Unable to download %s\n",
					member->pkg->name);
				successful = false;
			}
		}
		list = list->next;

		free (local_file);
	}

	return successful;
}

static void
add_installs_to_transaction (GHashTable *hash, rpmts ts)
{
	GList *list = g_hash_table_get_values (hash);
	while (list != NULL) {
		LowTransactionMember *member = list->data;

		/* XXX RPM needs this around during the transaction */
		char *filepath = create_package_filepath (member->pkg);
		FD_t fd = Fopen (filepath, "r.ufdio");
		Header hdr;
		int res = rpmReadPackageFile (ts, fd, NULL, &hdr);
		Fclose (fd);
		if (res != RPMRC_OK) {
			printf ("%d\n", res);
			/* XXX do something better here */
			printf ("Unable to read %s, skipping\n", filepath);
		} else {
			rpmtsAddInstallElement (ts, hdr, (fnpyKey) filepath,
						0, 0);
		}
		list = list->next;
	}
}

static void
add_removes_to_transaction (GHashTable *hash, rpmts ts)
{
	GList *list = g_hash_table_get_values (hash);
	while (list != NULL) {
		LowTransactionMember *member = list->data;
		LowPackage *pkg = member->pkg;

		rpmdbMatchIterator iter;
		Header hdr;
		unsigned int offset;

		iter = rpmdbInitIterator (low_repo_rpmdb_get_db (pkg->repo),
					  RPMTAG_PKGID, pkg->id, 16);
		hdr = rpmdbNextIterator (iter);
		offset = rpmdbGetIteratorOffset (iter);

		rpmtsAddEraseElement (ts, hdr, offset);
		list = list->next;

		rpmdbFreeIterator (iter);
	}
}

typedef enum _CallbackState {
	CALLBACK_PREPARE,
	CALLBACK_INSTALL,
	CALLBACK_REMOVE
} CallbackState;

typedef struct _CallbackData {
	bool verbose;
	char *name;
	int total_rpms;
	int current_rpm;
	CallbackState state;
} CallbackData;

static void
printHash (int part, int total, CallbackData *data)
{
	int num_digits = digit_count (data->total_rpms);

	if (data->state == CALLBACK_INSTALL) {
		printf ("\r(%*d/%d) Installing ", num_digits,
			data->current_rpm, data->total_rpms);
	} else if (data->state == CALLBACK_REMOVE) {
		printf ("\r(%*d/%d) Removing   ", num_digits,
			data->current_rpm, data->total_rpms);
	} else {
		printf ("\r");
	}

	printf ("%s %3d%%", data->name, part * 100 / total);

	if (part == total) {
		printf ("\n");
	}

	fflush (stdout);
}

static void *
low_show_rpm_progress (const void *arg, const rpmCallbackType what,
		       const rpm_loff_t amount, const rpm_loff_t total,
		       fnpyKey key, void *data)
{
	Header h = (Header) arg;
	void *rc = NULL;
	const char *filename = (const char *) key;
	static FD_t fd = NULL;

	CallbackData *callback = (CallbackData *) data;
	bool verbose = callback->verbose;

	switch (what) {
		case RPMCALLBACK_INST_OPEN_FILE:
			if (filename == NULL || filename[0] == '\0')
				return NULL;
			fd = Fopen (filename, "r.ufdio");
			/* FIX: still necessary? */
			if (fd == NULL || Ferror (fd)) {
				if (fd != NULL) {
					Fclose (fd);
					fd = NULL;
				}
			} else
				fd = fdLink (fd, "persist (showProgress)");
			return (void *) fd;
			break;

		case RPMCALLBACK_INST_CLOSE_FILE:
			/* FIX: still necessary? */
			fd = fdFree (fd, "persist (showProgress)");
			if (fd != NULL) {
				Fclose (fd);
				fd = NULL;
			}
			break;

		case RPMCALLBACK_INST_START:
		case RPMCALLBACK_UNINST_START:
			if (h == NULL)
				break;
			/* @todo Remove headerFormat() on a progress callback. */
			if (verbose) {
				callback->current_rpm++;
				callback->state =
					(what ==
					 RPMCALLBACK_INST_START) ?
					CALLBACK_INSTALL : CALLBACK_REMOVE;
				callback->name =
					headerFormat (h,
						      "%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}",
						      NULL);
				printHash (0, 1, callback);
			}
			break;

		case RPMCALLBACK_TRANS_PROGRESS:
		case RPMCALLBACK_INST_PROGRESS:
		case RPMCALLBACK_UNINST_PROGRESS:
			if (verbose)
				printHash (amount, total, callback);

			if (amount == total) {
				free (callback->name);
				callback->name = NULL;
			}
			break;

		case RPMCALLBACK_TRANS_START:
			if (verbose) {
				callback->state = CALLBACK_PREPARE;
				callback->name = strdup ("Preparing...");
				callback->total_rpms = total;
				callback->current_rpm = 0;
			}
			break;

		case RPMCALLBACK_TRANS_STOP:
		case RPMCALLBACK_UNINST_STOP:
			if (verbose)
				printHash (1, 1, callback);

			free (callback->name);
			callback->name = NULL;

			break;

		case RPMCALLBACK_UNPACK_ERROR:
		case RPMCALLBACK_CPIO_ERROR:
		case RPMCALLBACK_SCRIPT_ERROR:
		case RPMCALLBACK_UNKNOWN:
		case RPMCALLBACK_REPACKAGE_PROGRESS:
		case RPMCALLBACK_REPACKAGE_START:
		case RPMCALLBACK_REPACKAGE_STOP:
		default:
			break;
	}

	return rc;
}

static rpmts
low_transaction_to_rpmts (LowTransaction *trans, CallbackData *data)
{
	int flags;
	rpmts ts = rpmtsCreate ();
	rpmtsSetRootDir (ts, "/");
	rpmtsSetNotifyCallback (ts, low_show_rpm_progress, data);

	flags = rpmtsSetVSFlags (ts, _RPMVSF_NOSIGNATURES | _RPMVSF_NODIGESTS);
	add_installs_to_transaction (trans->install, ts);
	add_installs_to_transaction (trans->update, ts);

	add_removes_to_transaction (trans->remove, ts);
	add_removes_to_transaction (trans->updated, ts);

	rpmtsSetVSFlags (ts, flags);

	return ts;
}

static void
run_transaction (LowTransaction *trans, bool assume_yes)
{
	if (!(g_hash_table_size (trans->install) ||
	      g_hash_table_size (trans->update) ||
	      g_hash_table_size (trans->remove) ||
	      g_hash_table_size (trans->updated))) {
		printf ("Nothing to do.\n");
		return;
	}

	print_transaction (trans);

	if (assume_yes || prompt_confirmed ()) {
		rpmts ts;
		int rc;
		CallbackData data;
		data.name = NULL;
		data.verbose = true;

		printf ("Running\n");
		if (!download_required_packages (trans)) {
			printf ("Some packages failed to download. aborting\n");
			return;
		}
//              rpmSetVerbosity(RPMLOG_DEBUG);
		ts = low_transaction_to_rpmts (trans, &data);
		rpmtsSetFlags (ts, RPMTRANS_FLAG_NONE);

		rc = rpmtsRun (ts, NULL, RPMPROB_FILTER_NONE);
		if (rc != 0) {
			rpmps problems = rpmtsProblems (ts);

			printf ("Error running transaction\n");
			/* XXX probably should print the error stuff ourselves */
			rpmpsPrint (stdout, problems);
		}

		rpmtsFree (ts);
	}
}

static LowPackage *
select_package_for_install (LowPackageIter *iter)
{
	LowPackage *best = NULL;
	char *best_evr = strdup ("");

	while (iter = low_package_iter_next (iter), iter != NULL) {
		char *new_evr = low_package_evr_as_string (iter->pkg);
		int cmp = low_util_evr_cmp (new_evr, best_evr);

		if (cmp > 0 ||
		    (cmp == 0 && best != NULL &&
		     low_arch_choose_best_for_system (best->arch,
						      iter->pkg->arch) < 0)) {
			if (best) {
				low_package_unref (best);
			}

			free (best_evr);
			best = iter->pkg;
			best_evr = new_evr;
		} else {
			low_package_unref (iter->pkg);
			free (new_evr);
		}

	}

	free (best_evr);

	return best;
}

bool assume_yes = false;

LowOption transaction_options[] = {
	{OPTION_BOOL, 'y', "assume-yes", &assume_yes, NULL,
		"Assume yes for any questions"},
	LOW_OPTION_END
};

static int
command_install (int argc, const char *argv[])
{
	LowRepo *repo_rpmdb;
	LowRepoSet *repos;
	LowPackageIter *iter;
	LowTransaction *trans;
	int i;
	int res;
	int counter = 0;

	if (!initialize_repos (&repo_rpmdb, &repos)) {
		return EXIT_FAILURE;
	}

	trans = low_transaction_new (repo_rpmdb, repos, transaction_callback,
				     &counter);

	for (i = 0; i < argc; i++) {
		LowPackage *pkg;
		LowPackageDependency *provides =
			low_package_dependency_new_from_string (argv[i]);

		iter = low_repo_rpmdb_search_provides (repo_rpmdb, provides);
		iter = low_package_iter_next (iter);
		if (iter != NULL) {
			low_package_unref (iter->pkg);
			low_package_iter_free (iter);

			printf ("'%s' is already installed.\n", argv[i]);
			continue;
		}

		iter = low_repo_set_search_provides (repos, provides);
		pkg = select_package_for_install (iter);
		if (pkg) {
			low_transaction_add_install (trans, pkg);
		}

		low_package_dependency_free (provides);
	}

	if (low_transaction_resolve (trans) != LOW_TRANSACTION_OK) {
		print_transaction_problems (trans);
		res = EXIT_FAILURE;
	} else {
		run_transaction (trans, assume_yes);
		res = EXIT_SUCCESS;
	}

	low_transaction_free (trans);
	low_repo_set_free (repos);
	low_repo_rpmdb_shutdown (repo_rpmdb);

	return res;
}

static int
command_update (int argc, const char *argv[])
{
	LowRepo *repo_rpmdb;
	LowRepoSet *repos;
	LowPackageIter *iter;
	LowTransaction *trans;
	int i;
	int res;
	int counter = 0;

	if (!initialize_repos (&repo_rpmdb, &repos)) {
		return EXIT_FAILURE;
	}

	trans = low_transaction_new (repo_rpmdb, repos, transaction_callback,
				     &counter);

	for (i = 0; i < argc; i++) {
		LowPackageDependency *provides =
			low_package_dependency_new_from_string (argv[i]);
		/* XXX just do the most EVR newest */
		iter = low_repo_rpmdb_search_provides (repo_rpmdb, provides);

		/* XXX get rid of this nastiness somehow */
		while (iter = low_package_iter_next (iter), iter != NULL) {
			low_transaction_add_update (trans, iter->pkg);
		}

		low_package_dependency_free (provides);
	}

	if (argc == 0) {
		compute_updates (trans, repo_rpmdb);
	}

	if (low_transaction_resolve (trans) != LOW_TRANSACTION_OK) {
		print_transaction_problems (trans);
		res = EXIT_FAILURE;
	} else {
		run_transaction (trans, assume_yes);
		res = EXIT_SUCCESS;
	}

	low_transaction_free (trans);
	low_repo_set_free (repos);
	low_repo_rpmdb_shutdown (repo_rpmdb);

	return res;
}

static int
command_remove (int argc, const char *argv[])
{
	LowRepo *repo_rpmdb;
	LowRepoSet *repos;
	LowPackageIter *iter;
	LowTransaction *trans;
	int i;
	int res;
	int counter = 0;

	if (!initialize_repos (&repo_rpmdb, &repos)) {
		return EXIT_FAILURE;
	}

	trans = low_transaction_new (repo_rpmdb, repos, transaction_callback,
				     &counter);

	for (i = 0; i < argc; i++) {
		LowPackageDependency *provides =
			low_package_dependency_new_from_string (argv[i]);

		/* XXX just do the most EVR newest */
		iter = low_repo_rpmdb_search_provides (repo_rpmdb, provides);
		iter = low_package_iter_next (iter);

		if (iter == NULL) {
			printf ("No such package to remove\n");
			return EXIT_FAILURE;
		}

		low_transaction_add_remove (trans, iter->pkg);

		low_package_iter_free (iter);

		low_package_dependency_free (provides);
	}

	if (low_transaction_resolve (trans) != LOW_TRANSACTION_OK) {
		print_transaction_problems (trans);
		res = EXIT_FAILURE;
	} else {
		run_transaction (trans, assume_yes);
		res = EXIT_SUCCESS;
	}

	low_transaction_free (trans);
	low_repo_set_free (repos);
	low_repo_rpmdb_shutdown (repo_rpmdb);

	return res;
}

static char *
download_repodata_file (LowRepo *repo, const char *relative_name)
{
	int ret;
	LowMirrorList *mirrors = low_repo_sqlite_get_mirror_list (repo);

	const char *basename = get_file_basename (relative_name);
	char *local_file = g_strdup_printf ("%s/%s/%s.tmp",
					    LOCAL_CACHE, repo->id,
					    basename);

	/* Just something nice to display */
	char *displayed_basename;
	if (strlen (basename) > 24) {
		int offset = strlen (basename) - 24;
		displayed_basename =
			g_strdup_printf ("%s - ...%s", repo->id,
					 basename + offset);
	} else {
		displayed_basename = g_strdup_printf ("%s - %s", repo->id,
						      basename);
	}

	/* XXX use if_missing here for non repomd.xml */
	ret = low_download_from_mirror (mirrors, relative_name, local_file,
					displayed_basename, download_callback);

	free (displayed_basename);

	if (ret != 0) {
		printf ("\nUnable to download %s\n", basename);
		exit (EXIT_FAILURE);
	}
	return local_file;
}

#define BUF_SIZE 1024

/* XXX should do this while downloading */
static char *
uncompress_file_bz2 (const char *filename)
{
	int error;
	int size_read;
	FILE *file;
	BZFILE *compressed;
	int fd;
	char buf[BUF_SIZE];
	char *uncompressed_name = malloc (strlen (filename) - 3);
	strncpy (uncompressed_name, filename, strlen (filename) - 4);
	uncompressed_name[strlen (filename) - 4] = '\0';

	file = fopen (filename, "r");
	compressed = BZ2_bzReadOpen (&error, file, 0, 0, NULL, 0);

	fd = open (uncompressed_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	printf ("Uncompressing...\n");
	while (size_read = BZ2_bzRead (&error, compressed, &buf, BUF_SIZE),
	       size_read != 0) {
		if (write (fd, buf, size_read) == -1) {
			/* XXX do something better here on failure */
			exit (EXIT_FAILURE);
		}
	}

	close (fd);

	return uncompressed_name;
}

static char *
uncompress_file_gz (const char *filename)
{
	int size_read;
	gzFile *compressed;
	int fd;
	char buf[BUF_SIZE];
	char *uncompressed_name = malloc (strlen (filename) - 2);
	strncpy (uncompressed_name, filename, strlen (filename) - 3);
	uncompressed_name[strlen (filename) - 3] = '\0';

	compressed = gzopen (filename, "rb");

	fd = open (uncompressed_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	printf ("Uncompressing...\n");
	while (size_read = gzread (compressed, &buf, BUF_SIZE), size_read != 0) {
		if (write (fd, buf, size_read) == -1) {
			/* XXX do something better here on failure */
			exit (EXIT_FAILURE);
		}
	}

	close (fd);

	return uncompressed_name;
}

static char *
create_repodata_filename (LowRepo *repo, const char *relative_name)
{
	const char *basename = get_file_basename (relative_name);
	return g_strdup_printf ("%s/%s/%s", LOCAL_CACHE, repo->id, basename);
}

static bool
repodata_missing (LowRepo *repo, const char *relative_name)
{
	char *filename = create_repodata_filename (repo, relative_name);
	const char *last_dot = rindex (filename, '.');
	char *uncompressed_name = malloc ((last_dot - filename) + 1);
	strncpy (uncompressed_name, filename, last_dot - filename);
	uncompressed_name[last_dot - filename] = '\0';

	free (filename);

	/* XXX verify checksum */
	if (!g_file_test (uncompressed_name, G_FILE_TEST_EXISTS)) {
		free (uncompressed_name);
		return true;
	} else {
		free (uncompressed_name);
		return false;
	}
}

static void
fetch_repodata_file (LowRepo *repo, const char *relative_name, bool is_bz2)
{
	char *tmp_file;
	char *local_file;
	char *db_file;

	tmp_file = download_repodata_file (repo, relative_name);
	local_file = create_repodata_filename (repo, relative_name);
	rename (tmp_file, local_file);

	if (is_bz2) {
		db_file = uncompress_file_bz2 (local_file);
	} else {
		db_file = uncompress_file_gz (local_file);
	}

	free (local_file);
	free (tmp_file);
	free (db_file);
}

static void
refresh_repo (LowRepo *repo)
{
	char *local_file;
	char *tmp_file;
	char *dirname;
	LowRepomd *old_repomd;
	LowRepomd *new_repomd;
	LowRepomd *repomd;

	dirname = g_strdup_printf ("/var/cache/yum/%s", repo->id);
	if (!g_file_test (dirname, G_FILE_TEST_EXISTS)) {
		mkdir (dirname, 0755);
	}
	free (dirname);

	if (repo->mirror_list) {
		char *display;
		/*
		 * copy yum's hack to decide if the mirrorlist is plain text,
		 * or fancy metalink.
		 */
		if (strstr (repo->mirror_list, "metalink")) {
			display = g_strdup_printf ("%s - metalink", repo->id);
			local_file = create_repodata_filename (repo,
							       "metalink.xml");
		} else {
			display = g_strdup_printf ("%s - mirrorlist.txt",
						   repo->id);
			local_file =
				create_repodata_filename (repo,
							  "mirrorlist.txt");
		}
		low_download (repo->mirror_list, local_file, display,
			      download_callback);

		free (display);
		free (local_file);
	}

	local_file = create_repodata_filename (repo, "repodata/repomd.xml");
	old_repomd = low_repomd_parse (local_file);

	tmp_file = download_repodata_file (repo, "repodata/repomd.xml");
	new_repomd = low_repomd_parse (tmp_file);

	if (old_repomd == NULL ||
	    old_repomd->primary_db_time < new_repomd->primary_db_time ||
	    old_repomd->filelists_db_time < new_repomd->filelists_db_time) {
		rename (tmp_file, local_file);
		repomd = new_repomd;
		low_repomd_free (old_repomd);
	} else {
		repomd = old_repomd;
		low_repomd_free (new_repomd);
	}

	free (local_file);
	free (tmp_file);

	if (repomd->primary_db) {
		if (repodata_missing (repo, repomd->primary_db)) {
			fetch_repodata_file (repo, repomd->primary_db, true);
		}

		if (repodata_missing (repo, repomd->filelists_db)) {
			fetch_repodata_file (repo, repomd->filelists_db, true);
		}
	} else {
		char *primary_file;
		char *filelists_file;

		if (repodata_missing (repo, repomd->primary_xml)) {
			fetch_repodata_file (repo, repomd->primary_xml, false);
		}

		if (repodata_missing (repo, repomd->filelists_xml)) {
			fetch_repodata_file (repo, repomd->filelists_xml,
					     false);
		}

		primary_file = create_repodata_filename (repo,
							 repomd->primary_xml);
		filelists_file =
			create_repodata_filename (repo, repomd->filelists_xml);

		/* XXX Do something better here */
		primary_file[strlen (primary_file) - 3] = '\0';
		filelists_file[strlen (filelists_file) - 3] = '\0';

		low_repoxml_parse (primary_file, filelists_file);

		free (primary_file);
		free (filelists_file);
	}

	if (repomd->delta_xml && repodata_missing (repo, repomd->delta_xml)) {
		fetch_repodata_file (repo, repomd->delta_xml, false);
	}

	low_repomd_free (repomd);
}

static int
command_refresh (int argc G_GNUC_UNUSED, const char **argv G_GNUC_UNUSED)
{
	LowRepo *repo_rpmdb;
	LowRepoSet *repos;
	LowConfig *config;
	LowRepoSetFilter filter = ENABLED;

	repo_rpmdb = low_repo_rpmdb_initialize ();

	config = low_config_initialize (repo_rpmdb);
	repos = low_repo_set_initialize_from_config (config, false);

	low_repo_set_for_each (repos, filter, refresh_repo);

	low_repo_set_free (repos);
	low_config_free (config);
	low_repo_rpmdb_shutdown (repo_rpmdb);

	return EXIT_SUCCESS;
}

/**
 * Display the program version as specified in configure.ac
 */
static int
command_version (int argc G_GNUC_UNUSED, const char **argv G_GNUC_UNUSED)
{
	printf (PACKAGE_STRING "\n");
	return EXIT_SUCCESS;
}

bool help = false;
bool version = false;

LowOption global_options[] = {
	{OPTION_BOOL, 'h', "help", &help, NULL, "Show command help"},
	{OPTION_BOOL, 0, "version", &version, NULL, "Show program version"},
	LOW_OPTION_END
};

static int
command_help (int argc, const char *argv[])
{
	if (argc == 0) {
		usage ();
	} else if (argc == 1) {
		show_help (argv[0]);
	} else {
		show_help ("help");
	}

	return EXIT_SUCCESS;
}

static int
NOT_IMPLEMENTED (int argc G_GNUC_UNUSED, const char **argv G_GNUC_UNUSED)
{
	printf ("This function is not yet implemented\n");

	return EXIT_FAILURE;
}

#define NO_USAGE ""

typedef struct _SubCommand {
	const char *name;
	const char *usage;
	const char *summary;
	int (*func) (int argc, const char *argv[]);
	LowOption *options;
} SubCommand;

const SubCommand commands[] = {
	{"refresh", NO_USAGE, "Download new metadata", command_refresh, NULL},
	{"install", "PACKAGE", "Install a package", command_install,
	  transaction_options},
	{"update", "[PACKAGE]", "Update or install a package",
	 command_update, transaction_options},
	{"remove", "PACKAGE", "Remove a package", command_remove,
	 transaction_options},
	{"clean", NO_USAGE, "Remove cached data", NOT_IMPLEMENTED, NULL},
	{"info", "PACKAGE", "Display package details", command_info,
	 info_options},
	{"list", "[all|installed|PACKAGE]", "Display a group of packages",
	 command_list, NULL},
	{"download", NO_USAGE,
	 "Download (but don't install) a list of packages", command_download,
	 NULL},
	{"search", "PATTERN",
	 "Search package information for the given string", command_search,
	 NULL},
	{"repolist", "[all|enabled|disabled]",
	 "Display configured software repositories", command_repolist, NULL},
	{"whatprovides", "PATTERN",
	 "Find what package provides the given value", command_whatprovides,
	 NULL},
	{"whatrequires", "PATTERN",
	 "Find what package requires the given value", command_whatrequires,
	 NULL},
	{"whatconflicts", "PATTERN",
	 "Find what package conflicts the given value",
	 command_whatconflicts, NULL},
	{"whatobsoletes", "PATTERN",
	 "Find what package obsoletes the given value",
	 command_whatobsoletes, NULL},
	{"version", NO_USAGE, "Display version information", command_version,
	 NULL},
	{"help", "COMMAND", "Display a helpful usage message", command_help,
	 NULL}
};

static void
print_options (LowOption *options)
{
	LowOption *option;

	for (option = options; option->type != OPTION_END; option++) {
		if (option->short_name && option->long_name) {
			printf ("  -%c, --%-14s", option->short_name,
				option->long_name);
		} else if (option->short_name) {
			printf ("  -%-19c", option->short_name);
		} else {
			printf ("  --%-18s", option->long_name);
		}

		printf ("%s\n", option->help);
	}

}

static void
show_help (const char *command)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE (commands); i++) {
		if (!strcmp (command, commands[i].name)) {
			printf ("Usage: %s %s\n", commands[i].name,
				commands[i].usage);
			printf ("\n%s\n", commands[i].summary);

			if (commands[i].options) {
				printf ("Options:\n");
				print_options (commands[i].options);
			}
		}
	}
}

/**
 * Display the default help message.
 *
 * @return A return code to exit the program with.
 */
static int
usage (void)
{
	unsigned int i;

	printf ("low: a yum-like package manager\n\n");
	printf ("Top-level options:\n");
	print_options (global_options);
	printf ("\nAvailable commands:\n");
	for (i = 0; i < ARRAY_SIZE (commands); i++) {
		printf ("  %-20s%s\n", commands[i].name, commands[i].summary);
	}

	return EXIT_FAILURE;
}

int
main (int argc, const char *argv[])
{
	unsigned int i;
	int consumed;

	argc--;
	argv++;

	consumed = low_parse_options (argc, argv, global_options);

	argc -= consumed;
	argv += consumed;

	if (version) {
		return command_version (argc, argv);
	}

	if (consumed < 0 || help || argc < 1) {
		return usage ();
	}

	low_debug_init ();

	for (i = 0; i < ARRAY_SIZE (commands); i++) {
		if (!strcmp (argv[0], commands[i].name)) {
			argc--;
			argv++;

			if (commands[i].options != NULL) {
				consumed =
					low_parse_options (argc, argv,
							   commands[i].options);

				if (consumed < 0) {
					show_help (commands[i].name);

					return EXIT_FAILURE;
				}

				argc -= consumed;
				argv += consumed;
			}
			return commands[i].func (argc, argv);
		}
	}
	printf ("Unknown command: %s\n", argv[0]);

	return usage ();
}

/* vim: set ts=8 sw=8 noet: */
