import logging

logging.basicConfig(
    format='%(asctime)s - %(message)s',
    datefmt='%H:%M:%S',
)

logger = logging.getLogger("ae")
logger.setLevel(logging.INFO)
