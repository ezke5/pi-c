# SPDX-License-Identifier: GPL-3.0-or-later
#
# Bash completion for pi.

_pi()
{
    local cur prev words cword
    _init_completion || return

    local commands='build sync install remove upgrade autoremove search verify info clean index'
    if [[ ${cword} -eq 1 ]]; then
        COMPREPLY=($(compgen -W '--root --nodeps --keep --output --help --version' -- "$cur"))
        COMPREPLY+=($(compgen -W "$commands" -- "$cur"))
        return
    fi

    if [[ ${words[1]} == --root ]]; then
        if [[ ${cword} -eq 2 ]]; then
            _filedir -d
            return
        fi
        local command_index=3
    else
        local command_index=1
    fi

    if [[ ${cword} -eq ${command_index} ]]; then
        COMPREPLY=($(compgen -W "$commands" -- "$cur"))
        return
    fi

    case ${words[command_index]} in
        clean)
            if [[ $prev == --keep ]]; then
                COMPREPLY=($(compgen -W '0 1 2 3 4 5' -- "$cur"))
            else
                COMPREPLY=($(compgen -W '--keep' -- "$cur"))
            fi
            ;;
        build|install)
            _filedir
            ;;
        remove|verify|info|search)
            COMPREPLY=($(compgen -W '' -- "$cur"))
            ;;
        sync)
            COMPREPLY=($(compgen -W '' -- "$cur"))
            ;;
    esac
}

complete -F _pi pi
