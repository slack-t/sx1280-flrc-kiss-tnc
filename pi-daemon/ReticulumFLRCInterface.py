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
#   # RNS routing mode: "ptp"/"pointtopoint" for this dedicated backhaul
#   # (RNS does not recognize the hyphenated form "point-to-point")
#   mode = ptp
#   enabled = yes
#
# Optional:
#   bitrate = 150000   # effective radio goodput used by RNS for pacing;
#                      # do NOT set this to the serial baud rate

import threading
import time
import struct
import zlib
import serial
import RNS
from RNS.Interfaces.Interface import Interface

# KISS framing constants
FEND  = 0xC0
FESC  = 0xDB
TFEND = 0xDC
TFESC = 0xDD
KISS_DATA_PORT = 0x00

# Integrity header constants (must match firmware kiss/SerialIntegrity.h)
SERIAL_INTEGRITY_MAGIC = 0x8AC1
SERIAL_INTEGRITY_HDR_LEN = 8

# Firmware payload cap (framing/Framing.h TNC_PAYLOAD_MAX_LEN)
FIRMWARE_PAYLOAD_CAP = 1280

# Conservative effective goodput of the FLRC link (on-air ~325 kb/s,
# half-duplex, minus ARQ/fragmentation overhead). RNS uses this for
# announce pacing and airtime budgeting — overstating it overloads the
# TNC TX queue and starves ARQ credits.
DEFAULT_RADIO_BITRATE = 150000

# Pace host->TNC writes to avoid overflowing the device CDC RX ring
# (same mitigation as kiss_tun.py write_kiss_frame).
SERIAL_WRITE_CHUNK = 64
SERIAL_WRITE_GAP_S = 0.001
SERIAL_WRITE_TIMEOUT_S = 2.0

# Seconds between reconnection attempts after a serial failure
RECONNECT_WAIT_S = 5


def close_stalled_serial(ser):
    try:
        if hasattr(ser, "cancel_write"):
            ser.cancel_write()
    except Exception:
        pass
    try:
        ser.reset_output_buffer()
    except Exception:
        pass
    try:
        ser.close()
    except Exception:
        pass

class ReticulumFLRCInterface(Interface):
    """
    Native Reticulum Interface for the custom SX1280 FLRC TNC.
    Handles physical serial communication and wraps/unwraps packets in the
    TNC's 8-byte serial integrity envelope.
    """

    DEFAULT_IFAC_SIZE = 8

    owner    = None
    port     = None
    speed    = None
    serial   = None

    def __init__(self, owner, configuration):
        super().__init__()

        # Read parameters from config using RNS methodology
        ifconf = Interface.get_config_obj(configuration)

        self.name = ifconf.get("name", "FLRC TNC Interface")
        self.port = ifconf.get("port", "/dev/ttyACM0")
        self.speed = int(ifconf.get("speed", 921600))

        self.mtu = int(ifconf.get("mtu", FIRMWARE_PAYLOAD_CAP))
        if self.mtu > FIRMWARE_PAYLOAD_CAP:
            RNS.log(f"[{self.name}] Configured mtu {self.mtu} exceeds firmware payload cap, clamping to {FIRMWARE_PAYLOAD_CAP}", RNS.LOG_WARNING)
            self.mtu = FIRMWARE_PAYLOAD_CAP

        # Tell Reticulum our hardware packet size cap
        self.HW_MTU = self.mtu

        self.online = False
        # Effective radio goodput, NOT the serial baud rate
        self.bitrate = int(ifconf.get("bitrate", DEFAULT_RADIO_BITRATE))

        self.owner = owner
        self.serial = None

        # Serializes frame writes: RNS calls process_outgoing() from
        # multiple threads (Transport + announce queue timers), and a
        # chunk-paced write must not interleave with another frame.
        self.tx_lock = threading.Lock()

        self.rx_integrity_drops = 0

        # KISS stream parser state
        self._rx_state = 0  # 0=IDLE, 1=IN_FRAME, 2=ESCAPE
        self._rx_buf = bytearray()
        self._rx_overflow = False

        # The read loop owns connecting and reconnecting; it retries
        # until the port opens, so a failure here is not fatal.
        self.running = True
        self.read_thread = threading.Thread(target=self.read_loop, daemon=True)
        self.read_thread.start()

    def open_serial(self):
        """(Re)open the serial port. Returns True on success."""
        with self.tx_lock:
            if self.serial:
                try:
                    self.serial.close()
                except Exception:
                    pass
                self.serial = None

            try:
                RNS.log(f"[{self.name}] Opening serial port {self.port} at {self.speed} baud...", RNS.LOG_VERBOSE)
                # Fresh Serial object each attempt so a stale fd from a
                # USB re-enumeration can't linger.
                s = serial.Serial()
                s.port = self.port
                s.baudrate = self.speed
                s.timeout = 0.1
                s.dsrdtr = False
                s.rtscts = False
                s.dtr = False
                s.rts = False
                s.write_timeout = SERIAL_WRITE_TIMEOUT_S
                s.open()
                s.dtr = False
                s.rts = False

                s.reset_input_buffer()
                s.reset_output_buffer()

                # Discard any partial frame from before the reconnect
                self._rx_state = 0
                self._rx_buf.clear()
                self._rx_overflow = False

                self.serial = s
                self.online = True
                RNS.log(f"[{self.name}] Connected to physical serial port {self.port} at {self.speed} baud.", RNS.LOG_INFO)
                return True
            except Exception as e:
                RNS.log(f"[{self.name}] Failed to open serial port {self.port}: {e}", RNS.LOG_ERROR)
                self.online = False
                return False

    def read_loop(self):
        while self.running:
            try:
                if not self.online or not self.serial or not self.serial.is_open:
                    if not self.open_serial():
                        time.sleep(RECONNECT_WAIT_S)
                    continue

                # Blocking read (0.1s timeout), then drain whatever else arrived
                data = self.serial.read(1)
                if data:
                    waiting = self.serial.in_waiting
                    if waiting:
                        data += self.serial.read(waiting)
                    self.process_rx_data(data)
            except Exception as e:
                if self.running:
                    RNS.log(f"[{self.name}] Error in read loop: {e}", RNS.LOG_ERROR)
                    self.online = False
                    time.sleep(RECONNECT_WAIT_S)

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
                        self._rx_buf.clear()
                        self._rx_overflow = False
                        self._rx_state = 0
                    else:
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

    def _integrity_drop(self, reason):
        self.rx_integrity_drops += 1
        RNS.log(f"[{self.name}] RX integrity drop #{self.rx_integrity_drops}: {reason}", RNS.LOG_DEBUG)

    def handle_inbound_frame(self, frame):
        """Validate the serial integrity header and forward raw payload to RNS."""
        if len(frame) < SERIAL_INTEGRITY_HDR_LEN:
            self._integrity_drop(f"runt frame len={len(frame)}")
            return

        magic, length, crc = struct.unpack(">HHI", frame[:SERIAL_INTEGRITY_HDR_LEN])
        if magic != SERIAL_INTEGRITY_MAGIC:
            self._integrity_drop(f"bad magic {hex(magic)}")
            return
        if length != len(frame) - SERIAL_INTEGRITY_HDR_LEN:
            self._integrity_drop(f"length mismatch header={length} actual={len(frame) - SERIAL_INTEGRITY_HDR_LEN}")
            return

        payload = frame[SERIAL_INTEGRITY_HDR_LEN:]

        computed_crc = zlib.crc32(payload) & 0xFFFFFFFF
        if computed_crc != crc:
            self._integrity_drop(f"CRC mismatch header={hex(crc)} computed={hex(computed_crc)}")
            return

        self.rxb += len(payload)

        # An error in the RNS stack must be visible, and must not be
        # mistaken for a serial failure by the read loop.
        try:
            self.owner.inbound(payload, self)
        except Exception as e:
            RNS.log(f"[{self.name}] Error while handling inbound packet in RNS: {e}", RNS.LOG_ERROR)

    def process_outgoing(self, data):
        """Wrap outbound packet with integrity header, KISS encode, and write."""
        if not self.online:
            return False

        if len(data) > FIRMWARE_PAYLOAD_CAP:
            RNS.log(f"[{self.name}] Dropping outbound packet: {len(data)} bytes exceeds firmware payload cap {FIRMWARE_PAYLOAD_CAP}", RNS.LOG_ERROR)
            return False

        crc = zlib.crc32(data) & 0xFFFFFFFF
        header = struct.pack(">HHI", SERIAL_INTEGRITY_MAGIC, len(data), crc)
        wrapped = header + data

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

        try:
            with self.tx_lock:
                if not self.serial or not self.serial.is_open:
                    return False
                for offset in range(0, len(encoded), SERIAL_WRITE_CHUNK):
                    chunk = encoded[offset:offset+SERIAL_WRITE_CHUNK]
                    written = self.serial.write(chunk)
                    if written != len(chunk):
                        raise serial.SerialTimeoutException(
                            f"partial serial write {written}/{len(chunk)} bytes"
                        )
                    self.serial.flush()
                    if offset + SERIAL_WRITE_CHUNK < len(encoded):
                        time.sleep(SERIAL_WRITE_GAP_S)

            self.txb += len(data)
            return True
        except serial.SerialTimeoutException as e:
            RNS.log(f"[{self.name}] Serial write timed out, reopening port: {e}", RNS.LOG_ERROR)
            if self.serial:
                close_stalled_serial(self.serial)
            self.online = False
            return False
        except Exception as e:
            RNS.log(f"[{self.name}] Error transmitting: {e}", RNS.LOG_ERROR)
            # The read loop notices online == False and reconnects.
            self.online = False
            return False

    def detach(self):
        """Called when Reticulum shuts down the interface."""
        self.detached = True
        self.running = False
        self.online = False
        if self.serial and self.serial.is_open:
            try:
                self.serial.close()
            except Exception:
                pass
        RNS.log(f"[{self.name}] Interface detached.", RNS.LOG_INFO)

    def should_ingress_limit(self):
        return False

    def __str__(self):
        return f"ReticulumFLRCInterface[{self.name}]"

# Register the interface class with Reticulum
interface_class = ReticulumFLRCInterface
