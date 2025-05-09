import os
import sys
from subprocess import Popen, DEVNULL, run
from time import sleep


REQUIRED_DIR = "/home/ubuntu/share/scalio"
current_dir = os.getcwd()

if current_dir != REQUIRED_DIR:
    print(f"Error: Please run this script from {REQUIRED_DIR}.")
    print(f"Current directory: {current_dir}")
    sys.exit(1)


class ServerManager:
    def __init__(self):
        self.server_process: Popen | None = None
        self.ditto_process: Popen | None = None

    def run(self, num_ssd: int, system: str, n_cores: int = 8, n_slots: int = 8, set_batch: int = 8):
        os.system("pkill init")
        Popen(["sudo rm /dev/shm/kv_app_trace.*"], shell=True, stdin=DEVNULL, stdout=DEVNULL, stderr=DEVNULL)
        sleep(3)
        if system == "scalio":
            system_flag = ["-C", "ours"]
        elif system == "ditto":
            system_flag = ["-C", "ditto"]
            self.ditto_process = Popen(["../../build/experiments/init", "-S", "-c", "../configs/server_conf_sample.json", "-m", "10.1.4.6"], stdin=DEVNULL, stdout=DEVNULL, stderr=DEVNULL, cwd="spdk/app/leed/ditto/experiments/ycsb_test")
            sleep(1)
        else:
            system_flag = []

        Popen(["sudo", "pkill", "-f", "app/leed/app/ring_server/kv_ring_server"], stdin=DEVNULL, stdout=DEVNULL, stderr=DEVNULL)
        os.system(f"sed -i \"s/^#define MAX_SSD_WORKERS.*/#define MAX_SSD_WORKERS {n_cores // 2}/\" spdk/app/leed/app/ring_server/kv_server.c")
        os.system("touch spdk/app/leed/ditto/src/dmc_table.h && touch spdk/app/leed/ditto/src/client.h")
        run(["make", "SKIP_DPDK_BUILD=1", f"HASH_BUCKET_ASSOC_NUM={n_slots}"], stdin=DEVNULL, stdout=DEVNULL, stderr=DEVNULL, check=True, cwd="spdk")
        sleep(5)
        self.server_process = Popen(["sudo", "app/leed/app/ring_server/kv_ring_server", "-b", f"{set_batch}", "-d", f"{num_ssd}"] + system_flag, stdin=DEVNULL, stdout=DEVNULL, stderr=DEVNULL, cwd="spdk", start_new_session=True)
        sleep(20)

    def kill(self):
        Popen(["sudo", "pkill", "-f", "app/leed/app/ring_server/kv_ring_server"], stdin=DEVNULL, stdout=DEVNULL, stderr=DEVNULL)
        self.server_process.wait()
        if self.ditto_process:
            self.ditto_process.terminate()
            self.ditto_process.wait()
            self.ditto_process = None
        sleep(30)
