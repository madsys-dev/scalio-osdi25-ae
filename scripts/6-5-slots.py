import os

from ae_logger import logger
from client_manager import ClientManager
from server_manager import ServerManager

import matplotlib.pyplot as plt
import numpy as np

client_manager = ClientManager()

server_manager = ServerManager()

slots_range = [4, 8, 16]

x = np.arange(len(slots_range))

for workload in ["a", "b", "c", "d", "f"]:
    if os.path.exists(f"figs/6-5-slots-{workload}.pdf"):
        continue
    fig, ax1 = plt.subplots(figsize=(10, 6))
    qps = []
    hr = []
    for n_slots in slots_range:
        server_manager.run(7, "scalio", n_slots=n_slots)
        logger.info(f"Testing workload = {workload}, n_slots = {n_slots}")
        result = client_manager.run(workload, 7, "scalio", True)
        qps.append(result.qps)
        hr.append(result.hr)
        server_manager.kill()

    bar_width = 0.2
    ax1.bar(x, qps, width=bar_width, label="Throughput")

    ax2 = ax1.twinx()
    ax2.plot(x, hr, label="Hit ratio")

    ax1.set_xticks(x)
    ax1.set_xticklabels(map(str, slots_range))

    ax2.set_ylim(0, 1)

    bars_labels, bars_handles = ax1.get_legend_handles_labels()
    lines_labels, lines_handles = ax2.get_legend_handles_labels()
    ax1.legend(bars_labels + lines_labels, bars_handles + lines_handles, loc='upper left')

    # Grid on primary y-axis
    ax1.grid(axis='y', linestyle='--', alpha=0.7)

    plt.title("Throughput and cache hit ratio when varying the number of slots per hash block under YCSB A, B, C, D, and F.")
    plt.xlabel("#slot")
    plt.xticks(x, list(map(str, slots_range)))
    plt.tight_layout()
    plt.savefig(f"figs/6-5-slots-{workload}.pdf", bbox_inches='tight')
