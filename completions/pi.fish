# SPDX-License-Identifier: GPL-3.0-or-later
# Fish completion for pi.

complete -c pi -f -n '__fish_use_subcommand' -l root -r -d 'operate on an alternate root'
complete -c pi -f -n '__fish_use_subcommand' -a 'build sync install remove upgrade autoremove search verify info clean index'
complete -c pi -f -n '__fish_use_subcommand' -l help -d 'show help'
complete -c pi -f -n '__fish_use_subcommand' -l version -d 'show version'
complete -c pi -f -n '__fish_use_subcommand' -l nodeps -d 'skip dependencies'

complete -c pi -n '__fish_seen_subcommand_from build' -a '(__fish_complete_path)' -d 'package name'
complete -c pi -n '__fish_seen_subcommand_from install' -a '(__fish_complete_path)' -d 'package archive'
complete -c pi -n '__fish_seen_subcommand_from remove verify info' -d 'package name'
complete -c pi -n '__fish_seen_subcommand_from clean' -l keep -r -d 'number of cache generations to keep'
complete -c pi -n '__fish_seen_subcommand_from index' -l output -r -d 'index output file'
