from ae_logger import logger
from client_manager import ClientManager
from server_manager import ServerManager

import matplotlib.pyplot as plt
import numpy as np

client_manager = ClientManager()

server_manager = ServerManager()

cores_range = [4, 8, 16]

x = np.arange(len(cores_range))

for workload in ["a", "b", "c", "d", "f"]:
    plt.figure(figsize=(10, 6))
    for system in ["ditto", "scalio"]:
        qps = []
        for n_cores in cores_range:
            logger.info(f"Testing workload = {workload}, system = {system}, n_cores = {n_cores}")
            server_manager.run(7, system, n_cores=n_cores)
            result = client_manager.run(workload, 7, system, True)
            qps.append(result.qps)
            server_manager.kill()

        bar_width = 0.2
        bar_x = x - 0.5 * bar_width if system == "scalio" else x + 0.5 * bar_width
        plt.bar(bar_x, qps, width=bar_width, label=f"{system} throughput")

    # Grid on primary y-axis
    plt.grid(axis='y', linestyle='--', alpha=0.7)

    plt.title("Throughput when varying the number of cores under YCSB A, B, C, D, and F.")
    plt.xlabel("#cores")
    plt.xticks(x, list(map(str, cores_range)))
    plt.tight_layout()
    plt.savefig(f"figs/6-5-cores-{workload}.pdf", bbox_inches='tight')
