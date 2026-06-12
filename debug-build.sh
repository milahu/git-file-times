#!/bin/sh

rm -rf build/; cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-debug -j1
