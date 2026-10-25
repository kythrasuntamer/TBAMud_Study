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

# Direct stdio-to-stderr error reporting bypasses severity/category/source fields.
check_pattern "fprintf(stderr,...)" '(^|[^A-Za-z0-9_])fprintf[[:space:]]*\([[:space:]]*stderr[[:space:]]*,'
check_pattern "fputs(...,stderr)" '(^|[^A-Za-z0-9_])fputs[[:space:]]*\([^;]*,[[:space:]]*stderr[[:space:]]*\)'
check_pattern "printf SYSERR" '(^|[^A-Za-z0-9_])printf[[:space:]]*\([[:space:]]*"SYSERR'

if [ "$failed" -ne 0 ]; then
  echo "Use TB_REPORT_DOMAIN_ERRNO/TB_REPORT_DOMAIN_ERROR (or the compatibility TB_REPORT_* wrappers) so failures retain severity, category, source location, and the original error code."
  exit 1
fi

echo "Error-locality audit passed."
