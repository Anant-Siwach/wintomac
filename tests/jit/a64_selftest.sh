#!/bin/sh
# Assemble every case of a64_selftest.c with clang and compare against our encoder (runtime/jit_a64.h).
set -e
D=${1:-build/jit_a64}
mkdir -p $D
clang -O1 -w -o $D/selftest tests/jit/a64_selftest.c
$D/selftest > $D/mine.txt
{ echo ".text"; sed 's/^[0-9a-f]* //' $D/mine.txt; } > $D/cases.s
clang -c -arch arm64 -mcpu=apple-m1 -o $D/cases.o $D/cases.s
otool -tX $D/cases.o | awk '{for (i = 2; i <= NF; i++) print $i}' > $D/ref.txt
cut -d' ' -f1 $D/mine.txt > $D/mine_words.txt
if paste -d' ' $D/ref.txt $D/mine.txt | awk '$1 != $2 { print "MISMATCH clang=" $1 " ours=" $2 " :", substr($0, index($0, $3)); bad = 1 } END { exit bad }'; then
  echo "a64 selftest: $(wc -l < $D/mine.txt | tr -d ' ') encodings match clang"
else exit 1; fi
