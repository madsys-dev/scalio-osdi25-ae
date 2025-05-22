import os

from ae_logger import logger
from client_manager import ClientManager

import matplotlib.pyplot as plt
import numpy as np

client_manager = ClientManager()

if os.path.exists("figs/2-2.pdf"):
    exit(0)

x = range(1, 8)

standard = []
offload = []
kworker_cpu = []

for num_ssd in x:
    logger.info(f"num_ssd = {num_ssd}")
    result = client_manager.fio(num_ssd)
    standard.append(result.standard)
    offload.append(result.offload)
    kworker_cpu.append(result.kworker_cpu / 100.0)

fig, ax1 = plt.subplots(figsize=(10, 6))
ax2 = ax1.twinx()

bar_width = 0.2
npx = np.arange(1, 8)
ax1.bar(npx - 0.5 * bar_width, standard, width=bar_width, label="Standard")
ax1.bar(npx + 0.5 * bar_width, offload, width=bar_width, label="Offload")
ax2.plot(x, kworker_cpu, color='red', label="CPU usage", marker='o')

ax1.set_xticks(x)

bars_labels, bars_handles = ax1.get_legend_handles_labels()
lines_labels, lines_handles = ax2.get_legend_handles_labels()
ax1.legend(bars_labels + lines_labels, bars_handles + lines_handles, loc='upper left')

# Grid on primary y-axis
ax1.grid(axis='y', linestyle='--', alpha=0.7)

plt.title("FIO performance and CPU usage")
plt.xlabel("#SSD")

plt.savefig("figs/2-2.pdf", bbox_inches='tight')
