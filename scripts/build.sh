#!/bin/bash

if [[ "$(basename "$PWD")" == "scripts" ]]; then
    cd ..
fi

if [[ "$(basename "$PWD")" != "spdk" ]]; then
    cd spdk
fi

./configure --with-rdma
make -j
make SKIP_DPDK_BUILD=1

cd app/leed/ditto
mkdir build
cd build
cmake ..
make -j
