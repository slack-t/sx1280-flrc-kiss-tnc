# ReticulumFLRCInterface.py
# Place this file in ~/.reticulum/interfaces/
#
# Then configure it in your Reticulum config (~/.reticulum/config):
#
# [[FLRC TNC Interface]]
#   type = ReticulumFLRCInterface
#   port = /dev/ttyACM0
#   speed = 921600
#   mtu = 1280
#   mode = point-to-point
#   enabled = yes

import threading
import time
import struct
import zlib
import serial
from RNS.Interfaces import Interface

# KISS framing constants
FEND  = 0xC0
FESC  = 0xDB
TFEND = 0xDC
TFESC = 0xDD
KISS_DATA_PORT = 0x00

# Integrity header constants
SERIAL_INTEGRITY_MAGIC = 0x8AC1
SERIAL_INTEGRITY_HDR_LEN = 8


class ReticulumFLRCInterface(Interface):
    """
    Native Reticulum Interface for the custom SX1280 FLRC TNC.
    Handles physical serial communication and wraps/unwraps packets in the
    TNC's 8-byte serial integrity envelope.
    """
    
    def __init__(self, owner, configuration):
        super().__init__()
        self.owner = owner
        
        # Read parameters from config
        self.name = configuration.get("name", "FLRC TNC Interface")
        self.port = configuration.get("port", "/dev/ttyACM0")
        self.speed = int(configuration.get("speed", 921600))
        self.mtu = int(configuration.get("mtu", 1280))
        
        # Tell Reticulum our hardware packet size cap (1280 bytes)
        self.HW_MTU = self.mtu
        
        self.serial = None
        self.running = False
        
        # KISS stream parser state
        self._rx_state = 0  # 0=IDLE, 1=IN_FRAME, 2=ESCAPE
        self._rx_buf = bytearray()
        self._rx_overflow = False
        
        # Reader thread
        self.read_thread = None
        
        # Initialize physical serial connection
        self.open_serial()

    def open_serial(self):
        try:
            self.serial = serial.Serial()
            self.serial.port = self.port
            self.serial.baudrate = self.speed
            self.serial.timeout = 0
            self.serial.dsrdtr = False
            self.serial.rtscts = False
            self.serial.dtr = False
            self.serial.rts = False
            self.serial.open()
            self.serial.dtr = False
            self.serial.rts = False
            
            self.serial.reset_input_buffer()
            self.serial.reset_output_buffer()
            
            self.running = True
            self.read_thread = threading.Thread(target=self.read_loop, daemon=True)
            self.read_thread.start()
            
            self.online = True
            print(f"[{self.name}] Connected to physical serial port {self.port} at {self.speed} baud.", flush=True)
        except Exception as e:
            print(f"[{self.name}] Failed to open serial port {self.port}: {e}", flush=True)
            self.online = False

    def read_loop(self):
        while self.running:
            try:
                if not self.serial or not self.serial.is_open:
                    time.sleep(1)
                    continue
                
                waiting = self.serial.in_waiting
                if waiting:
                    data = self.serial.read(waiting)
                    self.process_rx_data(data)
                else:
                    time.sleep(0.001)
            except Exception as e:
                print(f"[{self.name}] Error in read loop: {e}", flush=True)
                self.online = False
                time.sleep(2)
                self.reconnect()

    def reconnect(self):
        self.online = False
        if self.serial and self.serial.is_open:
            try:
                self.serial.close()
            except Exception:
                pass
        
        print(f"[{self.name}] Reconnecting to TNC on {self.port}...", flush=True)
        try:
            self.serial.open()
            self.serial.reset_input_buffer()
            self.serial.reset_output_buffer()
            self.online = True
            print(f"[{self.name}] Reconnection successful.", flush=True)
        except Exception:
            pass

    def process_rx_data(self, data):
        """Standard KISS stream byte parser."""
        for b in data:
            if self._rx_state == 0:  # IDLE
                if b == FEND:
                    self._rx_buf.clear()
                    self._rx_overflow = False
                    self._rx_state = 1  # IN_FRAME
            elif self._rx_state == 1:  # IN_FRAME
                if b == FEND:
                    if len(self._rx_buf) == 0:
                        self._rx_overflow = False
                        self._rx_state = 0
                    elif self._rx_overflow:
                        self._rx_buf.clear()
                        self._rx_overflow = False
                        self._rx_state = 0
                    elif self._rx_buf[0] != KISS_DATA_PORT:
                        # Ignore frames destined for command/control ports
                        self._rx_buf.clear()
                        self._rx_overflow = False
                        self._rx_state = 0
                    else:
                        # Extract the unescaped packet data
                        payload = bytes(self._rx_buf[1:])
                        self._rx_buf.clear()
                        self._rx_overflow = False
                        self._rx_state = 0
                        self.handle_inbound_frame(payload)
                elif b == FESC:
                    self._rx_state = 2  # ESCAPE
                else:
                    if len(self._rx_buf) < self.HW_MTU + SERIAL_INTEGRITY_HDR_LEN + 1:
                        self._rx_buf.append(b)
                    else:
                        self._rx_overflow = True
            elif self._rx_state == 2:  # ESCAPE
                if b not in (TFEND, TFESC):
                    # Malformed sequence: drop packet
                    self._rx_buf.clear()
                    self._rx_overflow = False
                    self._rx_state = 0
                else:
                    actual_byte = FEND if b == TFEND else FESC
                    if len(self._rx_buf) < self.HW_MTU + SERIAL_INTEGRITY_HDR_LEN + 1:
                        self._rx_buf.append(actual_byte)
                    else:
                        self._rx_overflow = True
                    self._rx_state = 1

    def handle_inbound_frame(self, frame):
        """Validate the serial integrity header and forward raw payload to RNS."""
        if len(frame) < SERIAL_INTEGRITY_HDR_LEN:
            return
        
        try:
            # Parse the header fields
            magic, length, crc = struct.unpack(">HHI", frame[:SERIAL_INTEGRITY_HDR_LEN])
            if magic != SERIAL_INTEGRITY_MAGIC:
                return
            if length != len(frame) - SERIAL_INTEGRITY_HDR_LEN:
                return
            
            payload = frame[SERIAL_INTEGRITY_HDR_LEN:]
            
            # Verify packet integrity
            computed_crc = zlib.crc32(payload) & 0xFFFFFFFF
            if computed_crc != crc:
                return
            
            # Forward data inbound to the Reticulum transport engine
            self.rxs += 1
            self.owner.inbound(payload, self)
        except Exception:
            pass

    def transmit(self, data):
        """Wrap outbound packet with integrity header, KISS encode, and write."""
        if not self.online or not self.serial or not self.serial.is_open:
            return False
            
        try:
            # Build the 8-byte serial integrity wrapper
            crc = zlib.crc32(data) & 0xFFFFFFFF
            header = struct.pack(">HHI", SERIAL_INTEGRITY_MAGIC, len(data), crc)
            wrapped = header + data
            
            # Perform standard KISS escaping
            encoded = bytearray()
            encoded.append(FEND)
            encoded.append(KISS_DATA_PORT)
            for b in wrapped:
                if b == FEND:
                    encoded.append(FESC)
                    encoded.append(TFEND)
                elif b == FESC:
                    encoded.append(FESC)
                    encoded.append(TFESC)
                else:
                    encoded.append(b)
            encoded.append(FEND)
            
            # Pacing: Write in 64-byte chunks with 1ms sleeps to avoid CDC TX ring overflows
            chunk_size = 64
            for offset in range(0, len(encoded), chunk_size):
                chunk = encoded[offset:offset+chunk_size]
                self.serial.write(chunk)
                self.serial.flush()
                if offset + chunk_size < len(encoded):
                    time.sleep(0.001)
                    
            self.txs += 1
            return True
        except Exception as e:
            print(f"[{self.name}] Error transmitting: {e}", flush=True)
            self.online = False
            return False

    def detach(self):
        """Called when Reticulum shuts down the interface."""
        self.running = False
        self.online = False
        if self.serial and self.serial.is_open:
            try:
                self.serial.close()
            except Exception:
                pass
        print(f"[{self.name}] Interface detached.", flush=True)
