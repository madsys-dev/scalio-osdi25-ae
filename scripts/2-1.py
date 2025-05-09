import os

from ae_logger import logger
from client_manager import ClientManager
from server_manager import ServerManager

import matplotlib.pyplot as plt

client_manager = ClientManager()

server_manager = ServerManager()

if os.path.exists("figs/2-1.pdf"):
    exit(0)

x = range(1, 8)

qps = []
cpu = []

for num_ssd in x:
    logger.info(f"num_ssd = {num_ssd}")
    server_manager.run(num_ssd, "leed")
    client_manager.run("c", num_ssd, "leed", True)
    result = client_manager.run("c", num_ssd, "leed", False, skewness="0", op_cnt="8000000" if num_ssd == 1 else "40000000", perf_pid=server_manager.server_process.pid + 1)
    qps.append(result.qps)
    cpu.append(result.perf * 7)  # Actually the total is 700%
    server_manager.kill()

fig, ax1 = plt.subplots(figsize=(10, 6))
ax2 = ax1.twinx()

ax1.bar(x, qps, width=0.2, label="Throughput")
ax2.plot(x, cpu, label="CPU usage for SSD I/O")

ax1.set_xticks(x)

bars_labels, bars_handles = ax1.get_legend_handles_labels()
lines_labels, lines_handles = ax2.get_legend_handles_labels()
ax1.legend(bars_labels + lines_labels, bars_handles + lines_handles, loc='upper left')

# Grid on primary y-axis
ax1.grid(axis='y', linestyle='--', alpha=0.7)

plt.title("LEED’s throughput and CPU usage for handling SSD I/O operations as the number of SSDs scale up")
plt.xlabel("#SSD")

plt.savefig("figs/2-1.pdf", bbox_inches='tight')
