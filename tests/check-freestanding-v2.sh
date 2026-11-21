#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Audit installed Common V2 API and unchanged embedded firmware ELF, no BOOT.
set -euo pipefail
module=$(realpath "${1:?MINIX.so required}")
test -s "$module"
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sh "$root/tests/check_production_v2.sh" "$module"
readelf -SW "$module" | grep -F .infiltrator_boot >/dev/null
work=$(mktemp -d);trap 'rm -rf -- "$work"' EXIT
objcopy --dump-section ".infiltrator_boot=$work/firmware.so" "$module" "$work/carrier.so"
test -s "$work/firmware.so"
readelf -hW "$work/firmware.so" | grep -F 'DYN (Shared object file)' >/dev/null
! readelf -dW "$work/firmware.so" | grep -F '(NEEDED)'
! nm -u "$work/firmware.so" | grep .
nm -D --defined-only "$work/firmware.so" | grep -F infiltrator_filesystem_api_v2 >/dev/null
if readelf -Wr "$work/firmware.so" | grep 'R_X86_64_' | grep -v 'R_X86_64_RELATIVE'; then
 echo "Unsupported freestanding relocation" >&2;exit 1
fi
echo MINIX_COMMON_V2_PACKAGED_READER_PASS
