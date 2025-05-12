#!/bin/bash
set -e
nvme connect -t rdma -n nvme-4-6-0 -a 10.3.4.6 -s 4420
nvme connect -t rdma -n nvme-4-6-1 -a 10.3.4.6 -s 4421
nvme connect -t rdma -n nvme-4-6-2 -a 10.3.4.6 -s 4422
nvme connect -t rdma -n nvme-4-6-3 -a 10.3.4.6 -s 4423
nvme connect -t rdma -n nvme-4-6-4 -a 10.3.4.6 -s 4424
nvme connect -t rdma -n nvme-4-6-5 -a 10.3.4.6 -s 4425
nvme connect -t rdma -n nvme-4-6-6 -a 10.3.4.6 -s 4426

nvme connect -t rdma -n nvme-4-6-not-offload-0 -a 10.3.4.6 -s 4430
nvme connect -t rdma -n nvme-4-6-not-offload-1 -a 10.3.4.6 -s 4431
nvme connect -t rdma -n nvme-4-6-not-offload-2 -a 10.3.4.6 -s 4432
nvme connect -t rdma -n nvme-4-6-not-offload-3 -a 10.3.4.6 -s 4433
nvme connect -t rdma -n nvme-4-6-not-offload-4 -a 10.3.4.6 -s 4434
nvme connect -t rdma -n nvme-4-6-not-offload-5 -a 10.3.4.6 -s 4435
nvme connect -t rdma -n nvme-4-6-not-offload-6 -a 10.3.4.6 -s 4436
