#!/bin/env bash

sudo apt install libebook1.2-dev libedata-book1.2-dev evolution-data-server-dev
sudo apt-get build-dep pidgin

export AFL_USE_ASAN=1
#export AFL_USE_MSAN=1
#export AFL_USE_UBSAN=1
#export AFL_USE_CFISAN=1

CC="afl-clang-fast"
CFLAGS="-fsanitize=address -g -O0"

cd pidgin-2.14.5
make clean

./configure CFLAGS="${CFLAGS}" \
            CXXFLAGS="${CFLAGS}" \
            LDFLAGS="-fsanitize=address" \
            CC="${CC}" \
	    --disable-screensaver

make -j9 STATIC=1
${CC} ../fuzz_dbus.c -o ../fuzz.out -fsanitize=address -g -O0  $(pkg-config --libs --cflags dbus-1)
cd -
