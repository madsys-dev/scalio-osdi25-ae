#!/bin/bash

if [[ "$(basename "$PWD")" == "scripts" ]]; then
    cd ..
fi

if [[ "$(basename "$PWD")" != "spdk" ]]; then
    cd spdk
fi

./configure --with-rdma
make

cd app/leed/ditto
mkdir build
cd build
cmake ..
make -j
