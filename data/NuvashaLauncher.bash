# bash completion for Nuvasha Launcher
_nuvashalauncher()
{
    local cur prev
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    case "$prev" in
        -l|--launch|--switch-account|--remove-account|--remove-instance|--account)
            return 0
            ;;
        -i|--import)
            compopt -o filenames
            COMPREPLY=( $(compgen -f -- "$cur") )
            return 0
            ;;
    esac

    COMPREPLY=( $(compgen -W \
        "--help --version --import --launch --list-instances --list-accounts \
         --switch-account --remove-account --remove-instance --account" \
        -- "$cur") )
}
complete -F _nuvashalauncher NuvashaLauncher
