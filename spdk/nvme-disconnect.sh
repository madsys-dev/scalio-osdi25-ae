#!/bin/bash
set -e
nvme disconnect -n nvme-4-6-0
nvme disconnect -n nvme-4-6-1
nvme disconnect -n nvme-4-6-2
nvme disconnect -n nvme-4-6-3
nvme disconnect -n nvme-4-6-4
nvme disconnect -n nvme-4-6-5
nvme disconnect -n nvme-4-6-6

nvme disconnect -n nvme-4-6-not-offload-0
nvme disconnect -n nvme-4-6-not-offload-1
nvme disconnect -n nvme-4-6-not-offload-2
nvme disconnect -n nvme-4-6-not-offload-3
nvme disconnect -n nvme-4-6-not-offload-4
nvme disconnect -n nvme-4-6-not-offload-5
nvme disconnect -n nvme-4-6-not-offload-6
