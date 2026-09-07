#!/bin/bash
#
# Build the macOS developer binary and keep the local .app bundle in step
# with it. Installation in /Applications is optional and must be requested.
#
# There are three ways to end up running code you did not just build, and all
# three have happened:
#
#   1. plain `make` leaves MCP out, so the binary does not recognise --mcp-http
#      and prints usage instead - which reads as a failed launch;
#   2. `make` builds the bare binary but not the bundle, and .mcp.json and the
#      Finder both launch the bundle, so the old one keeps being used;
#   3. a GUI instance can still read resources from its bundle after startup,
#      so replacing that bundle under it can mix two builds. A headless process
#      does not consume GUI resources and may safely keep running from its old
#      mapped executable while the next bundle is installed atomically.
#
# This script builds and verifies the local bundle, then says what the two
# binaries actually are. It never changes /Applications unless --install is
# passed explicitly.
#
# Usage:  platforms/macos/build.sh              build and local bundle only
#         platforms/macos/build.sh --no-mcp     build without the MCP server
#         platforms/macos/build.sh --no-sprite-expander
#         platforms/macos/build.sh --install    also install in /Applications
#         BUILD_JOBS=4 platforms/macos/build.sh
#         BUILD_HEARTBEAT_SECONDS=5 platforms/macos/build.sh
#         GEARSF7000_INSTALL_DIR=/path platforms/macos/build.sh

set -u

cd "$(dirname "$0")" || exit 1

JOBS="${BUILD_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
ENABLE_MCP=1
ENABLE_SPRITE_EXPANDER=1
INSTALL_APP=0
for arg in "$@"; do
    case "$arg" in
        --no-mcp) ENABLE_MCP=0 ;;
        --no-sprite-expander) ENABLE_SPRITE_EXPANDER=0 ;;
        --install) INSTALL_APP=1 ;;
        # Retained for compatibility; local-only is now the default.
        --no-install) INSTALL_APP=0 ;;
        *) echo "build.sh: unknown option '$arg'" >&2; exit 2 ;;
    esac
done

BARE="gearsf7000"
LOCAL_APP="GearSF7000.app"
BUNDLE="$LOCAL_APP/Contents/MacOS/gearsf7000"
INSTALL_DIR="${GEARSF7000_INSTALL_DIR:-/Applications}"
INSTALLED_APP="$INSTALL_DIR/GearSF7000.app"
INSTALLED_BINARY="$INSTALLED_APP/Contents/MacOS/gearsf7000"

# A GUI instance can still lazy-load resources from its bundle, so it blocks
# replacement. A headless instance only keeps its old executable mapped: it is
# reported but may remain alive while a new bundle is installed for later runs.
running_instances()
{
    pgrep -f "GearSF7000.app/Contents/MacOS/gearsf7000|/gearsf7000( |$)" 2>/dev/null
}

classify_instances()
{
    HEADLESS_PIDS=""
    GUI_PIDS=""

    local pid command
    for pid in $(running_instances); do
        command="$(ps -p "$pid" -o command= 2>/dev/null)"
        [ -n "$command" ] || continue
        case "$command" in
            *--headless*) HEADLESS_PIDS="$HEADLESS_PIDS $pid" ;;
            *)            GUI_PIDS="$GUI_PIDS $pid" ;;
        esac
    done
}

stamp_of()
{
    # The compile timestamp is printed into the binary by src/build_info.cpp.
    [ -f "$1" ] || { echo "missing"; return; }
    local when
    when="$(strings "$1" 2>/dev/null | grep -E '^[0-9]{2}:[0-9]{2}:[0-9]{2}$' | head -1)"
    [ -n "$when" ] && echo "$when" || echo "unknown"
}

identity_of()
{
    [ -x "$1" ] || { echo "missing"; return; }
    "$1" --version 2>/dev/null | awk '/^Build: / || /^Compiled: /'
}

run_with_heartbeat()
(
    local label="$1"
    shift

    local interval="${BUILD_HEARTBEAT_SECONDS:-5}"
    case "$interval" in
        ''|*[!0-9]*|0) interval=5 ;;
    esac

    "$@" &
    local child=$!
    local elapsed=0
    local status

    trap 'kill -TERM "$child" 2>/dev/null; wait "$child" 2>/dev/null; exit 130' INT TERM HUP
    while kill -0 "$child" 2>/dev/null; do
        sleep 1
        elapsed=$((elapsed + 1))
        if (( elapsed % interval == 0 )) && kill -0 "$child" 2>/dev/null; then
            printf '    ... %s still running (%ss)\n' "$label" "$elapsed"
        fi
    done

    wait "$child"
    status=$?
    trap - INT TERM HUP
    return "$status"
)

cleanup_stage()
{
    local stage="$1"
    # Never let a malformed or empty variable become an rm target.
    if [[ -n "$stage" && "$stage" == "$INSTALL_DIR"/.GearSF7000-install.* ]]; then
        rm -rf -- "$stage"
    else
        echo "build.sh: refusing to clean unexpected path '$stage'" >&2
        return 1
    fi
}

install_bundle()
{
    case "$INSTALL_DIR" in
        /*) ;;
        *) echo "build.sh: install directory must be absolute: $INSTALL_DIR" >&2; return 1 ;;
    esac

    [ -d "$INSTALL_DIR" ] || {
        echo "build.sh: install directory does not exist: $INSTALL_DIR" >&2
        return 1
    }

    local stage_root staged_app staged_binary previous_app expected actual
    stage_root="$(mktemp -d "$INSTALL_DIR/.GearSF7000-install.XXXXXX")" || {
        echo "build.sh: cannot create an installation stage in $INSTALL_DIR" >&2
        return 1
    }
    staged_app="$stage_root/GearSF7000.app"
    staged_binary="$staged_app/Contents/MacOS/gearsf7000"
    previous_app="$stage_root/previous.app"

    if ! ditto "$LOCAL_APP" "$staged_app"; then
        echo "build.sh: failed to stage the bundle" >&2
        cleanup_stage "$stage_root"
        return 1
    fi

    expected="$(identity_of "$BUNDLE")"
    actual="$(identity_of "$staged_binary")"
    if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
        echo "build.sh: staged bundle identity does not match the local bundle" >&2
        printf '    expected: %s\n' "$expected" >&2
        printf '    staged:   %s\n' "$actual" >&2
        cleanup_stage "$stage_root"
        return 1
    fi

    if ! codesign --verify --deep --strict "$staged_app"; then
        echo "build.sh: staged bundle failed code-signature verification" >&2
        cleanup_stage "$stage_root"
        return 1
    fi

    if [ -e "$INSTALLED_APP" ]; then
        if ! mv "$INSTALLED_APP" "$previous_app"; then
            echo "build.sh: cannot move the previous installed bundle aside" >&2
            cleanup_stage "$stage_root"
            return 1
        fi
    fi

    if ! mv "$staged_app" "$INSTALLED_APP"; then
        echo "build.sh: cannot install the staged bundle" >&2
        [ ! -e "$previous_app" ] || mv "$previous_app" "$INSTALLED_APP"
        cleanup_stage "$stage_root"
        return 1
    fi

    actual="$(identity_of "$INSTALLED_BINARY")"
    if [ "$expected" != "$actual" ] || \
       ! codesign --verify --deep --strict "$INSTALLED_APP"; then
        echo "build.sh: installed bundle failed verification; restoring previous app" >&2
        mv "$INSTALLED_APP" "$stage_root/rejected.app"
        [ ! -e "$previous_app" ] || mv "$previous_app" "$INSTALLED_APP"
        cleanup_stage "$stage_root"
        return 1
    fi

    cleanup_stage "$stage_root"
    return 0
}

echo "==> building (ENABLE_MCP=$ENABLE_MCP, ENABLE_SPRITE_EXPANDER=$ENABLE_SPRITE_EXPANDER, -j$JOBS)"
# The Makefile's stale-bundle warning is for someone running make by hand.
# Here it would fire and then be answered two lines later by this script
# bundling anyway, which just teaches you to ignore warnings.
if ! run_with_heartbeat "build/link" make SKIP_BUNDLE_CHECK=1 ENABLE_MCP=$ENABLE_MCP ENABLE_SPRITE_EXPANDER=$ENABLE_SPRITE_EXPANDER all -j"$JOBS"; then
    echo "build.sh: the build failed - bundle left untouched" >&2
    exit 1
fi

classify_instances
if [ -n "$GUI_PIDS" ]; then
    echo ""
    echo "==> NOT bundling: a GUI instance is running (pid$GUI_PIDS)"
    echo "    A GUI process may read bundle resources after startup, so replacing"
    echo "    the bundle underneath it could mix files from two builds."
    echo "    Close it and run this script again."
    # Do not let callers mistake a deliberately skipped install for success.
    exit 3
fi

if [ -n "$HEADLESS_PIDS" ]; then
    echo ""
    echo "==> headless instance still running (pid$HEADLESS_PIDS)"
    echo "    It is not stopped and does not block bundling or installation."
    echo "    It remains on its currently mapped build until it is restarted;"
    echo "    newly launched instances will use the bundle installed below."
fi

echo "==> bundling"
if ! run_with_heartbeat "bundle/sign" make ENABLE_MCP=$ENABLE_MCP ENABLE_SPRITE_EXPANDER=$ENABLE_SPRITE_EXPANDER bundle; then
    echo "build.sh: the bundle step failed" >&2
    exit 1
fi

echo ""
echo "==> result"
printf '    git     %s\n' "$(git describe --abbrev=7 --dirty --always --tags 2>/dev/null)"
printf '    binary  compiled %s\n' "$(stamp_of "$BARE")"
printf '    bundle  compiled %s\n' "$(stamp_of "$BUNDLE")"

if [ "$(stamp_of "$BARE")" != "$(stamp_of "$BUNDLE")" ]; then
    echo ""
    echo "    WARNING: the two do not match. Whatever launches the .app is"
    echo "    running different code from the binary just built." >&2
    exit 1
fi

if [ "$INSTALL_APP" = "1" ]; then
    echo ""
    echo "==> installing $INSTALLED_APP"
    if ! run_with_heartbeat "install/verify" install_bundle; then
        echo "build.sh: installation failed" >&2
        exit 1
    fi
    printf '    installed %s\n' "$(identity_of "$INSTALLED_BINARY" | tr '\n' ' ')"
else
    echo ""
    echo "==> local bundle ready; /Applications left untouched"
    echo "    Install explicitly when wanted with: $0 --install"
fi

if [ "$ENABLE_MCP" = "1" ]; then
    echo ""
    if [ "$INSTALL_APP" = "1" ]; then
        echo "    MCP ready:  open -n '$INSTALLED_APP' --args --mcp-http"
    else
        echo "    MCP ready:  open -n '$LOCAL_APP' --args --mcp-http"
    fi
fi
