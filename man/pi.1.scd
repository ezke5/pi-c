pi(1)

# NAME

pi - build and manage packages

# SYNOPSIS

*pi* [--root _PATH_] _COMMAND_ [_ARGUMENT_...]

# DESCRIPTION

*pi* builds packages and manages an installed package database.  The
optional *--root* argument makes all package state relative to _PATH_
instead of the system root.

# COMMANDS

*build* _PACKAGE_
	Build _PACKAGE_ from its cached source.

*sync* _REPOSITORY_ _INDEX_
	Fetch a repository index from _REPOSITORY_ and store it as _INDEX_.

*install* _ARCHIVE_...
	Install one or more package archives.

*remove* _PACKAGE_
	Remove an installed package.

*upgrade*
	Upgrade installed packages.  Repository orchestration must be configured.

*autoremove*
	Remove packages no longer needed by another package.

*search* _QUERY_
	Search synchronized repository indexes.

*verify* _PACKAGE_
	Verify the installed package and print *OK* or *CORRUPTED*.

*info* _PACKAGE_
	Print metadata for an installed package.

*clean* [--keep _N_]
	Remove cached package data, retaining _N_ generations (two by default).

# OPTIONS

*--root* _PATH_
	Use _PATH_ as the package database, cache, and staging root.

*--keep* _N_
	With *clean*, retain _N_ cache generations.

# FILES

*/var/db/pi/*
	Installed package database and the world set.

*/var/cache/pi/*
	Package sources, archives, and staging files.

# EXIT STATUS

0
	The operation succeeded.

2
	The command line was invalid.

Non-zero
	The operation failed; the value is an operating-system error number when
	available.

# LICENSE

GNU General Public License, version 3 or later.
