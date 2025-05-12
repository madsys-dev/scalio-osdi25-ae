#!/bin/bash

modprobe nvmet
modprobe nvmet-rdma
modprobe nvme-rdma

for i in $(seq 0 6);
do
    mkdir /sys/kernel/config/nvmet/subsystems/nvme-4-6-$i
    cd /sys/kernel/config/nvmet/subsystems/nvme-4-6-$i
    echo 1 > attr_allow_any_host
    echo 1 > attr_offload

    mkdir namespaces/1$i
    cd namespaces/1$i
    echo -n /dev/nvme${i}n1 > device_path
    echo 1 > enable
    cd ..

    mkdir /sys/kernel/config/nvmet/ports/1$i
    cd /sys/kernel/config/nvmet/ports/1$i
    echo 10.3.4.6 > addr_traddr
    echo rdma > addr_trtype
    echo 442$i > addr_trsvcid
    echo ipv4 > addr_adrfam
    ln -s /sys/kernel/config/nvmet/subsystems/nvme-4-6-$i /sys/kernel/config/nvmet/ports/1$i/subsystems/nvme-4-6-$i
done

for i in $(seq 0 6);
do
    mkdir /sys/kernel/config/nvmet/subsystems/nvme-4-6-not-offload-$i
    cd /sys/kernel/config/nvmet/subsystems/nvme-4-6-not-offload-$i
    echo 1 > attr_allow_any_host

    mkdir namespaces/2$i
    cd namespaces/2$i
    echo -n /dev/nvme${i}n1 > device_path
    echo 1 > enable
    cd ..

    mkdir /sys/kernel/config/nvmet/ports/2$i
    cd /sys/kernel/config/nvmet/ports/2$i
    echo 10.3.4.6 > addr_traddr
    echo rdma > addr_trtype
    echo 443$i > addr_trsvcid
    echo ipv4 > addr_adrfam
    ln -s /sys/kernel/config/nvmet/subsystems/nvme-4-6-not-offload-$i /sys/kernel/config/nvmet/ports/2$i/subsystems/nvme-4-6-not-offload-$i
done
