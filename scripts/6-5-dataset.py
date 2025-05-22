import os

from ae_logger import logger
from client_manager import ClientManager
from server_manager import ServerManager

import matplotlib.pyplot as plt
import numpy as np

client_manager = ClientManager()

server_manager = ServerManager()

size_range = ["20000000", "40000000", "60000000", "80000000"]

x = np.arange(len(size_range))

for workload in ["a", "b", "c", "d", "f"]:
    if os.path.exists(f"figs/6-5-dataset-{workload}.pdf"):
        continue
    fig, ax1 = plt.subplots(figsize=(10, 6))
    ax2 = ax1.twinx()
    for system in ["scalio", "ditto"]:
        qps = []
        hr = []
        for size in size_range:
            server_manager.run(7, system)
            logger.info(f"Testing workload = {workload}, system = {system}, size = {size}")
            result = client_manager.run(workload, 7, system, True, size=size)
            qps.append(result.qps)
            hr.append(result.hr)
            server_manager.kill()

        bar_width = 0.2
        bar_x = x - 0.5 * bar_width if system == "scalio" else x + 0.5 * bar_width
        color = 'blue' if system == "scalio" else 'red'
        ax1.bar(bar_x, qps, width=bar_width, color=color, label=f"{system} throughput")
        ax2.plot(x, hr, color=color, label=f"{system} hit ratio", marker='o', markersize=6, markeredgecolor='black')

    ax1.set_xticks(x)
    ax1.set_xticklabels(size_range)

    bars_labels, bars_handles = ax1.get_legend_handles_labels()
    lines_labels, lines_handles = ax2.get_legend_handles_labels()
    ax1.legend(bars_labels + lines_labels, bars_handles + lines_handles, loc='upper left', ncol=4)

    ax1.set_ylabel("Throughput")
    ax2.set_ylabel("Hit Ratio")

    # Grid on primary y-axis
    ax1.grid(axis='y', linestyle='--', alpha=0.7)

    plt.title("Throughput and cache hit ratio when varying the total number of records under YCSB A, B, C, and F.")
    plt.xlabel("#record")
    plt.xticks(x, size_range)
    plt.tight_layout()
    plt.savefig(f"figs/6-5-dataset-{workload}.pdf", bbox_inches='tight')
