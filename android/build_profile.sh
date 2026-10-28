#!/usr/bin/env bash
# Shared CLI selection. Source after entering dev, then call totk_build_options with the remaining arguments.
totk_build_options() {
    TOTK_BUILD_VARIANT=${TOTK_BUILD_VARIANT:-}
    TOTK_BUILD_PROFILE=${TOTK_BUILD_PROFILE:-}
    while (($#)); do
        case "$1" in
            debug|release) TOTK_BUILD_VARIANT=$1; shift ;;
            --perftest) TOTK_PERFORMANCE_TEST=true; shift ;;
            --variant|--profile)
                if (($# < 2)); then echo "missing value for $1" >&2; return 2; fi
                if [[ $1 == --variant ]]; then TOTK_BUILD_VARIANT=$2; else TOTK_BUILD_PROFILE=$2; fi
                shift 2 ;;
            *) echo "unknown build option: $1" >&2; return 2 ;;
        esac
    done
    if [[ -z $TOTK_BUILD_VARIANT ]]; then
        if [[ -n $TOTK_BUILD_PROFILE && $TOTK_BUILD_PROFILE != dev ]]; then
            TOTK_BUILD_VARIANT=release
        else
            TOTK_BUILD_VARIANT=debug
        fi
    fi
    case "$TOTK_BUILD_VARIANT" in debug|release) ;; *) echo "variant must be debug or release" >&2; return 2 ;; esac
    if [[ -z $TOTK_BUILD_PROFILE ]]; then
        if [[ $TOTK_BUILD_VARIANT == release ]]; then TOTK_BUILD_PROFILE=performance; else TOTK_BUILD_PROFILE=dev; fi
    fi
    case "$TOTK_BUILD_PROFILE" in dev|performance|snapdragon-8elite) ;; *) echo "unknown profile: $TOTK_BUILD_PROFILE" >&2; return 2 ;; esac
    TOTK_PACKAGE=com.totkrecomp.android
    if [[ ${TOTK_PERFORMANCE_TEST:-false} == true ]]; then
        TOTK_PACKAGE+=.perftest
    elif [[ $TOTK_BUILD_VARIANT == debug ]]; then
        TOTK_PACKAGE+=.debug
    fi
    TOTK_PACKAGE_DIR="$TOTK_ROOT/.work/totk-port/packages/$TOTK_BUILD_VARIANT-$TOTK_BUILD_PROFILE"
    [[ ${TOTK_PERFORMANCE_TEST:-false} != true ]] || TOTK_PACKAGE_DIR+=-perftest
    export TOTK_BUILD_VARIANT TOTK_BUILD_PROFILE TOTK_PACKAGE TOTK_PACKAGE_DIR TOTK_PERFORMANCE_TEST
}
