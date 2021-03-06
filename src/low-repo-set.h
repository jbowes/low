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

#include <glib.h>
#include "low-config.h"
#include "low-repo.h"
#include "low-package.h"

#ifndef _LOW_REPO_SET_H_
#define _LOW_REPO_SET_H_

/**
 * A set of multiple repositories.
 */
typedef struct _LowRepoSet {
	LowRepo super;
	GHashTable *repos;
} LowRepoSet;

typedef enum {
	ENABLED,
	DISABLED,
	ALL
} LowRepoSetFilter;

typedef void (*LowRepoSetFunc) (LowRepo *repo);

LowRepoSet *    low_repo_set_initialize_from_config 	(LowConfig *config,
							 bool bind_dbs);
void            low_repo_set_free                      	(LowRepoSet *repo_set);

void            low_repo_set_for_each                  	(LowRepoSet *repo_set,
							 LowRepoSetFilter filter,
							 LowRepoSetFunc func);

LowPackageIter * low_repo_set_list_all			(LowRepoSet *repo_set);
LowPackageIter * low_repo_set_list_by_name 		(LowRepoSet *repo_set,
							 const char *name);

LowPackageIter * low_repo_set_search_provides      	(LowRepoSet *repo_set,
							 const LowPackageDependency *provides);
LowPackageIter * low_repo_set_search_requires      	(LowRepoSet *repo_set,
							 const LowPackageDependency *requires);
LowPackageIter * low_repo_set_search_conflicts 		(LowRepoSet *repo_set,
							 const LowPackageDependency *conflicts);
LowPackageIter * low_repo_set_search_obsoletes 		(LowRepoSet *repo_set,
							 const LowPackageDependency *obsoletes);

LowPackageIter * low_repo_set_search_files      	(LowRepoSet *repo_set,
							 const char *file);

LowPackageIter * low_repo_set_search_details      	(LowRepoSet *repo_set,
							 const char *querystr);

#endif /* _LOW_REPO_SET_H_ */

/* vim: set ts=8 sw=8 noet: */
