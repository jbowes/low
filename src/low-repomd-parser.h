/*
 *  Low: a yum-like package manager
 *
 *  Copyright (C) 2009 - 2010 James Bowes <jbowes@repl.ca>
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

#ifndef _LOW_REPOMD_PARSER_H_
#define _LOW_REPOMD_PARSER_H_

typedef struct _LowRepomd {
	char *primary_db;
	time_t primary_db_time;

	char *filelists_db;
	time_t filelists_db_time;

	char *primary_xml;
	time_t primary_xml_time;

	char *filelists_xml;
	time_t filelists_xml_time;

	char *delta_xml;
	time_t delta_xml_time;

} LowRepomd;

LowRepomd *low_repomd_parse (const char *repodata);
void low_repomd_free (LowRepomd *repomd);

#endif /* _LOW_REPOMD_PARSER_H_ */

/* vim: set ts=8 sw=8 noet: */
