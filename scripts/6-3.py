import os

from ae_logger import logger
from client_manager import ClientManager
from server_manager import ServerManager

import matplotlib.pyplot as plt
import numpy as np

client_manager = ClientManager()

server_manager = ServerManager()

values = [[] for _ in range(4)]

if os.path.exists("figs/6-3.pdf"):
    exit(0)

for workload in ["a", "b", "c", "d", "f"]:
    for stage in range(4):
        logger.info(f"Testing workload = {workload}, stage = {stage}")
        server_manager.run(7, "scalio")
        result = client_manager.run(workload, 7, "scalio", True, stage)
        values[stage].append(result.qps)
        server_manager.kill()


plt.figure(figsize=(10, 6))

x = np.arange(5)

bar_width = 0.2

plt.bar(x - 1.5 * bar_width, values[0], width=bar_width, label="baseline")
plt.bar(x - 0.5 * bar_width, values[1], width=bar_width, label="+ offloaded read")
plt.bar(x + 0.5 * bar_width, values[2], width=bar_width, label="+ inline cache")
plt.bar(x + 1.5 * bar_width, values[3], width=bar_width, label="+ batched write")

plt.title("Throughput improvement breakdown under YCSB workloads A, B, C, D, and F.")
plt.xlabel("Workloads")
plt.ylabel("Throughput")

plt.xticks(x, ["A", "B", "C", "D", "F"])
plt.yscale('log')

plt.legend()

plt.tight_layout()

plt.savefig("figs/6-3.pdf", bbox_inches='tight')
