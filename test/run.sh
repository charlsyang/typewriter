#!/bin/sh
# Builds lib/Typewriter/src/Sync.cpp against in-memory fakes and runs the tests.
set -e
cd "$(dirname "$0")"
mkdir -p ../build/test
cp ../lib/Typewriter/src/Sync.cpp ../lib/Typewriter/src/Sync.h fakes/*.h ../build/test/
c++ -std=c++17 -Wall -I../build/test -o ../build/test/sync_test sync_test.cpp ../build/test/Sync.cpp
../build/test/sync_test
