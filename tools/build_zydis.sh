#!/bin/sh
# Fetch and build Zydis (x86/x86-64 decoder, MIT, zyantific) + its Zycore dependency (MIT) into
# third_party/zydis/libzydis.a. Used by the runtime interpreter (runtime/interp*.c) for decoding.
# Only the decoder/formatter is built (no encoder); sources are pinned by SHA-256.
set -e
ZVER=4.1.1
ZSHA=45c6d4d499a1cc80780f7834747c637509777c01dca1e98c5e7c0bfaccdb1514
CCOMMIT=0b2432ced0884fd152b471d97ecf0258ff4d859f      # the zycore-c commit zydis v4.1.1 pins as a submodule
CSHA=698e4623e661db312eb9da327519f53f0a9dd7fbbfacf02159687bca62369467
D=third_party/zydis
mkdir -p $D && cd $D
[ -f zydis-$ZVER.tar.gz ] || curl -sSfL -o zydis-$ZVER.tar.gz https://github.com/zyantific/zydis/archive/refs/tags/v$ZVER.tar.gz
[ -f zycore-${CCOMMIT%${CCOMMIT#????????}}.tar.gz ] || curl -sSfL -o zycore-${CCOMMIT%${CCOMMIT#????????}}.tar.gz https://github.com/zyantific/zycore-c/archive/$CCOMMIT.tar.gz
echo "$ZSHA  zydis-$ZVER.tar.gz" | shasum -a 256 -c - >/dev/null
echo "$CSHA  zycore-${CCOMMIT%${CCOMMIT#????????}}.tar.gz" | shasum -a 256 -c - >/dev/null
[ -d zydis-$ZVER ] || tar xzf zydis-$ZVER.tar.gz
[ -d zycore-c-$CCOMMIT ] || tar xzf zycore-${CCOMMIT%${CCOMMIT#????????}}.tar.gz
rm -f include && ln -s zydis-$ZVER/include include
rm -f zycore-include && ln -s zycore-c-$CCOMMIT/include zycore-include
mkdir -p obj
FLAGS="-O2 -mcpu=apple-m1 -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -DZYDIS_DISABLE_ENCODER -Izydis-$ZVER/include -Izydis-$ZVER/src -Izycore-c-$CCOMMIT/include -w"
OBJS=""
for f in Decoder DecoderData Disassembler Formatter FormatterATT FormatterBase FormatterBuffer FormatterIntel \
         MetaInfo Mnemonic Register Segment SharedData String Utils Zydis; do
    clang $FLAGS -c zydis-$ZVER/src/$f.c -o obj/zydis_$f.o; OBJS="$OBJS obj/zydis_$f.o"
done
for f in Allocator Format String Vector Zycore; do
    clang $FLAGS -c zycore-c-$CCOMMIT/src/$f.c -o obj/zycore_$f.o; OBJS="$OBJS obj/zycore_$f.o"
done
rm -f libzydis.a && ar rcs libzydis.a $OBJS
echo "[zydis] built $D/libzydis.a"
