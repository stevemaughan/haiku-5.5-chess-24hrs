#!/usr/bin/env bash
# Builds the release executable into final/. Run from anywhere: bash source/build_final.sh
set -e
cd "$(dirname "$0")/.."
g++ -std=c++17 -O3 -DNDEBUG -march=x86-64-v3 -mtune=generic -flto \
    -static -s -o final/Haiku55chess24hrs.exe source/haiku.cpp -lpthread
echo "built final/Haiku55chess24hrs.exe"
