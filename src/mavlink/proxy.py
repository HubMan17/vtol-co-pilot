import socket
import threading
import time
import logging
from typing import Callable, Optional, List
from pymavlink import mavutil

from src.mavlink.telemetry import TelemetryParser, TelemetryState
from src.core.events import EventBus, Event

logger = logging.getLogger(__name__)


class MAVLinkProxy:
    def __init__(self, sitl_host: str = "127.0.0.1", sitl_port: int = 5762,
                 proxy_port: int = 14550, system_id: int = 255, component_id: int = 0):
        self.sitl_host = sitl_host
        self.sitl_port = sitl_port
        self.proxy_port = proxy_port
        self.system_id = system_id
        self.component_id = component_id

        self._sitl_conn: Optional[mavutil.mavlink_connection] = None
        self._proxy_socket: Optional[socket.socket] = None
        self._clients: List[socket.socket] = []

        self._running = False
        self._sitl_thread: Optional[threading.Thread] = None
        self._proxy_thread: Optional[threading.Thread] = None

        self._telemetry = TelemetryParser()
        self._event_bus = EventBus()
        self._message_callbacks: List[Callable] = []

        self._connected = False
        self._lock = threading.Lock()

    def connect(self) -> bool:
        try:
            conn_str = f"tcp:{self.sitl_host}:{self.sitl_port}"
            self._sitl_conn = mavutil.mavlink_connection(
                conn_str,
                source_system=self.system_id,
                source_component=self.component_id
            )

            self._sitl_conn.wait_heartbeat(timeout=10)
            self._connected = True
            self._event_bus.emit(Event.CONNECTION_RESTORED)
            return True
        except Exception as e:
            print(f"Connection failed: {e}")
            self._connected = False
            return False

    def start(self):
        if self._running:
            return

        self._running = True

        self._sitl_thread = threading.Thread(target=self._sitl_loop, daemon=True)
        self._sitl_thread.start()

        self._proxy_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._proxy_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._proxy_socket.bind(("0.0.0.0", self.proxy_port))
        self._proxy_socket.listen(5)

        self._proxy_thread = threading.Thread(target=self._proxy_accept_loop, daemon=True)
        self._proxy_thread.start()

    def stop(self):
        self._running = False

        if self._sitl_conn:
            self._sitl_conn.close()
            self._sitl_conn = None

        if self._proxy_socket:
            self._proxy_socket.close()
            self._proxy_socket = None

        for client in self._clients:
            try:
                client.close()
            except:
                pass
        self._clients.clear()

        self._connected = False

    def _sitl_loop(self):
        while self._running and self._sitl_conn:
            try:
                msg = self._sitl_conn.recv_match(blocking=True, timeout=0.1)
                if msg is None:
                    continue

                if msg.get_type() == "BAD_DATA":
                    continue

                self._telemetry.parse_message(msg)
                self._event_bus.emit(Event.TELEMETRY_UPDATE, self._telemetry.get_state())

                for callback in self._message_callbacks:
                    callback(msg)

                raw_bytes = msg.get_msgbuf()
                self._broadcast_to_clients(raw_bytes)

            except Exception as e:
                if self._running:
                    print(f"SITL loop error: {e}")
                    self._event_bus.emit(Event.CONNECTION_LOST)
                    time.sleep(1)

    def _proxy_accept_loop(self):
        while self._running and self._proxy_socket:
            try:
                self._proxy_socket.settimeout(1.0)
                client, addr = self._proxy_socket.accept()
                print(f"Client connected: {addr}")
                with self._lock:
                    self._clients.append(client)
                threading.Thread(target=self._client_handler, args=(client,), daemon=True).start()
            except socket.timeout:
                continue
            except Exception as e:
                if self._running:
                    print(f"Accept error: {e}")

    def _client_handler(self, client: socket.socket):
        try:
            while self._running:
                data = client.recv(1024)
                if not data:
                    break
                if self._sitl_conn:
                    self._sitl_conn.write(data)
        except:
            pass
        finally:
            with self._lock:
                if client in self._clients:
                    self._clients.remove(client)
            try:
                client.close()
            except:
                pass

    def _broadcast_to_clients(self, data: bytes):
        with self._lock:
            dead_clients = []
            for client in self._clients:
                try:
                    client.sendall(data)
                except:
                    dead_clients.append(client)
            for client in dead_clients:
                self._clients.remove(client)
                try:
                    client.close()
                except:
                    pass

    def send_rc_override(self, channels: dict):
        if not self._sitl_conn:
            return

        ch = [0] * 8
        for i, val in channels.items():
            if 1 <= i <= 8:
                ch[i - 1] = val

        self._sitl_conn.mav.rc_channels_override_send(
            self._sitl_conn.target_system,
            self._sitl_conn.target_component,
            ch[0], ch[1], ch[2], ch[3], ch[4], ch[5], ch[6], ch[7]
        )
        self._event_bus.emit(Event.RC_OVERRIDE_SENT, channels)

    def release_rc_override(self):
        self.send_rc_override({1: 0, 2: 0, 3: 0, 4: 0})

    def set_mode(self, mode_name: str):
        """Set ArduPilot flight mode (e.g., 'CRUISE', 'MANUAL', 'AUTO')"""
        if not self._sitl_conn:
            logger.warning(f"SET MODE FAILED: No SITL connection, mode={mode_name}")
            return

        # ArduPilot plane mode numbers
        mode_mapping = {
            'MANUAL': 0,
            'CIRCLE': 1,
            'STABILIZE': 2,
            'TRAINING': 3,
            'ACRO': 4,
            'FLY_BY_WIRE_A': 5,
            'FLY_BY_WIRE_B': 6,
            'CRUISE': 7,
            'AUTOTUNE': 8,
            'AUTO': 10,
            'RTL': 11,
            'LOITER': 12,
            'GUIDED': 15,
        }

        if mode_name not in mode_mapping:
            logger.error(f"SET MODE FAILED: Unknown mode={mode_name}")
            print(f"Unknown mode: {mode_name}")
            return

        mode_id = mode_mapping[mode_name]
        self._sitl_conn.set_mode(mode_id)
        logger.info(f"SET MODE: {mode_name} (id={mode_id})")

    def on_message(self, callback: Callable):
        self._message_callbacks.append(callback)

    def get_telemetry(self) -> TelemetryState:
        return self._telemetry.get_state()

    def is_connected(self) -> bool:
        return self._connected

    def request_data_streams(self, rate: int = 4):
        if not self._sitl_conn:
            return

        self._sitl_conn.mav.request_data_stream_send(
            self._sitl_conn.target_system,
            self._sitl_conn.target_component,
            mavutil.mavlink.MAV_DATA_STREAM_ALL,
            rate, 1
        )
