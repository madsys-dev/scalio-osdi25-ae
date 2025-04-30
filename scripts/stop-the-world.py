from ae_logger import logger
from client_manager import ClientManager

client_manager = ClientManager()

logger.info("Stop the world in progress.")
client_manager.stop_all()
logger.info("Stop the world Done.")
