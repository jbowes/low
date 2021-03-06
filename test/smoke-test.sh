#!/bin/bash
#
#  Low: a yum-like package manager
#
#  Copyright (C) 2008 - 2010 James Bowes <jbowes@repl.ca>
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
#  02110-1301  USA
#
#  Basic smoke tests for low, which require a live system.
#
#  XXX Check the output


LOW=../src/low

PASSED=1

TOTAL=0
NUM_PASSED=0

function run_or_die {
    let TOTAL=$TOTAL+1

    printf "Testing '$1'... "
    `$LOW $1 > /dev/null`
    if (($?)); then
        printf "\E[31mFAIL\n"
        PASSED=0
    else
        printf "\E[32mOK\n"
        let NUM_PASSED=$NUM_PASSED+1
    fi
    tput sgr0
}

run_or_die "version"
run_or_die "help"

run_or_die "repolist"
run_or_die "repolist all"
run_or_die "repolist enabled"
run_or_die "repolist disabled"

run_or_die "info zsh"
# An uninstalled package
run_or_die "info -a kdesdk"
# Something from the rpmdb
run_or_die "info -a bash"

run_or_die "list installed"
run_or_die "list available"
run_or_die "list all"
run_or_die "list updates"
run_or_die "list bash"
# An uninstalled package
run_or_die "list beagle"

run_or_die "search zsh"

run_or_die "whatprovides zsh"
run_or_die "whatprovides /bin/zsh"
run_or_die "whatprovides /usr/share/git-core"
run_or_die "whatrequires git"
run_or_die "whatconflicts bash"
run_or_die "whatobsoletes git-core"

echo "$TOTAL tests run, $[ $TOTAL - $NUM_PASSED ] failures"

if (($PASSED)); then
    echo "Smoke tests passed"
else
    echo "*** SMOKE TESTS FAILED ***"
    exit 1
fi
