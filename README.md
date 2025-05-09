# Scalio: Scaling up DPU-based JBOF Key-value Store with NVMe-oF Target Offload

This is the repository for the paper *Scalio: Scaling up DPU-based JBOF Key-value Store with NVMe-oF Target Offload*. This artifact provides the source code of Scalio and scripts to reproduce the evaluation results in our paper.

## Prerequisites

> ⚡ **Notice for artifact evaluation:**
> The environment has been fully prepared for artifact evaluation, aiming to streamline the evaluation process and reduce unnecessary effort in setting up the environment.

**Hardware:**

- A server node with a bunch of NVMe SSDs, connected to several client nodes with RDMA network.
- Mellanox ConnectX NIC that supports NVMe-oF target offload.

**Software:**

- etcd
- Memcached
- perf
- Ubuntu libraries: libaio-dev libcunit1-dev libmemcached-dev libgtest-dev libboost-all-dev uuid-dev libssl-dev libibverbs-dev librdmacm-dev
- Python packages: fabric matplotlib numpy python-memcached
- Ensure that hugepages are enabled on all nodes (`echo 10240 > /proc/sys/vm/nr_hugepages`)
- The LEED baseline requires a `kv_etcd` to run. Refer to `src/kv_etcd` for the building Dockerfile.

**NVMe-oF target offload configuration:**

- For the server node, follow the instructions at https://enterprise-support.nvidia.com/s/article/howto-configure-nvme-over-fabrics--nvme-of--target-offload
- For the client node, SPDK offers built-in support for NVMe-oF hosts (a.k.a. clients), and the sample configuration file is located at `spdk/client.config.json`

## Build Source Code *(Artifacts Available)*

- On the server node:

```bash
# Clone source code
git clone https://github.com/madsys-dev/scalio-osdi25-ae.git ~/share/scalio
# Build source code
cd ~/share/scalio/
bash scripts/build.sh
```

> ⚠️ **Caveat for artifact evaluation:**  
> Ensure that the source code is placed at `~/share/scalio`, as the directory is configured to be shared among the cluster with NFS.

## Kick the Tires *(Artifacts Functional)*

- Enter the directory `~/share/scalio`.
- Execute `python3 scripts/kick-the-tires.py`.
- The script should finish successfully in 8 minutes.

## Reproduce Evaluation Results *(Results Reproduced)*

> Scripts are located in `scripts/` and the output files are located in `figs/`.

Although we have made every effort to ensure the robustness of the reproduction scripts, occasional failures may still occur.

If you encounter stuck execution or any other issues, try on the server node with:

- `python3 scripts/stop-the-world.py`
- Use Ctrl + C to stop the failing script.
- `python3 scripts/kick-the-tires.py`

And retry with the failed script.

For scripts that generate several sub-figures, you may manually modify the `for workload in [...]` loop to proceed at where the script failed.

If the problem persists, please contact us to assist with resetting the environment.

| Figure    | Script          | Output files       | Estimated time           |
| --------- | --------------- | ------------------ | ------------------------ |
| Figure 2  | 6-2.py          | 6-2-**c**.pdf      | Same script as Figure 11 |
| Figure 5  | 2-1.py          | 2-1.pdf            | 40 minutes               |
| Figure 11 | 6-2.py          | 6-2-*.pdf          | 8 hours                  |
| Figure 13 | 6-3.py          | 6-3.pdf            | 1 hour                   |
| Figure 14 | 6-4.py          | 6-4-*.pdf          | 4 hours                  |
| Figure 15 | 6-5-skewness.py | 6-5-skewness-*.pdf | 40 minutes               |
| Figure 16 | 6-5-dataset.py  | 6-5-dataset-*.pdf  | 4 hours                  |
| Figure 17 | 6-5-cores.py    | 6-5-cores-*.pdf    | 1.5 hours                |
| Figure 18 | 6-5-slots.py    | 6-5-slots-*.pdf    | 40 minutes               |
