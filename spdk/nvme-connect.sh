#!/bin/bash
set -e
nvme connect -t rdma -n nvme-4-6-0 -a 10.3.4.6 -s 4420
nvme connect -t rdma -n nvme-4-6-1 -a 10.3.4.6 -s 4421
nvme connect -t rdma -n nvme-4-6-2 -a 10.3.4.6 -s 4422
nvme connect -t rdma -n nvme-4-6-3 -a 10.3.4.6 -s 4423
nvme connect -t rdma -n nvme-4-6-4 -a 10.3.4.6 -s 4424
nvme connect -t rdma -n nvme-4-6-5 -a 10.3.4.6 -s 4425
nvme connect -t rdma -n nvme-4-6-6 -a 10.3.4.6 -s 4426
