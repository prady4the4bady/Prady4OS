#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/../.."
for i in 1 2 3; do
    make smoke-shell >/dev/null 2>&1
    rc=$?
    echo "run$i rc=$rc lines=$(wc -l < build/shell_serial.log)" \
         "BIGHEAD=$(grep -ac BIGHEAD build/shell_serial.log)" \
         "BIGTAIL=$(grep -ac BIGTAIL build/shell_serial.log)" \
         "EAP55a=$(grep -ac EAP55a build/shell_serial.log)" \
         "drops=$(grep -ao 'thre_drops=[0-9]*' build/shell_serial.log | tail -1)"
done
