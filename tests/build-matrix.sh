#!/bin/bash
# build-matrix.sh — cross-kernel compile validation for the BrieFS module.
#
# Builds briefs_fs.ko against every kernel series Debian trixie provides
# (6.12 from trixie proper, the backports series from trixie-backports)
# plus the upstream linux-master tree.  Compile-only by design: runtime
# validation stays on the 6.12 test VM, and any API drift on the other
# kernels is caught here by the compiler (see compat/compat.h — the
# build matrix is the feature probe for the version-guard boundaries).
#
# Usage (host, or the trixie VM — anywhere Debian trixie with trixie-
#   backports configured; every header package installs in both):
#   bash tests/build-matrix.sh [target ...]
# In the VM, /boot sits on the root filesystem, so the linux-image
# packages the 6.16/6.17 backports headers hard-depend on are harmless
# there; on the host the 456M /boot partition is the constraint, which
# is why the VM is the preferred place to run it.
# With no arguments, runs all targets.  Targets:
#   6.12            the running trixie-proper kernel's headers; when the
#                   running kernel is a custom build not in apt (the VM's
#                   -lockdep/-kasan kernels), the newest stock 6.12.x
#                   point release in the apt index instead
#   6.16 6.17 6.18 6.19 7.0 7.1   trixie-backports series (bare series
#                   names resolve to the newest point release in the
#                   apt index; full point versions also accepted)
#   master          the prepared upstream tree (default /home/jeremy/src/linux
#                   on the host, /kernel-src/linux in the VM, where the
#                   same tree is NFS-mounted; override with BRIEFS_MASTER_DIR)
#
# Output: an aligned PASS/FAIL table on stdout, saved to
#   tests/build-matrix-results/<date>-<git-describe>.txt
# with per-target build logs beside it.  Exit status: nonzero if any
# target failed.
#
# Every target also asserts the compile-time value of
# BRIEFS_HAS_CGROUPWB_FIX (printed as cgwb=0/1).  The probe builds a
# throwaway module force-including the real compat/compat.h, and its
# result is compared against an independent boundary computation from
# the target's own utsrelease.h -- the two disagreeing is a FAIL,
# because that means the flag's version range and the bugfix record
# have drifted apart (see compat/compat.h: keyed to stable-backport
# patchlevels, 6.12.96/6.18.39/7.1.4+).
#
# One-time host prep, recorded for reproducibility:
#   - trixie-backports enabled in /etc/apt/sources.list.
#   - the master tree prepared once: `make defconfig` (x86_64 defconfig
#     enables ext4, which selects CONFIG_BUFFER_HEAD and CONFIG_FS_IOMAP)
#     then `make modules_prepare`.  The per-run config probe below fails
#     with a named cause if either is missing.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(dirname "$SCRIPT_DIR")"
RESULTS_DIR="$SRC_DIR/tests/build-matrix-results"
BRIEFS_MASTER_DIR="${BRIEFS_MASTER_DIR:-/home/jeremy/src/linux}"
# The trixie VM sees the same prepared tree over NFS at /kernel-src/linux.
[ -d "$BRIEFS_MASTER_DIR" ] || BRIEFS_MASTER_DIR=/kernel-src/linux

# trixie-backports newest point release per series, as of the apt index
# at authoring time; a bare-series target re-resolves this live.
DEFAULT_SERIES="6.16 6.17 6.18 6.19 7.0 7.1"

apt_install() {  # apt_install [-t SUITE] PKG ... — sudo only when needed
    if [ "$(id -u)" -eq 0 ]; then
        apt-get install -y "$@"
    else
        sudo apt-get install -y "$@"
    fi
}

pkg_installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q "install ok installed"; }

# config_ok CONF — BUFFER_HEAD (7.1+) and FS_IOMAP are selected only by
# in-tree filesystems, so a config without them cannot provide the
# metadata (buffer_head) or data (iomap) API BrieFS builds against.
# CONF is an autoconf.h ("#define CONFIG_X 1") or a .config
# ("CONFIG_X=y" / "# CONFIG_X is not set"); older kernels that predate
# the Kconfig symbol provide the API unconditionally, so an absent
# symbol passes and a disabled one fails.
config_ok() {
    local conf="$1" sym
    for sym in CONFIG_BUFFER_HEAD CONFIG_FS_IOMAP; do
        if grep -Eq "^#define ${sym} 1$" "$conf" || grep -Eq "^${sym}=y$" "$conf"; then
            continue
        fi
        if grep -Eq "^#define ${sym} " "$conf" || \
           grep -Eq "^${sym}=" "$conf" || \
           grep -Eq "^# ${sym} is not set" "$conf"; then
            return 1
        fi
    done
    return 0
}

# newest_apt_point SERIES — e.g. 6.16 -> 6.16.12+deb13, or 6.12 ->
# 6.12.107+deb13 (trixie proper; only the 6.16+ backports series live
# in trixie-backports, and the regex matches any enabled suite).  The
# full match excludes the -cloud/-rt/-common flavours by anchoring on
# the plain "-amd64" suffix.
newest_apt_point() {
    apt-cache search --names-only linux-headers 2>/dev/null |
        awk -v s="$1." '$1 ~ ("^linux-headers-" s "[0-9]+[+]deb13-amd64$") { print $1 }' |
        sort -V | tail -1 |
        sed -e 's/^linux-headers-//' -e 's/-amd64$//'
}

ensure_installed() {  # ensure_installed [-t SUITE] PKG ...
    local -a to_install=()
    local p
    for p in "$@"; do
        [ "$p" = "-t" ] && continue
        case "$p" in *-backports|trixie|stable) continue ;; esac
        pkg_installed "$p" || to_install+=("$p")
    done
    if [ ${#to_install[@]} -gt 0 ]; then
        echo "  installing: ${to_install[*]}"
        apt_install "$@" || return 1
    fi
    return 0
}

# cgwb_flag KDIR LOG — echo "0"/"1" as BRIEFS_HAS_CGROUPWB_FIX compiles
# on this KDIR.  A throwaway kbuild module force-includes the real
# compat/compat.h (so the probe sees exactly what the module build
# sees) and stringifies the flag into its rodata.  Returns nonzero if
# the probe build itself fails (log appended to LOG).
cgwb_flag() {
    local kdir="$1" log="$2" d
    d="$(mktemp -d)" || return 2
    {
        echo "obj-m += cgwbprobe.o"
        echo "ccflags-y += -include $SRC_DIR/compat/compat.h"
    } > "$d/Makefile"
    {
        echo '#include <linux/module.h>'
        echo '#include <linux/stringify.h>'
        echo 'const char *cgwb_probe = "CGWB=" __stringify(BRIEFS_HAS_CGROUPWB_FIX);'
        echo 'MODULE_LICENSE("GPL");'
    } > "$d/cgwbprobe.c"
    if ! make -C "$kdir" M="$d" >>"$log" 2>&1; then
        rm -rf "$d"
        return 2
    fi
    strings "$d/cgwbprobe.o" | grep -o 'CGWB=[01]' | head -1 | cut -d= -f2
    local rc=$?
    rm -rf "$d"
    return $rc
}

# expected_cgwb KDIR — the value BRIEFS_HAS_CGROUPWB_FIX must have on
# this kernel, computed independently of compat.h: the shell re-derives
# the both-bugs-fixed boundaries (>= 7.2, 7.1.y >= 7.1.4,
# 6.18.y >= 6.18.39, 6.12.y >= 6.12.96) from UTS_RELEASE.  Agreement of
# the two implementations is the assert; keep in sync with compat.h.
expected_cgwb() {
    local uts x y z
    uts="$(sed -n 's/^#define UTS_RELEASE "\(.*\)"/\1/p' \
        "$1/include/generated/utsrelease.h" 2>/dev/null)"
    [ -n "$uts" ] || return 2
    x="$(echo "$uts" | sed -E 's/^([0-9]+)\..*/\1/')"
    y="$(echo "$uts" | sed -E 's/^[0-9]+\.([0-9]+)\..*/\1/')"
    z="$(echo "$uts" | sed -E 's/^[0-9]+\.[0-9]+\.([0-9]+).*/\1/')"
    if [ "$x" -ge 7 ] 2>/dev/null; then
        { [ "$y" -ge 2 ] || { [ "$y" -eq 1 ] && [ "$z" -ge 4 ]; }; } && { echo 1; return; }
    elif [ "$x" -eq 6 ] 2>/dev/null; then
        { { [ "$y" -eq 18 ] && [ "$z" -ge 39 ]; } || \
          { [ "$y" -eq 12 ] && [ "$z" -ge 96 ]; }; } && { echo 1; return; }
    fi
    echo 0
}

run_target() {  # NAME KDIR — clean + modules build; echo "NAME STATUS"
    local name="$1" kdir="$2" autoconf status

    if [ ! -d "$kdir" ]; then
        echo "$name FAIL-NO-KDIR ($kdir)"
        return 1
    fi
    if [ -f "$kdir/include/generated/autoconf.h" ]; then
        autoconf="$kdir/include/generated/autoconf.h"
    elif [ -f "$kdir/.config" ]; then
        autoconf="$kdir/.config"
    else
        echo "$name FAIL-NO-CONFIG (neither autoconf.h nor .config under $kdir)"
        return 1
    fi
    if ! config_ok "$autoconf"; then
        echo "$name FAIL-CONFIG (BUFFER_HEAD/FS_IOMAP not enabled)"
        return 1
    fi

    local log="$RESULTS_DIR/$name.build.log"
    : > "$log"
    if make -C "$kdir" M="$SRC_DIR" clean >>"$log" 2>&1 && \
       make -C "$kdir" M="$SRC_DIR" modules >>"$log" 2>&1 && \
       [ -f "$SRC_DIR/briefs_fs.ko" ]; then
        local cgwb want
        if ! cgwb="$(cgwb_flag "$kdir" "$log")"; then
            echo "$name FAIL-CGWB-PROBE (flag probe build failed; see $name.build.log)"
            return 1
        fi
        want="$(expected_cgwb "$kdir")"
        if [ "$cgwb" != "$want" ]; then
            echo "$name FAIL-CGWB (flag=$cgwb expected=$want: compat.h/bugfix boundary drift)"
            return 1
        fi
        echo "$name PASS cgwb=$cgwb"
        return 0
    fi
    echo "$name FAIL-BUILD (see tests/build-matrix-results/$name.build.log)"
    return 1
}

mkdir -p "$RESULTS_DIR"
git_desc="$(git -C "$SRC_DIR" describe --always --dirty 2>/dev/null || echo unknown)"
report="$RESULTS_DIR/$(date +%Y%m%d-%H%M%S)-${git_desc}.txt"

targets=("$@")
if [ ${#targets[@]} -eq 0 ]; then
    targets=(6.12 $DEFAULT_SERIES master)
fi

{
    echo "BrieFS build matrix  $(date '+%Y-%m-%d %H:%M:%S')  git: $git_desc"
    echo
} > "$report"

overall=0
for t in "${targets[@]}"; do
    case "$t" in
    6.12)
        # The trixie-proper floor series.  Prefer the running kernel's
        # own header set when it is a stock +deb13 build (the host); a
        # custom kernel name (the VM's -lockdep/-kasan builds, which apt
        # cannot provide) falls back to the newest stock point release
        # in the apt index.
        case "$(uname -r)" in
        *+deb13*) ver="$(uname -r)" ;;
        *)        ver="$(newest_apt_point 6.12)-amd64" ;;
        esac
        if [ -z "$ver" ] || [ "$ver" = "-amd64" ]; then
            echo "$t FAIL-RESOLVE (no stock 6.12 headers in apt index)"
            overall=1
            continue
        fi
        # linux-kbuild is named by the full version (e.g.
        # linux-kbuild-6.12.48+deb13), unlike the series-named
        # linux-headers-common packages.
        ensure_installed "linux-headers-$ver" "linux-headers-${ver%-amd64}-common" \
            "linux-kbuild-${ver%-amd64}" || { echo "$t FAIL-INSTALL"; overall=1; continue; }
        run_target "6.12" "/usr/src/linux-headers-$ver"
        ;;
    master)
        run_target "master" "$BRIEFS_MASTER_DIR"
        ;;
    6.16|6.17|6.18|6.19|7.0|7.1)
        full="$(newest_apt_point "$t")"
        if [ -z "$full" ]; then
            echo "$t FAIL-RESOLVE (no +deb13 headers in apt index)"
            overall=1
            continue
        fi
        series="$(echo "$full" | cut -d. -f1,2)"
        ensure_installed -t trixie-backports \
            "linux-headers-$full-amd64" "linux-headers-$full-common" \
            "linux-kbuild-$full" || { echo "$t FAIL-INSTALL"; overall=1; continue; }
        run_target "$series" "/usr/src/linux-headers-$full-amd64"
        ;;
    6.16.*|6.17.*|6.18.*|6.19.*|7.0.*|7.1.*|*[0-9]*\+deb13*)
        full="${t%+deb13}+deb13"
        series="$(echo "$full" | cut -d. -f1,2)"
        ensure_installed -t trixie-backports \
            "linux-headers-$full-amd64" "linux-headers-$full-common" \
            "linux-kbuild-$full" || { echo "$t FAIL-INSTALL"; overall=1; continue; }
        run_target "$series" "/usr/src/linux-headers-$full-amd64"
        ;;
    *)
        echo "unknown target: $t" >&2
        overall=1
        continue
        ;;
    esac || overall=1
done > >(tee -a "$report")

echo | tee -a "$report"
echo "report: $report"
exit $overall