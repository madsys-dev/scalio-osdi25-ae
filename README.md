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
- fio
- Ubuntu libraries: libaio-dev libcunit1-dev libmemcached-dev libgtest-dev libboost-all-dev uuid-dev libssl-dev libibverbs-dev librdmacm-dev
- Python packages: fabric matplotlib numpy psutil python-memcached
- Ensure that hugepages are enabled on all nodes (`echo 10240 > /proc/sys/vm/nr_hugepages`)
- The LEED baseline requires a `kv_etcd` to run. Refer to `src/kv_etcd` for the building Dockerfile.

**NVMe-oF target offload configuration:**

- For the server node, follow the instructions at https://enterprise-support.nvidia.com/s/article/howto-configure-nvme-over-fabrics--nvme-of--target-offload. Refer to `scripts/setup_nvme_of_target.sh` for an example.
- For the client node, execute `bash spdk/nvme-connect.sh` (this prerequisite is for evaluating Figure 6).
- SPDK offers built-in support for NVMe-oF hosts (a.k.a. clients), and the sample configuration file is located at `spdk/client.config.json`.

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

- Enter the directory `~/share/scalio`.
- Execute `bash scripts/evaluate.sh`.
- The following scripts will run sequentially. (Figures that have already been generated will be skipped.)

| Figure           | Script          | Output files       | Estimated time |
| ---------------- | --------------- | ------------------ | -------------- |
| Figure 2         | 6-2.py          | 6-2-**c**.pdf      | *              |
| Figure 5         | 2-1.py          | 2-1.pdf            | 40 minutes     |
| Figure 6         | 2-2.py          | 2-2.pdf            | 5 minutes      |
| Figures 11, 12** | 6-2.py          | 6-2-*.pdf          | 8 hours        |
| Figure 13        | 6-3.py          | 6-3.pdf            | 1 hour         |
| Figure 14        | 6-4.py          | 6-4-*.pdf          | 4 hours        |
| Figure 15        | 6-5-skewness.py | 6-5-skewness-*.pdf | 40 minutes     |
| Figure 16        | 6-5-dataset.py  | 6-5-dataset-*.pdf  | 4 hours        |
| Figure 17        | 6-5-cores.py    | 6-5-cores-*.pdf    | 1.5 hours      |
| Figure 18        | 6-5-slots.py    | 6-5-slots-*.pdf    | 40 minutes     |

*Figure 2 is the same as Figure 11 (c), so there is no need to run the script separately for Figure 2.

**Figures 11 and 12 are generated together with one script. For example, 6-2-a.pdf includes Figure 11 (a) and Figure 12 (a).
