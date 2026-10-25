#!/bin/sh
set -eu

failed=0

check_pattern() {
  label=$1
  pattern=$2

  matches=$(grep -RInE --include='*.c' "$pattern" src || true)
  if [ -n "$matches" ]; then
    echo "ERROR: forbidden raw error-reporting pattern: $label"
    echo "$matches"
    echo
    failed=1
  fi
}

# Raw perror() loses structured/domain context and source metadata.
check_pattern "perror()" '(^|[^A-Za-z0-9_])perror[[:space:]]*\('

# errno is volatile. Capture it immediately before any other call.
check_pattern "strerror(errno)" 'strerror[[:space:]]*\([[:space:]]*errno[[:space:]]*\)'

if [ "$failed" -ne 0 ]; then
  echo "Use TB_REPORT_ERRNO/TB_REPORT_ERROR or save errno immediately and log rich context."
  exit 1
fi

echo "Error-locality audit passed."
