#!/usr/bin/env python3
"""
reticulum_kiss_bridge.py — Bridge adapter between Reticulum (standard KISS)
and the custom SX1280 FLRC TNC firmware (serial-integrity wrapped KISS).

This script creates a virtual serial port (PTY) for Reticulum to connect to.
It wraps outbound packets from Reticulum with the TNC's 8-byte serial integrity
header, and unwraps/verifies inbound packets from the TNC before sending them
to Reticulum.
"""

import argparse
import errno
import os
import pty
import sys
import termios
import threading
import time
import tty
import serial

# Add current directory to path to import local modules
sys.path.append(os.path.dirname(os.path.abspath(__file__)))

import serial_integrity
from kiss_tun import (
    FIRMWARE_PAYLOAD_CAP,
    KISS_DATA_PORT,
    KissDecoder,
    kiss_encode,
    write_kiss_frame,
)

# Reconnection delay in seconds on hardware disconnect
RECONNECT_DELAY_S = 5


def setup_raw_pty():
    """Create a PTY pair and configure the slave to run in raw mode."""
    master, slave = pty.openpty()
    
    # Configure the slave PTY as a raw device so the kernel doesn't
    # alter special bytes (like FEND 0xC0 or ESC 0xDB) or generate signals.
    tty.setraw(slave)
    
    # Disable HUPCL (hangup on close) so the PTY survives client disconnects
    attrs = termios.tcgetattr(slave)
    attrs[2] &= ~termios.HUPCL
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    
    slave_name = os.ttyname(slave)
    return master, slave, slave_name


def reticulum_to_tnc_thread(master_pty, ser_conn, mtu, stop_event, debug):
    """
    Read standard KISS frames from Reticulum, wrap them with serial integrity,
    and send them to the hardware TNC.
    """
    # Reticulum sends unwrapped payloads (up to mtu)
    decoder = KissDecoder(mtu)
    
    print("[bridge] Reticulum -> TNC forwarding thread started.", flush=True)
    while not stop_event.is_set():
        try:
            # Read from PTY master. This blocks until Reticulum writes data.
            # If Reticulum is disconnected, it raises EIO.
            try:
                data = os.read(master_pty, 4096)
                if not data:
                    time.sleep(0.1)
                    continue
            except OSError as e:
                if e.errno == errno.EIO:
                    # No client connected to the slave PTY; wait and try again
                    time.sleep(0.2)
                    continue
                raise

            # Feed bytes into standard KISS decoder
            for port, payload in decoder.feed(data):
                if port == KISS_DATA_PORT and payload:
                    if debug:
                        print(f"[bridge] TX (Ret -> TNC) raw len={len(payload)} hex={payload.hex()[:32]}...", flush=True)
                    
                    if len(payload) > FIRMWARE_PAYLOAD_CAP:
                        print(f"[bridge] ERROR: Reticulum packet too large ({len(payload)} > {FIRMWARE_PAYLOAD_CAP})", flush=True)
                        continue
                        
                    # Wrap with the 8-byte serial integrity header (Magic + Len + CRC32)
                    wrapped = serial_integrity.wrap_payload(payload)
                    # Encode wrapped payload back into a KISS data frame
                    encoded = kiss_encode(wrapped)
                    
                    # Write to hardware serial in chunks to avoid overwhelming the CDC RX ring
                    if ser_conn and ser_conn.is_open:
                        write_kiss_frame(ser_conn, encoded)
                        
        except Exception as e:
            print(f"[bridge] Error in Reticulum->TNC thread: {e}", flush=True)
            time.sleep(1)


def tnc_to_reticulum_thread(master_pty, ser_conn, mtu, stop_event, debug):
    """
    Read wrapped KISS frames from the TNC, unwrap/verify the serial integrity,
    and write standard KISS frames to Reticulum.
    """
    # TNC sends wrapped payloads (mtu + 8-byte integrity header)
    decoder = KissDecoder(mtu + serial_integrity.SERIAL_INTEGRITY_HDR_LEN)
    
    print("[bridge] TNC -> Reticulum forwarding thread started.", flush=True)
    while not stop_event.is_set():
        try:
            if not ser_conn or not ser_conn.is_open:
                time.sleep(0.1)
                continue
                
            # Read whatever bytes are waiting on the physical TNC serial port
            waiting = ser_conn.in_waiting
            if waiting:
                data = ser_conn.read(waiting)
                
                for port, payload in decoder.feed(data):
                    if port == KISS_DATA_PORT and payload:
                        try:
                            # Unwrap payload, which validates the magic and CRC32
                            unwrapped = serial_integrity.unwrap_payload(payload)
                            
                            if debug:
                                print(f"[bridge] RX (TNC -> Ret) unwrapped len={len(unwrapped)} hex={unwrapped.hex()[:32]}...", flush=True)
                                
                            # Re-encode unwrapped payload as a standard KISS frame for Reticulum
                            encoded = kiss_encode(unwrapped)
                            
                            # Write to Reticulum via the PTY master
                            try:
                                os.write(master_pty, encoded)
                            except OSError as e:
                                if e.errno == errno.EIO:
                                    # Reticulum not connected; discard frame
                                    pass
                                else:
                                    raise
                        except ValueError as integrity_err:
                            print(f"[bridge] TNC RX Serial Integrity Drop: {integrity_err}", flush=True)
            else:
                time.sleep(0.001)  # Limit polling CPU usage
                
        except (serial.SerialException, OSError) as serial_err:
            print(f"[bridge] TNC serial disconnect/error: {serial_err}", flush=True)
            stop_event.set()
        except Exception as e:
            print(f"[bridge] Error in TNC->Reticulum thread: {e}", flush=True)
            time.sleep(1)


def main():
    parser = argparse.ArgumentParser(
        description="Reticulum KISS Bridge for Custom SX1280 TNC Firmware"
    )
    parser.add_argument("--port", default="/dev/ttyACM0", help="TNC physical serial port (default: /dev/ttyACM0)")
    parser.add_argument("--baud", type=int, default=921600, help="TNC baud rate (default: 921600)")
    parser.add_argument("--virtual-port", default="/tmp/kiss_reticulum", help="Symlink path for the Reticulum virtual port (default: /tmp/kiss_reticulum)")
    parser.add_argument("--mtu", type=int, default=FIRMWARE_PAYLOAD_CAP, help=f"MTU payload limit (default: {FIRMWARE_PAYLOAD_CAP})")
    parser.add_argument("--debug", action="store_true", help="Print debug hex traces of forwarded packets")
    args = parser.parse_args()

    if args.mtu > FIRMWARE_PAYLOAD_CAP:
        sys.exit(f"MTU {args.mtu} exceeds firmware payload limit {FIRMWARE_PAYLOAD_CAP}")

    # Create the raw PTY pair
    try:
        master_pty, slave_pty, slave_name = setup_raw_pty()
    except Exception as e:
        sys.exit(f"Failed to create virtual PTY device: {e}")

    # Expose the virtual port symlink
    if os.path.exists(args.virtual_port):
        try:
            os.unlink(args.virtual_port)
        except OSError:
            pass
    try:
        os.symlink(slave_name, args.virtual_port)
        print(f"[bridge] Exposed virtual serial port at: {args.virtual_port} -> {slave_name}", flush=True)
    except OSError as e:
        sys.exit(f"Failed to create symlink at {args.virtual_port}: {e}")

    print("[bridge] Press Ctrl-C to stop.", flush=True)

    try:
        while True:
            try:
                print(f"[bridge] Connecting to TNC on {args.port} ({args.baud} baud)...", flush=True)
                ser = serial.Serial()
                ser.port = args.port
                ser.baudrate = args.baud
                ser.timeout = 0
                ser.dsrdtr = False
                ser.rtscts = False
                ser.dtr = False
                ser.rts = False
                ser.open()
                ser.dtr = False
                ser.rts = False
                
                # Clear standard buffers
                ser.reset_input_buffer()
                ser.reset_output_buffer()
                print("[bridge] TNC hardware serial connected.", flush=True)

                # Control event to signal threads if serial port fails
                stop_event = threading.Event()
                
                # Start forwarding threads
                t_tx = threading.Thread(
                    target=reticulum_to_tnc_thread,
                    args=(master_pty, ser, args.mtu, stop_event, args.debug),
                    daemon=True
                )
                t_rx = threading.Thread(
                    target=tnc_to_reticulum_thread,
                    args=(master_pty, ser, args.mtu, stop_event, args.debug),
                    daemon=True
                )
                
                t_tx.start()
                t_rx.start()

                # Block main thread until the serial connection encounters an error/disconnects
                while not stop_event.is_set():
                    time.sleep(0.5)

                ser.close()
                print(f"[bridge] TNC disconnected. Reconnecting in {RECONNECT_DELAY_S} seconds...", flush=True)
                time.sleep(RECONNECT_DELAY_S)

            except (serial.SerialException, OSError) as e:
                print(f"[bridge] Failed to connect/read from {args.port}: {e}", flush=True)
                print(f"[bridge] Retrying in {RECONNECT_DELAY_S} seconds...", flush=True)
                time.sleep(RECONNECT_DELAY_S)

    except KeyboardInterrupt:
        print("\n[bridge] Stopping bridge...", flush=True)
    finally:
        # Cleanup
        try:
            os.close(master_pty)
            os.close(slave_pty)
        except OSError:
            pass
        if os.path.exists(args.virtual_port):
            try:
                os.unlink(args.virtual_port)
            except OSError:
                pass
        print("[bridge] Bridge stopped and cleanup complete.", flush=True)


if __name__ == "__main__":
    main()
