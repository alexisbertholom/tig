# bash/zsh completion for tig
#
# Copyright (C) 2019 Roland Hieber, Pengutronix
# Copyright (C) 2006-2026 Jonas fonseca
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; either version 2 of
# the License, or (at your option) any later version.
#
# This completion builds upon the git completion (>= git 1.17.11),
# which most tig users should already have available at this point.
# To use these routines:
#
#    1) Copy this file to the completion directory of bash-completion,
#       under the name bash looks the command up by:
#
#           ~/.local/share/bash-completion/completions/tig
#
#       bash-completion reads that directory on the first tig
#       completion, so shells started afterwards need nothing else.
#
#    2) In shells that were already running, and in setups without
#       bash-completion, source the file by hand instead:
#
#           source ~/.local/share/bash-completion/completions/tig
#
#    3) You may want to make sure the git executable is available
#       in your PATH before this script is sourced, as some caching
#       is performed while the script loads.  If git isn't found
#       at source time then all lookups will be done on demand,
#       which may be slightly slower.

#tig-completion requires __git_complete
#* If not defined, source git completions script so __git_complete is available
if ! declare -f __git_complete &>/dev/null; then
	_bash_completion=$(pkg-config --variable=completionsdir bash-completion 2>/dev/null) ||
		_bash_completion='/usr/share/bash-completion/completions/'
			_locations=(
				"$(dirname "${BASH_SOURCE[0]%:*}")"/git-completion.bash #in same dir as this
				"$HOME/.local/share/bash-completion/completions/git"
				"$_bash_completion/git"
				'/etc/bash_completion.d/git' # old debian
			)
			for _e in "${_locations[@]}"; do
				# shellcheck disable=1090
				test -f "$_e" && . "$_e" && break
			done
			unset _bash_completion _locations _e
			if ! declare -f __git_complete &>/dev/null; then
				return #silently return without completions
			fi
fi

__tig_options="
	-v --version
	-h --help
	-C
"
# Comparing HEAD with another revision is tig's own business, and is asked
# for before any subcommand.
__tig_bdiff_options="
	--bdiff
	--bdiff-base=
	--bdiff-onto=
"
__tig_commands="
	blame
	diff
	grep
	log
	reflog
	refs
	stash
	status
	show
"
# Names tig resolves on its own wherever a revision is expected, and which
# git therefore knows nothing about.
__tig_revision_aliases="
	_up
"

# Offer tig's own revision names next to whatever git already completed.
__tig_complete_revision_aliases () {
	local cur_="${1-$cur}" pfx="${2-}" alias
	for alias in $__tig_revision_aliases; do
		if [[ $alias == "$cur_"* ]]; then
			COMPREPLY+=("$pfx$alias ")
		fi
	done
}

__tig_complete_revisions () {
	local cur_="${1-$cur}" pfx="${2-}"
	__git_complete_refs --cur="$cur_" --pfx="$pfx"
	__tig_complete_revision_aliases "$cur_" "$pfx"
}

# __gitcomp overwrites the leading entries of COMPREPLY instead of appending
# to it, so let it fill an empty array and put what was there back in front.
__tig_complete_options () {
	local completed=("${COMPREPLY[@]}")

	COMPREPLY=()
	__gitcomp "$1"
	COMPREPLY=("${completed[@]}" "${COMPREPLY[@]}")
}

__tig_main () {
	local i c=1 command dashdash __git_repo_path __git_C_args

	# past a --, tig parses nothing of its own: what follows names paths
	for ((i = 1; i < cword; i++)); do
		if [ "${words[i]}" = "--" ]; then
			dashdash=yes
			break
		fi
	done

	if [ -z "$dashdash" ]; then
		case "$prev" in
		-C)
			# a directory, which bash completes on its own
			return
			;;
		esac
		# tig parses the revision of these itself, so git never sees it
		case "$prev" in
		--bdiff|--bdiff-base|--bdiff-onto)
			__tig_complete_revisions
			return
			;;
		esac
		case "$cur" in
		--bdiff=*|--bdiff-base=*|--bdiff-onto=*)
			__tig_complete_revisions "${cur#*=}" "${cur%%=*}="
			return
			;;
		esac
	fi

	# parse already existing parameters
	while [ $c -lt $cword ]; do
		i="${words[c]}"
		case "$i" in
		--)	command="log"; break;;
		--bdiff-base|--bdiff-onto)
			c=$((++c));;	# skips the revision it takes
		-C)	# tig runs from there, so git must be asked from there
			case "${words[c+1]-}" in
			-*|"")	;;
			*)	c=$((++c))
				__git_C_args=(-C "${words[c]}");;
			esac;;
		--bdiff)
			# the revision is optional: tig takes the next
			# parameter only when it is not an option
			case "${words[c+1]-}" in
			-*|"")	;;
			*)	c=$((++c));;
			esac;;
		-*)	;;
		*)	command="$i"; break ;;
		esac
		c=$((++c))
	done

	# commands
	case "$command" in
		refs|status|stash)
			__gitcomp "$__tig_options"
			return
			;;
		grep)
			# takes a pattern, not a revision
			__git_complete_command grep
			return
			;;
		reflog)
			__git_complete_command log
			;;
		"")
			__git_complete_command log
			__tig_complete_options \
				"$__tig_options $__tig_bdiff_options $__tig_commands"
			;;
		*)
			__git_complete_command $command
			;;
	esac

	# the revisions git just completed can also be spelled tig's way
	if [ -z "$dashdash" ]; then
		__tig_complete_revision_aliases
	fi
}

# we use internal git-completion functions, so wrap _tig for all necessary
# variables (like cword and prev) to be defined
__git_complete tig __tig_main

# The following are necessary only for Cygwin, and only are needed
# when the user has tab-completed the executable name and consequently
# included the '.exe' suffix.
if [ Cygwin = "$(uname -o 2>/dev/null)" ]; then
	__git_complete tig.exe __tig_main
fi
