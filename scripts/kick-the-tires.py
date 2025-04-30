from ae_logger import logger
from client_manager import ClientManager
from server_manager import ServerManager

import matplotlib.pyplot as plt

client_manager = ClientManager()

server_manager = ServerManager()

num_ssd = 7

plt.figure(figsize=(8, 5))

for system in ["leed", "scalio"]:
    server_manager.run(num_ssd, system)
    logger.info(f"Testing workload = c, system = {system}")
    result = client_manager.run("c", num_ssd, system, True)
    logger.info(result)
    server_manager.kill()
