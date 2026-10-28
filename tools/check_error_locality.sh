#!/bin/sh
# Error-locality audit for src/*.c. Run from anywhere: sh tools/check_error_locality.sh
#
# Two kinds of rule:
#   forbid  - the pattern must not appear at all.
#   ratchet - legacy code still has some; the count may fall but never rise.
#             When you remove one, lower the limit here in the same commit
#             (the script says so), so the fix cannot quietly come back.
# See docs/maintainer-safety-guide.md for the TB_ERROR_* / TB_REPORT_* rules.
set -eu

cd "$(dirname "$0")/.."

failed=0

matches_for() {
  grep -RInE --include='*.c' "$1" src || true
}

forbid() {
  label=$1
  pattern=$2

  matches=$(matches_for "$pattern")
  if [ -n "$matches" ]; then
    echo "ERROR: forbidden raw error-reporting pattern: $label"
    echo "$matches"
    echo
    failed=1
  fi
}

ratchet() {
  label=$1
  limit=$2
  pattern=$3

  matches=$(matches_for "$pattern")
  if [ -z "$matches" ]; then
    count=0
  else
    count=$(printf '%s\n' "$matches" | wc -l | tr -d ' ')
  fi
  if [ "$count" -gt "$limit" ]; then
    echo "ERROR: $label: $count occurrences, limit $limit. New code must use TB_ERROR_*/TB_REPORT_*."
    echo "  List them:   grep -RInE --include='*.c' '$pattern' src"
    echo "  Find yours:  git diff -U0 master -- src | sed -n 's/^+//p' | grep -E '$pattern'"
    echo
    failed=1
  elif [ "$count" -lt "$limit" ]; then
    echo "NOTE: $label: $count occurrences, limit $limit. Lower the limit in tools/check_error_locality.sh to $count."
  fi
}

# ---- forbid: raw reporting that bypasses severity/category/source fields ----

# Raw perror() loses structured/domain context and source metadata.
forbid "perror()" '(^|[^A-Za-z0-9_])perror[[:space:]]*\('

# errno is volatile. Capture it immediately before any other call.
forbid "strerror(errno)" 'strerror[[:space:]]*\([[:space:]]*errno[[:space:]]*\)'

# Direct stdio/fd-2 error reporting bypasses severity/category/source fields.
forbid "fprintf(stderr,...)" '(^|[^A-Za-z0-9_])fprintf[[:space:]]*\([[:space:]]*stderr[[:space:]]*,'
forbid "vfprintf(stderr,...)" '(^|[^A-Za-z0-9_])vfprintf[[:space:]]*\([[:space:]]*stderr[[:space:]]*,'
forbid "fputs(...,stderr)" '(^|[^A-Za-z0-9_])fputs[[:space:]]*\([^;]*,[[:space:]]*stderr[[:space:]]*\)'
forbid "fputc(...,stderr)" '(^|[^A-Za-z0-9_])(fputc|putc)[[:space:]]*\([^;]*,[[:space:]]*stderr[[:space:]]*\)'
forbid "fwrite(...,stderr)" '(^|[^A-Za-z0-9_])fwrite[[:space:]]*\([^;]*,[[:space:]]*stderr[[:space:]]*\)'
forbid "write(2/STDERR_FILENO,...)" '(^|[^A-Za-z0-9_])write[[:space:]]*\([[:space:]]*(2|STDERR_FILENO)[[:space:]]*,'
forbid "printf SYSERR" '(^|[^A-Za-z0-9_])printf[[:space:]]*\([[:space:]]*"SYSERR'

# The operation of a TB_ERROR_* / TB_WARN_MSG / TB_FATAL_MSG diagnostic is a
# stable kebab-case identifier that log searches key on ("dg-wait-parse"),
# not prose. Checked when subsystem and operation are literals on one line.
forbid "non-kebab-case TB_ERROR_*/TB_WARN_MSG/TB_FATAL_MSG operation" \
  'TB_(ERROR_MSG|ERROR_ERRNO|WARN_MSG|FATAL_MSG)\([[:space:]]*"[^"]*",[[:space:]]*"([^"]*[^a-z0-9"-][^"]*)?"'

# ---- ratchet: legacy patterns, may only shrink ----

# A literal "SYSERR" line carries no severity, category or source location.
ratchet 'puts("SYSERR...")' 5 '(^|[^A-Za-z0-9_])puts[[:space:]]*\([[:space:]]*"SYSERR'
ratchet 'log("SYSERR...")' 222 '(^|[^A-Za-z0-9_])log[[:space:]]*\([[:space:]]*"SYSERR'
ratchet 'mudlog(..."SYSERR...")' 50 '(^|[^A-Za-z0-9_])mudlog[[:space:]]*\([^;]*"SYSERR'

# unlink/remove/rename as a bare statement discards the result: a file that
# should be gone (or renamed into place) silently is not. A bare statement is
# a line that starts with the call and either ends it with ';' or carries its
# arguments on to the next line. A checked call that continues a longer
# condition, such as the line "    rename(to, from) != 0)", is not counted.
ratchet "unchecked unlink/remove/rename" 1 \
  '^[[:space:]]*(unlink|remove|rename)[[:space:]]*\(([^;]*\)[[:space:]]*;|[^;]*,[[:space:]]*$|[[:space:]]*$)'

# TB_REPORT_* operations name the failing call or step ("fopen",
# "parse-options"); prose with spaces cannot be searched for reliably.
ratchet "TB_REPORT_* operation containing spaces" 0 \
  'TB_REPORT_(ERRNO|ERROR)\([[:space:]]*"[^"]* [^"]*"|TB_REPORT_DOMAIN_(ERRNO|ERROR)\([^,]*,[^,]*,[[:space:]]*"[^"]* [^"]*"'

if [ "$failed" -ne 0 ]; then
  echo "Use TB_ERROR_MSG/TB_ERROR_ERRNO/TB_WARN_MSG/TB_FATAL_MSG or TB_REPORT_DOMAIN_ERRNO/TB_REPORT_DOMAIN_ERROR so failures retain severity, category, source location, a kebab-case operation, and the original error code."
  exit 1
fi

echo "Error-locality audit passed."
