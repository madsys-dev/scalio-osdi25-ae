import invoke
import os
import re
from dataclasses import dataclass
from subprocess import Popen, DEVNULL
from time import sleep

from fabric import Connection

client_ips = ["10.1.4.2", "10.1.4.3", "10.1.6.4", "10.1.6.6"]

thread_per_client = 24


workload_to_read_ratio = {
    "a": 0.5,
    "b": 0.95,
    "c": 1.0,
    "d": 0.95,
    "f": 2 / 3,
}

workload_to_op_cnt = {
    "a": 8000000,
    "b": 8000000,
    "c": 8000000,
    "d": 8000000,
    "f": 12000000,
}


@dataclass
class Result:
    avg_lat: float
    qps: float
    hr: float
    ssd_iops: float

class ClientManager:
    def __init__(self):
        self.clients = [Connection(ip, user='ubuntu') for ip in client_ips]

    def run(self, workload: str, num_ssd: int, system: str, fill: bool, stage: int = 3, io: int = 512, skewness: str = "0.99", size: str = "20000000") -> Result:
        if system == "scalio":
            system_flag = "-C ours"
        elif system == "ditto":
            system_flag = "-C ditto"
        else:
            system_flag = ""

        if fill:
            fill_flag = "-F"
        else:
            fill_flag = ""

        if system_flag == "":
            num_clients = 1
        elif workload == "c":
            num_clients = 4
        elif workload == "b" or workload == "d":
            num_clients = 2
        else:
            num_clients = 1

        os.system(f"sed -i \"s/^zipfianconstant=.*/zipfianconstant={skewness}/\" spdk/app/leed/ycsb/workloads/workload{workload}.spec")
        os.system(f"sed -i \"s/^recordcount=.*/recordcount={size}/\" spdk/app/leed/ycsb/workloads/workload{workload}.spec")

        if fill and num_clients > 1:
            os.system("pkill -f controller.py")
            sleep(1)
            Popen(f"python3 ../controller.py twitter042-10m -s 1 -c {thread_per_client} -o result -m 10.1.4.6", shell=True, stdin=DEVNULL, stdout=DEVNULL, stderr=DEVNULL, cwd="spdk/app/leed/ditto/experiments/ycsb_test")
            sleep(3)
            self.clients[0].run(f"cd share/scalio/spdk && sudo app/leed/app/ring_ycsb_client/kv_ring_ycsb_client -w app/leed/ycsb/workloads/workload{workload}.spec -I 1000 -i {io} -x 1 {system_flag} -P {thread_per_client} -d {num_ssd} -B 3 -F", asynchronous=True).join()
            fill_flag = ""

        p = min(thread_per_client, io // num_clients)
        os.system("pkill -f controller.py")
        sleep(1)
        Popen(f"python3 ../controller.py twitter042-10m -s 1 -c {p * num_clients} -o result -m 10.1.4.6", shell=True, stdin=DEVNULL, stdout=DEVNULL, stderr=DEVNULL, cwd="spdk/app/leed/ditto/experiments/ycsb_test")
        sleep(3)

        client_processes = [client.run(f"cd share/scalio/spdk && sleep {index * 5} && sudo app/leed/app/ring_ycsb_client/kv_ring_ycsb_client -w app/leed/ycsb/workloads/workload{workload}.spec -I 1000 -i {io // num_clients} -x 1 {system_flag} -P {p} -x {1 + p * index} -d {num_ssd} -B {stage} {fill_flag}", asynchronous=True) for (index, client) in enumerate(self.clients[:num_clients])]
        results = []
        op_cnt = workload_to_op_cnt[workload]
        for client_process in client_processes:
            result = client_process.join()
            output = result.stdout.split('\n')[-4 if system == "scalio" or system == "ditto" else -3:]
            avg_lat = float(re.search(r"average latency: (.*) us", output[0]).group(1))
            qps = float(re.search(r"TRANSACTION rate: (.*)", output[1]).group(1))
            n_get = op_cnt * workload_to_read_ratio[workload]
            if system == "scalio":
                match = re.search(r"n_set = (.*), n_set_dummy = (.*), n_get = (.*),", output[2])
                n_set = float(match.group(1))
                n_set_dummy = float(match.group(2))
                hr = workload_to_read_ratio[workload] - (n_set + n_set_dummy) / op_cnt
                ssd_iops = (2 * (n_set + n_set_dummy) + 2 * (op_cnt - n_get)) / op_cnt * qps
            elif system == "ditto":
                match = re.search(r"n_set = (.*), n_get = (.*)", output[2])
                n_set = float(match.group(1)) - (0 if fill_flag == "" else float(size))
                hr = 1 - n_set / op_cnt
                ssd_iops = (2 * n_set + 1 * (op_cnt - n_get)) / op_cnt * qps
            else:
                hr = 0
                ssd_iops = (2 * n_get + 3 * (op_cnt - n_get)) / op_cnt * qps
            results.append(Result(avg_lat=avg_lat, qps=qps, hr=hr, ssd_iops=ssd_iops))
        sleep(20)
        return Result(
            avg_lat=sum(result.avg_lat for result in results) / len(results),
            qps=sum(result.qps for result in results),
            hr=sum(result.hr for result in results) / len(results),
            ssd_iops=sum(result.ssd_iops for result in results),
        )

    def stop_all(self):
        Popen(["sudo", "pkill", "-f", "app/leed/app/ring_server/kv_ring_server"], stdin=DEVNULL, stdout=DEVNULL, stderr=DEVNULL)
        for client in self.clients:
            try:
                client.run(f"sudo pkill -9 -f app/leed/app/ring_ycsb_client/kv_ring_ycsb_client", asynchronous=True).join()
            except invoke.exceptions.UnexpectedExit:
                pass
            try:
                client.run(f"sudo rm /dev/shm/kv_app_trace.*", asynchronous=True).join()
            except invoke.exceptions.UnexpectedExit:
                pass
        os.system("pkill -f controller.py && pkill init")
