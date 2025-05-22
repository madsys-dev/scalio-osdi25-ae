import os

from ae_logger import logger
from client_manager import ClientManager
from server_manager import ServerManager

import matplotlib.pyplot as plt

client_manager = ClientManager()

server_manager = ServerManager()

x = range(1, 8)

system_names = ["scalio", "ditto", "leed"]

for workload in ["a", "b", "c", "d", "f"]:
    if os.path.exists(f"figs/6-2-{workload}.pdf"):
        continue
    fig, (ax1, ax2) = plt.subplots(2, figsize=(10, 12))

    qps = {system: [] for system in system_names}
    ssd_iops = {system: [] for system in system_names}
    for num_ssd in x:
        for system in system_names:
            logger.info(f"Testing workload = {workload}, num_ssd = {num_ssd}, system = {system}")
            server_manager.run(num_ssd, system)
            result = client_manager.run(workload, num_ssd, system, True)
            qps[system].append(result.qps)
            ssd_iops[system].append(result.ssd_iops)
            server_manager.kill()

    for system in system_names:
        if system == "leed":
            color = "brown"
        elif system == "ditto":
            color = "red"
        else:
            color = "blue"
        ax1.plot(x, qps[system], color=color, label=system, marker='o')
        ax2.plot(x, ssd_iops[system], color=color, label=system, marker='o')

    ax1.grid(True)
    ax2.grid(True)

    ax1.legend(ncol=3)
    ax2.legend(ncol=3)

    plt.xlabel("#SSD")

    plt.savefig(f"figs/6-2-{workload}.pdf", bbox_inches='tight')
