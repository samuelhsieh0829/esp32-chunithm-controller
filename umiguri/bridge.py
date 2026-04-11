import argparse
import ctypes
import logging
import struct
import threading
import time
from ctypes import wintypes
from typing import Dict, List, Optional, Sequence, Set, Tuple

import serial


# ESP32 packet stream format
ESP32_SYNC = 0xFF
ESP32_ESCAPE = 0xFD
ESP32_CMD_REPORT = 0x81
ESP32_VERSION = 0x01
TOUCH_COUNT = 16
IR_COUNT = 6
ESP32_PAYLOAD_SIZE = 4 + TOUCH_COUNT * 2 + IR_COUNT * 2

# OpeNITHM keyboard mapping
OPENITHM_TOP_ROW = ["1", "q", "2", "w", "3", "e", "4", "r", "5", "t", "6", "y", "7", "u", "8", "i"]
OPENITHM_BOTTOM_ROW = ["a", "z", "s", "x", "d", "c", "f", "v", "g", "b", "h", "n", "j", "m", "k", ","]
OPENITHM_AIR_KEYS = ["/", ".", "'", ";", "]", "["]

# Virtual key codes
VK_1 = 0x31
VK_2 = 0x32
VK_3 = 0x33
VK_4 = 0x34
VK_5 = 0x35
VK_6 = 0x36
VK_7 = 0x37
VK_8 = 0x38
VK_A = 0x41
VK_B = 0x42
VK_C = 0x43
VK_D = 0x44
VK_E = 0x45
VK_F = 0x46
VK_G = 0x47
VK_H = 0x48
VK_I = 0x49
VK_J = 0x4A
VK_K = 0x4B
VK_M = 0x4D
VK_N = 0x4E
VK_Q = 0x51
VK_R = 0x52
VK_S = 0x53
VK_T = 0x54
VK_U = 0x55
VK_V = 0x56
VK_W = 0x57
VK_X = 0x58
VK_Y = 0x59
VK_Z = 0x5A
VK_HOME = 0x24
VK_END = 0x23
VK_PRIOR = 0x21  # Page Up
VK_NEXT = 0x22   # Page Down
VK_OEM_1 = 0xBA        # ;
VK_OEM_COMMA = 0xBC    # ,
VK_OEM_PERIOD = 0xBE   # .
VK_OEM_2 = 0xBF        # /
VK_OEM_4 = 0xDB        # [
VK_OEM_6 = 0xDD        # ]
VK_OEM_7 = 0xDE        # '

KEY_MAP: Dict[str, int] = {
    "1": VK_1,
    "2": VK_2,
    "3": VK_3,
    "4": VK_4,
    "5": VK_5,
    "6": VK_6,
    "7": VK_7,
    "8": VK_8,
    "a": VK_A,
    "b": VK_B,
    "c": VK_C,
    "d": VK_D,
    "e": VK_E,
    "f": VK_F,
    "g": VK_G,
    "h": VK_H,
    "i": VK_I,
    "j": VK_J,
    "k": VK_K,
    "m": VK_M,
    "n": VK_N,
    "q": VK_Q,
    "r": VK_R,
    "s": VK_S,
    "t": VK_T,
    "u": VK_U,
    "v": VK_V,
    "w": VK_W,
    "x": VK_X,
    "y": VK_Y,
    "z": VK_Z,
    ",": VK_OEM_COMMA,
    ".": VK_OEM_PERIOD,
    "/": VK_OEM_2,
    "'": VK_OEM_7,
    ";": VK_OEM_1,
    "[": VK_OEM_4,
    "]": VK_OEM_6,
    "home": VK_HOME,
    "end": VK_END,
    "page_up": VK_PRIOR,
    "page_down": VK_NEXT,
}


KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002

EXTENDED_KEYS = {VK_HOME, VK_END, VK_PRIOR, VK_NEXT}


class WinKeyboard:
    def __init__(self) -> None:
        self._user32 = ctypes.windll.user32
        self._user32.keybd_event.argtypes = (
            wintypes.BYTE,
            wintypes.BYTE,
            wintypes.DWORD,
            wintypes.ULONG,
        )
        self._user32.keybd_event.restype = None
        self._user32.MapVirtualKeyW.argtypes = (wintypes.UINT, wintypes.UINT)
        self._user32.MapVirtualKeyW.restype = wintypes.UINT

    def _send_vk(self, vk: int, key_up: bool) -> None:
        scan = self._user32.MapVirtualKeyW(vk, 0)
        flags = 0
        if vk in EXTENDED_KEYS:
            flags |= KEYEVENTF_EXTENDEDKEY
        if key_up:
            flags |= KEYEVENTF_KEYUP

        self._user32.keybd_event(vk, scan, flags, 0)

    def press(self, key_name: str) -> None:
        self._send_vk(KEY_MAP[key_name], key_up=False)

    def release(self, key_name: str) -> None:
        self._send_vk(KEY_MAP[key_name], key_up=True)

    def tap(self, key_name: str) -> None:
        self.press(key_name)
        self.release(key_name)


class EscapedFrameParser:
    def __init__(self, sync: int, esc: int, max_argc: int = 255) -> None:
        self.sync = sync
        self.esc = esc
        self.max_argc = max_argc
        self._started = False
        self._esc_pending = False
        self._checksum = 0
        self._buf = bytearray()
        self._argc: Optional[int] = None

    def _reset(self) -> None:
        self._started = False
        self._esc_pending = False
        self._checksum = 0
        self._buf.clear()
        self._argc = None

    def _start(self) -> None:
        self._started = True
        self._esc_pending = False
        self._checksum = self.sync
        self._buf.clear()
        self._argc = None

    def feed(self, data: bytes) -> list[Tuple[int, bytes]]:
        frames: list[Tuple[int, bytes]] = []

        for raw_byte in data:
            frame = self.push(raw_byte)
            if frame is not None:
                frames.append(frame)

        return frames

    def push(self, raw_byte: int) -> Optional[Tuple[int, bytes]]:
        if raw_byte == self.sync:
            self._start()
            return None

        if not self._started:
            return None

        if raw_byte == self.esc:
            self._esc_pending = True
            return None

        byte = raw_byte
        if self._esc_pending:
            self._esc_pending = False
            byte = (byte + 1) & 0xFF

        self._buf.append(byte)
        self._checksum = (self._checksum + byte) & 0xFF

        if len(self._buf) == 2:
            self._argc = self._buf[1]
            if self._argc > self.max_argc:
                self._reset()
                return None

        if self._argc is None:
            return None

        expected_len = self._argc + 3
        if len(self._buf) < expected_len:
            return None

        if len(self._buf) > expected_len or self._checksum != 0:
            self._reset()
            return None

        cmd = self._buf[0]
        args = bytes(self._buf[2: 2 + self._argc])
        self._reset()
        return cmd, args


class OpenIthmKeyboardBridge:
    def __init__(
        self,
        esp32_serial: serial.Serial,
        reverse_touch_order: bool,
        touch_single_threshold: int,
        touch_double_threshold: int,
        air_mode: str,
        air_thresholds: Sequence[int],
        air_hysteresis: int,
        air_higher_is_blocked: bool,
        ignore_ir_mask: bool,
        motion_epsilon: float,
    ) -> None:
        self.esp32_serial = esp32_serial
        self.reverse_touch_order = reverse_touch_order
        self.touch_single_threshold = touch_single_threshold
        self.touch_double_threshold = touch_double_threshold
        self.air_mode = air_mode
        self.air_thresholds = list(air_thresholds)
        self.air_hysteresis = air_hysteresis
        self.air_higher_is_blocked = air_higher_is_blocked
        self.ignore_ir_mask = ignore_ir_mask
        self.motion_epsilon = motion_epsilon

        self._state_lock = threading.Lock()
        self._stop_event = threading.Event()
        self._keyboard = WinKeyboard()

        self._pressed_keys: Set[str] = set()
        self._ir_blocked: List[bool] = [False] * IR_COUNT
        self._last_position = 0.0
        self._was_air_held = False

        self._packets_seen = 0
        self._last_stats_log = 0.0

        self._threads: list[threading.Thread] = []

    def start(self) -> None:
        self._threads = [
            threading.Thread(target=self._esp32_reader_worker, name="esp32-reader", daemon=True),
        ]

        for thread in self._threads:
            thread.start()

    def stop(self) -> None:
        self._stop_event.set()

        for thread in self._threads:
            thread.join(timeout=1.0)

        self._release_all_pressed()

        try:
            if self.esp32_serial.is_open:
                self.esp32_serial.close()
        except serial.SerialException:
            pass

    def wait(self) -> None:
        while not self._stop_event.is_set():
            time.sleep(0.1)

    def _apply_key_state(self, key_name: str, should_press: bool) -> None:
        currently_pressed = key_name in self._pressed_keys

        if should_press and not currently_pressed:
            self._keyboard.press(key_name)
            self._pressed_keys.add(key_name)
        elif not should_press and currently_pressed:
            self._keyboard.release(key_name)
            self._pressed_keys.remove(key_name)

    def _tap_key(self, key_name: str) -> None:
        self._keyboard.tap(key_name)

    def _release_all_pressed(self) -> None:
        for key_name in list(self._pressed_keys):
            try:
                self._keyboard.release(key_name)
            except OSError:
                pass
        self._pressed_keys.clear()

    def _ordered_touches(self, touches: Sequence[int]) -> Tuple[int, ...]:
        if self.reverse_touch_order:
            return tuple(reversed(touches))
        return tuple(touches)

    def _decode_ir_blocked(self, ir_values: Sequence[int], ir_valid_mask: int) -> List[bool]:
        blocked = [False] * IR_COUNT

        for i in range(IR_COUNT):
            bit = 1 << i
            if not self.ignore_ir_mask and (ir_valid_mask & bit) == 0:
                self._ir_blocked[i] = False
                blocked[i] = False
                continue

            value = int(ir_values[i])
            threshold = self.air_thresholds[i]
            upper = threshold + self.air_hysteresis
            lower = threshold - self.air_hysteresis
            if lower < 0:
                lower = 0

            current = self._ir_blocked[i]
            if self.air_higher_is_blocked:
                if not current:
                    current = value >= upper
                else:
                    current = value >= lower
            else:
                if not current:
                    current = value <= lower
                else:
                    current = value <= upper

            self._ir_blocked[i] = current
            blocked[i] = current

        return blocked

    def _handle_touch(self, touches: Sequence[int]) -> None:
        ordered = self._ordered_touches(touches)

        for i, raw in enumerate(ordered):
            pressed_bottom = raw >= self.touch_single_threshold
            pressed_top = raw >= self.touch_double_threshold
            self._apply_key_state(OPENITHM_BOTTOM_ROW[i], pressed_bottom)
            self._apply_key_state(OPENITHM_TOP_ROW[i], pressed_top)

    def _compute_air_position(self, blocked: Sequence[bool]) -> float:
        active = [idx for idx, is_blocked in enumerate(blocked) if is_blocked]
        if not active:
            return 0.0

        if len(active) == 1:
            return (active[0] + 1) / IR_COUNT

        avg = sum(active) / len(active)
        return (avg + 1) / IR_COUNT

    def _handle_air_keys(self, blocked: Sequence[bool]) -> None:
        for i in range(IR_COUNT):
            self._apply_key_state(OPENITHM_AIR_KEYS[i], blocked[i])

    def _handle_air_motion(self, blocked: Sequence[bool]) -> None:
        position = self._compute_air_position(blocked)

        if position > self._last_position + self.motion_epsilon:
            self._tap_key("page_up")
            if self._was_air_held:
                self._tap_key("end")

        if position < self._last_position - self.motion_epsilon:
            self._tap_key("page_down")
            if self._was_air_held:
                self._tap_key("end")

        if position > 0.05:
            self._apply_key_state("home", True)
            self._was_air_held = True
        else:
            self._apply_key_state("home", False)
            self._was_air_held = False

        self._last_position = position

    def _handle_report(self, payload: bytes) -> None:
        if len(payload) != ESP32_PAYLOAD_SIZE:
            return

        if payload[0] != ESP32_VERSION:
            return

        ir_valid_mask = payload[2]
        touches = struct.unpack_from("<16H", payload, 4)
        irs = struct.unpack_from("<6H", payload, 4 + TOUCH_COUNT * 2)

        blocked = self._decode_ir_blocked(irs, ir_valid_mask)

        with self._state_lock:
            self._handle_touch(touches)
            if self.air_mode == "keys":
                self._handle_air_keys(blocked)
            else:
                self._handle_air_motion(blocked)

    def _esp32_reader_worker(self) -> None:
        parser = EscapedFrameParser(sync=ESP32_SYNC, esc=ESP32_ESCAPE)

        while not self._stop_event.is_set():
            try:
                data = self.esp32_serial.read(256)
            except serial.SerialException as exc:
                logging.error("ESP32 serial read failed: %s", exc)
                self._stop_event.set()
                return

            if not data:
                continue

            for cmd, payload in parser.feed(data):
                if cmd != ESP32_CMD_REPORT:
                    continue
                try:
                    self._handle_report(payload)
                    self._packets_seen += 1
                except OSError as exc:
                    logging.error("Keyboard output failed: %s", exc)
                    self._stop_event.set()
                    return

            now = time.monotonic()
            if now - self._last_stats_log >= 5.0:
                logging.info("Bridge alive: packets=%d", self._packets_seen)
                self._last_stats_log = now


def parse_air_thresholds(raw: str) -> List[int]:
    parts = [item.strip() for item in raw.split(",") if item.strip()]
    if len(parts) != IR_COUNT:
        raise ValueError("--air-thresholds must contain exactly 6 comma-separated values")
    return [int(item) for item in parts]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Bridge ESP32 sensor stream to OpeNITHM keyboard mapping"
    )
    parser.add_argument("--esp32-port", default="COM9", help="ESP32 serial port")
    parser.add_argument("--esp32-baud", type=int, default=2000000, help="ESP32 baud rate")

    # Kept for command-line compatibility with older serial bridge usage.
    parser.add_argument("--umiguri-port", default="COM5", help="Ignored (legacy option)")
    parser.add_argument("--umiguri-baud", type=int, default=115200, help="Ignored (legacy option)")

    parser.add_argument(
        "--touch-single-threshold",
        type=int,
        default=8,
        help="Touch value threshold to press bottom-row key",
    )
    parser.add_argument(
        "--touch-double-threshold",
        type=int,
        default=140,
        help="Touch value threshold to additionally press top-row key",
    )
    parser.add_argument(
        "--reverse-touch-order",
        action="store_true",
        default=True,
        help="Reverse 16-key order before applying OpeNITHM mapping",
    )
    parser.add_argument(
        "--normal-touch-order",
        dest="reverse_touch_order",
        action="store_false",
        help="Disable reversed touch order",
    )
    parser.add_argument(
        "--air-mode",
        choices=["keys", "motion"],
        default="keys",
        help="keys: / . ' ; ] [, motion: Home/End/PageUp/PageDown",
    )
    parser.add_argument(
        "--air-thresholds",
        default="3300,3400,3600,3700,4050,4060",
        help="Comma-separated 6 IR thresholds",
    )
    parser.add_argument(
        "--air-hysteresis",
        type=int,
        default=80,
        help="IR threshold hysteresis",
    )
    parser.add_argument(
        "--air-higher-is-blocked",
        action="store_true",
        default=True,
        help="Treat IR value >= threshold as blocked (default)",
    )
    parser.add_argument(
        "--air-lower-is-blocked",
        dest="air_higher_is_blocked",
        action="store_false",
        help="Treat IR value <= threshold as blocked",
    )
    parser.add_argument(
        "--ignore-ir-mask",
        action="store_true",
        default=True,
        help="Ignore IR valid-bit mask and always evaluate all 6 IR channels (default)",
    )
    parser.add_argument(
        "--respect-ir-mask",
        dest="ignore_ir_mask",
        action="store_false",
        help="Use IR valid-bit mask from ESP32 payload",
    )
    parser.add_argument(
        "--motion-epsilon",
        type=float,
        default=0.03,
        help="Minimum movement delta for PageUp/PageDown in motion mode",
    )
    parser.add_argument(
        "--read-timeout",
        type=float,
        default=0.02,
        help="ESP32 serial read timeout in seconds",
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Log verbosity",
    )
    return parser.parse_args()


def open_serial(port: str, baud: int, read_timeout: float) -> serial.Serial:
    return serial.Serial(
        port=port,
        baudrate=baud,
        timeout=read_timeout,
    )


def main() -> int:
    args = parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    if args.touch_single_threshold < 0 or args.touch_single_threshold > 65535:
        logging.error("--touch-single-threshold out of range")
        return 2

    if args.touch_double_threshold < args.touch_single_threshold:
        logging.error("--touch-double-threshold must be >= --touch-single-threshold")
        return 2

    try:
        air_thresholds = parse_air_thresholds(args.air_thresholds)
    except ValueError as exc:
        logging.error(str(exc))
        return 2

    try:
        esp32_serial = open_serial(args.esp32_port, args.esp32_baud, args.read_timeout)
    except serial.SerialException as exc:
        logging.error("Failed to open serial port: %s", exc)
        return 1

    bridge = OpenIthmKeyboardBridge(
        esp32_serial=esp32_serial,
        reverse_touch_order=args.reverse_touch_order,
        touch_single_threshold=args.touch_single_threshold,
        touch_double_threshold=args.touch_double_threshold,
        air_mode=args.air_mode,
        air_thresholds=air_thresholds,
        air_hysteresis=args.air_hysteresis,
        air_higher_is_blocked=args.air_higher_is_blocked,
        ignore_ir_mask=args.ignore_ir_mask,
        motion_epsilon=args.motion_epsilon,
    )

    logging.info(
        "Bridge started: ESP32=%s@%d -> keyboard mode (air=%s)",
        args.esp32_port,
        args.esp32_baud,
        args.air_mode,
    )
    logging.info("Slider bottom row: %s", " ".join(OPENITHM_BOTTOM_ROW))
    logging.info("Slider top row   : %s", " ".join(OPENITHM_TOP_ROW))
    logging.info("Touch order reversed: %s", args.reverse_touch_order)
    logging.info("Ignore IR valid mask: %s", args.ignore_ir_mask)
    if args.air_mode == "keys":
        logging.info("Air keys (bottom->top): %s", " ".join(OPENITHM_AIR_KEYS))
    else:
        logging.info("Air motion keys: home hold, end tap, page_up/page_down tap")

    bridge.start()
    try:
        bridge.wait()
    except KeyboardInterrupt:
        logging.info("Stopping bridge")
    finally:
        bridge.stop()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())