#!/bin/sh
# Builds the host-testable parts of lib/Typewriter against in-memory fakes and runs the tests.
set -e
cd "$(dirname "$0")"
mkdir -p ../build/test
cp ../lib/Typewriter/src/Sync.cpp ../lib/Typewriter/src/Sync.h ../lib/Typewriter/src/History.cpp \
   ../lib/Typewriter/src/History.h ../lib/Typewriter/src/Highlight.cpp \
   ../lib/Typewriter/src/Highlight.h ../lib/Typewriter/src/HighlightStore.cpp \
   ../lib/Typewriter/src/HighlightStore.h fakes/*.h ../build/test/
c++ -std=c++17 -Wall -I../build/test -o ../build/test/sync_test sync_test.cpp ../build/test/Sync.cpp
c++ -std=c++17 -Wall -I../build/test -o ../build/test/history_test history_test.cpp ../build/test/History.cpp
c++ -std=c++17 -Wall -I../build/test -o ../build/test/highlight_test highlight_test.cpp ../build/test/Highlight.cpp
c++ -std=c++17 -Wall -I../build/test -o ../build/test/highlight_store_test highlight_store_test.cpp ../build/test/HighlightStore.cpp
echo "== sync"; ../build/test/sync_test | tail -1
echo "== history"; ../build/test/history_test
echo "== highlight"; ../build/test/highlight_test
echo "== highlight store"; ../build/test/highlight_store_test
