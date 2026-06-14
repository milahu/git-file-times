#!/bin/sh

rm -rf debug-build; cmake -S . -B debug-build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build debug-build -j1
