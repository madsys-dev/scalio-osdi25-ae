import os

from ae_logger import logger
from client_manager import ClientManager
from server_manager import ServerManager

import matplotlib.pyplot as plt
import numpy as np

client_manager = ClientManager()

server_manager = ServerManager()

for workload in ["a", "b", "c", "d", "f"]:
    if os.path.exists(f"figs/6-4-{workload}.pdf"):
        continue
    plt.figure(figsize=(8, 5))
    for (system, batch, color, label) in [("scalio", True, "blue", "Scalio w/ batched write"), ("scalio", False, "red", "Scalio w/o batched write"), ("ditto", False, "brown", "LEED+Ditto")]:

        x = []
        y = []
        for io in [32, 64, 128, 256, 512]:
            logger.info(f"Testing workload = {workload}, system = {system}, batch = {batch}, io = {io}")
            server_manager.run(7, system, set_batch=min(io // 16, 8) if batch else 1)
            result = client_manager.run(workload, 7, system, True, stage=3 if batch else 2, io=io)
            x.append(result.qps)
            y.append(result.avg_lat)
            server_manager.kill()
        plt.plot(x, y, color=color, label=label, marker='o')
    plt.xlabel('Throughput')
    plt.ylabel('Avg latency')
    plt.title('Latency to throughput by varying the number of concurrent I/O queues.')
    plt.legend()
    plt.savefig(f"figs/6-4-{workload}.pdf", bbox_inches='tight')
