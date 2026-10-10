#!/bin/bash
# proton_run.sh <exe> [args...]: run a cross-built Windows test under Proton
# (umu-run) and exit with its exit code. Meant as CMAKE_CROSSCOMPILING_EMULATOR,
# so ctest can run the WIN32 tests on a Linux host:
#   cmake ... -DCMAKE_CROSSCOMPILING_EMULATOR=<toolkit>/tests/proton_run.sh
#   ctest --test-dir <build>/tests/d3d8_hlsl_split
#
# stdout/stderr go through cmd.exe into a file (plain umu-run drops the
# program's stderr) and are printed afterwards. Inside a distrobox, umu-run
# is reached through distrobox-host-exec. RECOMP_* and HLSL_* variables are
# passed on explicitly.
#
# Environment: WINEPREFIX (default ~/.local/share/proton-test), PROTONPATH
# (GE-Proton), GAMEID (umu-default), PROTON_RUN_TIMEOUT (180 s),
# PROTON_RUN_LOCK (a file to flock around the run; unset: no lock).
set -u
exe=$(readlink -f "$1"); shift
out=$(mktemp /tmp/proton_run.XXXXXX)
winpath() { echo "Z:$1" | tr / '\\'; }
cmdline="$(winpath "$exe")"
for a in "$@"; do cmdline="$cmdline $a"; done

envs=(WINEPREFIX="${WINEPREFIX:-$HOME/.local/share/proton-test}"
      PROTONPATH="${PROTONPATH:-GE-Proton}" GAMEID="${GAMEID:-umu-default}")
while IFS= read -r kv; do envs+=("$kv"); done < <(env | grep -E '^(RECOMP|HLSL)_' || true)

run=(env "${envs[@]}" timeout -k 10 -s INT "${PROTON_RUN_TIMEOUT:-180}"
     umu-run cmd.exe /v:on /c "$cmdline > $(winpath "$out") 2>&1 & echo proton_run_exit=!ERRORLEVEL!>> $(winpath "$out")")
command -v umu-run >/dev/null || run=(distrobox-host-exec "${run[@]}")
cd "$(dirname "$exe")" || exit 2
if [ -n "${PROTON_RUN_LOCK:-}" ]; then
    flock -w 3600 "$PROTON_RUN_LOCK" "${run[@]}" </dev/null >/dev/null 2>&1
else
    "${run[@]}" </dev/null >/dev/null 2>&1
fi
rc=$(grep -a -o 'proton_run_exit=[0-9-]*' "$out" | tail -1 | cut -d= -f2)
grep -a -v 'proton_run_exit=' "$out"
rm -f "$out"
[ -n "$rc" ] || { echo "proton_run: no exit code (run failed or timed out)"; exit 125; }
exit "$rc"
