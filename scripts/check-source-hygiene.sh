#!/usr/bin/env bash
# Copyright (c) Corsair Dynamic Lighting Bridge contributors.
# SPDX-License-Identifier: MIT
#
# Source hygiene checks that are cheap to run and annoying to discover late.
#
#   1. No non-ASCII bytes in source.  A stray non-ASCII character in a C++ file
#      is a compile error on some toolchains and an invisible corruption on
#      others; in an INF it can break parsing outright.
#   2. No tabs in C/C++ sources (the codebase uses spaces throughout).
#   3. No trailing whitespace.
#   4. Every source file carries an SPDX licence identifier.
#
# Run from the repository root.  CI runs this too.

set -uo pipefail
cd "$(dirname "$0")/.."

status=0
note() { printf '%s\n' "$*" >&2; }

# Both tracked AND untracked-but-not-ignored files.  Checking only tracked
# files means a brand new file is silently skipped until after it has been
# committed -- which is exactly when the check is least useful.
patterns=('*.h' '*.c' '*.cpp' '*.inf' '*.vcxproj' '*.txt' '*.md' '*.ps1' '*.sh' '*.yml')
sources=$( { git ls-files "${patterns[@]}" 2>/dev/null;
             git ls-files --others --exclude-standard "${patterns[@]}" 2>/dev/null;
           } | sort -u || true)

if [ -z "$sources" ]; then
    note "no tracked source files found"
    exit 0
fi

# 1. non-ASCII
if hits=$(grep -PnI '[^\x00-\x7F]' $sources 2>/dev/null); then
    if [ -n "$hits" ]; then
        note "ERROR: non-ASCII characters found:"
        note "$hits"
        status=1
    fi
fi

# 2. tabs in C/C++ sources
code=$( { git ls-files '*.h' '*.c' '*.cpp' 2>/dev/null;
          git ls-files --others --exclude-standard '*.h' '*.c' '*.cpp' 2>/dev/null;
        } | sort -u || true)
if [ -n "$code" ]; then
    if hits=$(grep -PnI '\t' $code 2>/dev/null); then
        if [ -n "$hits" ]; then
            note "ERROR: tab characters in C/C++ sources:"
            note "$hits"
            status=1
        fi
    fi
fi

# 3. trailing whitespace
if hits=$(grep -nI ' $' $sources 2>/dev/null); then
    if [ -n "$hits" ]; then
        note "ERROR: trailing whitespace:"
        note "$hits"
        status=1
    fi
fi

# 4. SPDX identifier on every C/C++ source
for file in $code; do
    if ! grep -q 'SPDX-License-Identifier' "$file"; then
        note "ERROR: missing SPDX-License-Identifier: $file"
        status=1
    fi
done

if [ "$status" -eq 0 ]; then
    echo "source hygiene: OK"
fi
exit "$status"
