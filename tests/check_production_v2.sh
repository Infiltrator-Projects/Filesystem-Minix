#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
target="${1:?MINIX.so required}"
test -s "$target"
nm -D --defined-only "$target" | grep -E ' [TW] infiltrator_filesystem_api_v2$'
if nm -D --defined-only "$target" | grep -E 'infiltrator_filesystem_module_v[12]|infiltrator_filesystem_minix_defrag_v1|ifs_minix_(worker_main|fixture_run)|ldtm_raw_|ifs_minix_native_(read|write)_ops'; then
 echo 'Previous ABI or native provider exposed by production MINIX.so' >&2
 exit 1
fi
readelf -SW "$target" | grep -F '.infiltrator_boot'
