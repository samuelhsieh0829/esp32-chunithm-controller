#ifdef _WIN32

#define CHUNI_IO_EXPORTS
#include <windows.h>

#include <process.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "chuniio.h"

#define SENSOR_PACKET_SYNC 0xFF
#define SENSOR_PACKET_ESCAPE 0xFD
#define SENSOR_PACKET_CMD_REPORT 0x81

#define SENSOR_PACKET_VERSION 0x01
#define SENSOR_TOUCH_COUNT 16
#define SENSOR_IR_COUNT 6
#define SENSOR_SLIDER_CELLS 32

#define SENSOR_PACKET_PAYLOAD_SIZE \
	(4 + (SENSOR_TOUCH_COUNT * 2) + (SENSOR_IR_COUNT * 2))

#define SENSOR_PACKET_MAX_PAYLOAD 124
#define TOUCH_FIRMWARE_MAX 255
#define DEBUG_WINDOW_REFRESH_MS 50
#define SENSOR_LINK_BAUD_RATE_DEFAULT 2000000
#define SERIAL_READ_CHUNK_SIZE 256
#define SERIAL_AUTO_BAUD_SWITCH_MS 1000
#define TOUCH_BASELINE_ALPHA_SHIFT_DEFAULT 6
#define TOUCH_NOISE_ALPHA_SHIFT_DEFAULT 4
#define TOUCH_NOISE_SCALE_DEFAULT 6
#define TOUCH_DEBOUNCE_COUNT_DEFAULT 3

static const uint32_t s_serial_baud_candidates[] = {
	2000000,
	1500000,
	1000000,
	921600,
	460800,
	230400,
	115200,
};

struct chuni_io_config {
	uint8_t vk_test;
	uint8_t vk_service;
	uint8_t vk_coin;
	uint32_t com_port;
	uint32_t baud_rate;
	bool air_higher_is_blocked;
	uint16_t air_threshold[SENSOR_IR_COUNT];
	uint16_t air_hysteresis;
	uint16_t touch_deadzone;
	uint16_t touch_on_threshold;
	uint16_t touch_off_threshold;
	uint8_t touch_scale;
	bool touch_output_inverted;
	uint8_t touch_noise_scale;
	uint8_t touch_baseline_alpha_shift;
	uint8_t touch_noise_alpha_shift;
	uint8_t touch_debounce_count;
	uint32_t stale_timeout_ms;
	bool debug_window;
	bool auto_baud;
};

struct sensor_state {
	bool valid;
	uint8_t seq;
	uint8_t ir_valid_mask;
	uint16_t touch_raw[SENSOR_TOUCH_COUNT];
	uint16_t ir_raw[SENSOR_IR_COUNT];
	uint16_t touch_baseline[SENSOR_TOUCH_COUNT];
	uint16_t touch_noise[SENSOR_TOUCH_COUNT];
	bool touch_baseline_valid[SENSOR_TOUCH_COUNT];
	bool touch_active[SENSOR_TOUCH_COUNT];
	uint8_t touch_press_count[SENSOR_TOUCH_COUNT];
	uint8_t touch_release_count[SENSOR_TOUCH_COUNT];
	bool ir_blocked[SENSOR_IR_COUNT];
	uint64_t last_update_ms;
};

struct packet_parser {
	bool started;
	bool esc_pending;
	uint8_t checksum;
	uint8_t buf[128];
	size_t len;
	uint8_t size;
};

struct packet_frame {
	uint8_t cmd;
	uint8_t size;
	uint8_t payload[SENSOR_PACKET_MAX_PAYLOAD];
};

static unsigned int __stdcall chuni_io_serial_thread_proc(void *ctx);
static unsigned int __stdcall chuni_io_slider_thread_proc(void *ctx);
static unsigned int __stdcall chuni_io_debug_thread_proc(void *ctx);
static void chuni_io_ensure_debug_thread(void);

static void chuni_io_config_load(struct chuni_io_config *cfg, const wchar_t *filename);
static HANDLE chuni_io_open_serial_port(uint32_t com_port, uint32_t baud_rate);
static void chuni_io_close_serial_port_locked(void);
static void chuni_io_handle_sensor_report_locked(const uint8_t *payload, size_t size);
static uint16_t chuni_io_touch_abs_delta_u16(uint16_t a, uint16_t b);
static uint16_t chuni_io_touch_compute_on_threshold_locked(size_t index);
static uint16_t chuni_io_touch_compute_off_threshold_locked(size_t index);
static bool chuni_io_sensor_is_fresh_locked(uint64_t now_ms);
static uint8_t chuni_io_touch_to_pressure_locked(size_t index);
static bool chuni_io_ir_is_blocked_locked(size_t index);
static void packet_parser_reset(struct packet_parser *parser);
static bool packet_parser_push(
	struct packet_parser *parser,
	uint8_t byte,
	struct packet_frame *out);
static bool chuni_io_backend_init(void);

static CRITICAL_SECTION s_lock;
static bool s_lock_ready;
static bool s_backend_ready;

static struct chuni_io_config s_cfg;
static struct sensor_state s_sensor;

static HANDLE s_serial_handle = INVALID_HANDLE_VALUE;
static HANDLE s_serial_thread;
static volatile LONG s_serial_stop_flag;
static volatile LONG s_serial_active_baud;

static HANDLE s_slider_thread;
static volatile LONG s_slider_stop_flag;

static HANDLE s_debug_thread;
static volatile LONG s_debug_stop_flag;

static bool s_coin_pressed;
static uint16_t s_coin_count;

static void chuni_io_ensure_debug_thread(void)
{
	if (!s_cfg.debug_window) {
		return;
	}

	if (s_debug_thread != NULL) {
		DWORD wait = WaitForSingleObject(s_debug_thread, 0);
		if (wait == WAIT_TIMEOUT) {
			return;
		}

		CloseHandle(s_debug_thread);
		s_debug_thread = NULL;
	}

	InterlockedExchange(&s_debug_stop_flag, 0);
	s_debug_thread = (HANDLE) _beginthreadex(
		NULL,
		0,
		chuni_io_debug_thread_proc,
		NULL,
		0,
		NULL);
}

uint16_t chuni_io_get_api_version(void)
{
	return 0x0101;
}

HRESULT chuni_io_jvs_init(void)
{
	return chuni_io_backend_init() ? S_OK : E_FAIL;
}

void chuni_io_jvs_poll(uint8_t *opbtn, uint8_t *beams)
{
	if (opbtn == NULL || beams == NULL) {
		return;
	}

	if (!s_backend_ready && !chuni_io_backend_init()) {
		return;
	}

	*opbtn &= (uint8_t) ~0x03;
	*beams &= (uint8_t) ~0x3F;

	if (GetAsyncKeyState(s_cfg.vk_test) & 0x8000) {
		*opbtn |= 0x01;
	}

	if (GetAsyncKeyState(s_cfg.vk_service) & 0x8000) {
		*opbtn |= 0x02;
	}

	EnterCriticalSection(&s_lock);

	if (chuni_io_sensor_is_fresh_locked(GetTickCount64())) {
		for (size_t i = 0; i < SENSOR_IR_COUNT; i++) {
			const uint8_t bit = (uint8_t) (1u << i);

			if (chuni_io_ir_is_blocked_locked(i)) {
				*beams |= bit;
			}
		}
	}

	LeaveCriticalSection(&s_lock);
}

void chuni_io_jvs_read_coin_counter(uint16_t *total)
{
	if (total == NULL) {
		return;
	}

	if (!s_backend_ready && !chuni_io_backend_init()) {
		return;
	}

	if (GetAsyncKeyState(s_cfg.vk_coin) & 0x8000) {
		if (!s_coin_pressed) {
			s_coin_pressed = true;
			s_coin_count++;
		}
	} else {
		s_coin_pressed = false;
	}

	*total = s_coin_count;
}

void chuni_io_jvs_set_coin_blocker(bool open)
{
	(void) open;
}

HRESULT chuni_io_slider_init(void)
{
	return chuni_io_backend_init() ? S_OK : E_FAIL;
}

void chuni_io_slider_start(chuni_io_slider_callback_t callback)
{
	if (callback == NULL) {
		return;
	}

	if (!s_backend_ready && !chuni_io_backend_init()) {
		return;
	}

	if (s_slider_thread != NULL) {
		return;
	}

	InterlockedExchange(&s_slider_stop_flag, 0);

	s_slider_thread = (HANDLE) _beginthreadex(
		NULL,
		0,
		chuni_io_slider_thread_proc,
		callback,
		0,
		NULL);
}

void chuni_io_slider_stop(void)
{
	if (s_slider_thread == NULL) {
		return;
	}

	InterlockedExchange(&s_slider_stop_flag, 1);
	WaitForSingleObject(s_slider_thread, INFINITE);
	CloseHandle(s_slider_thread);
	s_slider_thread = NULL;
	InterlockedExchange(&s_slider_stop_flag, 0);
}

void chuni_io_slider_set_leds(const uint8_t *rgb)
{
	(void) rgb;
}

static bool chuni_io_backend_init(void)
{
	if (!s_lock_ready) {
		InitializeCriticalSection(&s_lock);
		s_lock_ready = true;
	}

	if (s_backend_ready) {
		chuni_io_ensure_debug_thread();
		return true;
	}

	memset(&s_sensor, 0, sizeof(s_sensor));
	chuni_io_config_load(&s_cfg, L".\\segatools.ini");

	InterlockedExchange(&s_serial_stop_flag, 0);
	s_serial_thread = (HANDLE) _beginthreadex(
		NULL,
		0,
		chuni_io_serial_thread_proc,
		NULL,
		0,
		NULL);

	if (s_serial_thread == NULL) {
		return false;
	}

	chuni_io_ensure_debug_thread();

	s_backend_ready = true;
	return true;
}

static unsigned int __stdcall chuni_io_serial_thread_proc(void *ctx)
{
	(void) ctx;

	struct packet_parser parser;
	packet_parser_reset(&parser);
	uint8_t rx_buf[SERIAL_READ_CHUNK_SIZE];
	uint64_t last_report_ms = GetTickCount64();
	size_t baud_index = 0;
	uint32_t open_baud = s_cfg.baud_rate;

	if (s_cfg.auto_baud) {
		for (size_t i = 0; i < _countof(s_serial_baud_candidates); i++) {
			if (s_serial_baud_candidates[i] == s_cfg.baud_rate) {
				baud_index = i;
				break;
			}
		}
		open_baud = s_serial_baud_candidates[baud_index];
	}

	while (InterlockedCompareExchange(&s_serial_stop_flag, 0, 0) == 0) {
		EnterCriticalSection(&s_lock);
		HANDLE port = s_serial_handle;
		LeaveCriticalSection(&s_lock);

		if (port == INVALID_HANDLE_VALUE) {
			HANDLE opened = chuni_io_open_serial_port(s_cfg.com_port, open_baud);
			if (opened == INVALID_HANDLE_VALUE) {
				Sleep(500);
				continue;
			}

			EnterCriticalSection(&s_lock);
			if (s_serial_handle == INVALID_HANDLE_VALUE) {
				s_serial_handle = opened;
				InterlockedExchange(&s_serial_active_baud, (LONG) open_baud);
				packet_parser_reset(&parser);
				last_report_ms = GetTickCount64();
			} else {
				CloseHandle(opened);
			}
			LeaveCriticalSection(&s_lock);
			continue;
		}

		DWORD bytes_read = 0;

		if (!ReadFile(port, rx_buf, sizeof(rx_buf), &bytes_read, NULL)) {
			EnterCriticalSection(&s_lock);
			chuni_io_close_serial_port_locked();
			LeaveCriticalSection(&s_lock);
			packet_parser_reset(&parser);
			Sleep(100);
			continue;
		}

		if (bytes_read == 0) {
			Sleep(0);
			continue;
		}

		for (DWORD i = 0; i < bytes_read; i++) {
			struct packet_frame frame;
			if (!packet_parser_push(&parser, rx_buf[i], &frame)) {
				continue;
			}

			if (frame.cmd == SENSOR_PACKET_CMD_REPORT) {
				EnterCriticalSection(&s_lock);
				chuni_io_handle_sensor_report_locked(frame.payload, frame.size);
				LeaveCriticalSection(&s_lock);
				last_report_ms = GetTickCount64();
			}
		}

		if (s_cfg.auto_baud && (GetTickCount64() - last_report_ms) > SERIAL_AUTO_BAUD_SWITCH_MS) {
			baud_index = (baud_index + 1u) % _countof(s_serial_baud_candidates);
			open_baud = s_serial_baud_candidates[baud_index];

			EnterCriticalSection(&s_lock);
			chuni_io_close_serial_port_locked();
			LeaveCriticalSection(&s_lock);

			packet_parser_reset(&parser);
			last_report_ms = GetTickCount64();
			Sleep(50);
		}
	}

	EnterCriticalSection(&s_lock);
	chuni_io_close_serial_port_locked();
	LeaveCriticalSection(&s_lock);

	return 0;
}

static unsigned int __stdcall chuni_io_slider_thread_proc(void *ctx)
{
	chuni_io_slider_callback_t callback = (chuni_io_slider_callback_t) ctx;
	uint8_t pressure[SENSOR_SLIDER_CELLS];
	uint8_t pad_pressure[SENSOR_TOUCH_COUNT];

	while (InterlockedCompareExchange(&s_slider_stop_flag, 0, 0) == 0) {
		memset(pressure, 0, sizeof(pressure));
		memset(pad_pressure, 0, sizeof(pad_pressure));

		EnterCriticalSection(&s_lock);

		if (chuni_io_sensor_is_fresh_locked(GetTickCount64())) {
			for (size_t i = 0; i < SENSOR_TOUCH_COUNT; i++) {
				pad_pressure[i] = chuni_io_touch_to_pressure_locked(i);
			}
		}

		LeaveCriticalSection(&s_lock);

		/* 16 sensors -> 32 slider cells, each sensor expanded to 2 cells */
		for (size_t i = 0; i < SENSOR_TOUCH_COUNT; i++) {
			const size_t base = i * 2;
			uint8_t value = pad_pressure[i];
			if (s_cfg.touch_output_inverted) {
				value = (uint8_t) (255u - value);
			}
			for (size_t j = 0; j < 2; j++) {
				pressure[base + j] = value;
			}
		}

		callback(pressure);
		Sleep(0);
	}

	return 0;
}

static unsigned int __stdcall chuni_io_debug_thread_proc(void *ctx)
{
	(void) ctx;

	FreeConsole();
	if (!AllocConsole()) {
		return 0;
	}

	SetConsoleTitleW(L"chuniio Sensor Debug");
	if (GetConsoleWindow() != NULL) {
		ShowWindow(GetConsoleWindow(), SW_SHOW);
	}

	FILE *stream = NULL;
	freopen_s(&stream, "CONOUT$", "w", stdout);
	freopen_s(&stream, "CONOUT$", "w", stderr);
	setvbuf(stdout, NULL, _IONBF, 0);

	while (InterlockedCompareExchange(&s_debug_stop_flag, 0, 0) == 0) {
		uint16_t raw[SENSOR_TOUCH_COUNT] = {0};
		uint8_t pad_out[SENSOR_TOUCH_COUNT] = {0};
		uint8_t cell_out[SENSOR_SLIDER_CELLS] = {0};
		bool fresh = false;
		uint8_t seq = 0;

		EnterCriticalSection(&s_lock);

		fresh = chuni_io_sensor_is_fresh_locked(GetTickCount64());
		seq = s_sensor.seq;
		memcpy(raw, s_sensor.touch_raw, sizeof(raw));

		if (fresh) {
			for (size_t i = 0; i < SENSOR_TOUCH_COUNT; i++) {
				uint8_t value = chuni_io_touch_to_pressure_locked(i);
				if (s_cfg.touch_output_inverted) {
					value = (uint8_t) (255u - value);
				}
				pad_out[i] = value;
			}

			for (size_t i = 0; i < SENSOR_TOUCH_COUNT; i++) {
				const size_t base = i * 2;
				for (size_t j = 0; j < 2; j++) {
					cell_out[base + j] = pad_out[i];
				}
			}
		}

		LeaveCriticalSection(&s_lock);

		HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
		if (out != INVALID_HANDLE_VALUE) {
			COORD origin = {0, 0};
			SetConsoleCursorPosition(out, origin);
		}

		printf("chuniio sensor debug window\n");
		printf("fresh=%s seq=%u output_inverted=%u\n",
			fresh ? "yes" : "no",
			(unsigned int) seq,
			s_cfg.touch_output_inverted ? 1u : 0u);
		printf("serial_active_baud=%u auto_baud=%u\n",
			(unsigned int) InterlockedCompareExchange(&s_serial_active_baud, 0, 0),
			s_cfg.auto_baud ? 1u : 0u);

		printf("touch_raw[16]        : ");
		for (size_t i = 0; i < SENSOR_TOUCH_COUNT; i++) {
			printf("%5u ", (unsigned int) raw[i]);
		}
		printf("\n");

		printf("touch_to_game_pad[16]: ");
		for (size_t i = 0; i < SENSOR_TOUCH_COUNT; i++) {
			printf("%3u ", (unsigned int) pad_out[i]);
		}
		printf("\n");

		for (size_t row = 0; row < 4; row++) {
			const size_t offset = row * 8;
			printf("slider_cell[%02u-%02u]  : ",
				(unsigned int) offset,
				(unsigned int) (offset + 7));
			for (size_t col = 0; col < 8; col++) {
				printf("%3u ", (unsigned int) cell_out[offset + col]);
			}
			printf("\n");
		}

		printf("\n(Refresh every %u ms)\n", (unsigned int) DEBUG_WINDOW_REFRESH_MS);
		Sleep(DEBUG_WINDOW_REFRESH_MS);
	}

	return 0;
}

// 從segatools.ini讀取設定
static void chuni_io_config_load(struct chuni_io_config *cfg, const wchar_t *filename)
{
	wchar_t key[32];

	memset(cfg, 0, sizeof(*cfg));

	cfg->vk_test = (uint8_t) GetPrivateProfileIntW(L"io3", L"test", VK_F1, filename);
	cfg->vk_service = (uint8_t) GetPrivateProfileIntW(L"io3", L"service", VK_F2, filename);
	cfg->vk_coin = (uint8_t) GetPrivateProfileIntW(L"io3", L"coin", VK_F3, filename);

	cfg->com_port = (uint32_t) GetPrivateProfileIntW(L"chuniio", L"com_port", 9, filename);
	cfg->baud_rate = (uint32_t) GetPrivateProfileIntW(
		L"chuniio",
		L"baud_rate",
		SENSOR_LINK_BAUD_RATE_DEFAULT,
		filename);

	cfg->touch_deadzone = (uint16_t) GetPrivateProfileIntW(
		L"chuniio",
		L"touch_deadzone",
		24,
		filename);
	cfg->touch_on_threshold = (uint16_t) GetPrivateProfileIntW(
		L"chuniio",
		L"touch_on_threshold",
		90,
		filename);
	cfg->touch_off_threshold = (uint16_t) GetPrivateProfileIntW(
		L"chuniio",
		L"touch_off_threshold",
		55,
		filename);
	cfg->touch_scale = (uint8_t) GetPrivateProfileIntW(
		L"chuniio",
		L"touch_scale",
		8,
		filename);
	cfg->touch_output_inverted = GetPrivateProfileIntW(
		L"chuniio",
		L"touch_output_inverted",
		0,
		filename) != 0;
	cfg->touch_noise_scale = (uint8_t) GetPrivateProfileIntW(
		L"chuniio",
		L"touch_noise_scale",
		TOUCH_NOISE_SCALE_DEFAULT,
		filename);
	cfg->touch_baseline_alpha_shift = (uint8_t) GetPrivateProfileIntW(
		L"chuniio",
		L"touch_baseline_alpha_shift",
		TOUCH_BASELINE_ALPHA_SHIFT_DEFAULT,
		filename);
	cfg->touch_noise_alpha_shift = (uint8_t) GetPrivateProfileIntW(
		L"chuniio",
		L"touch_noise_alpha_shift",
		TOUCH_NOISE_ALPHA_SHIFT_DEFAULT,
		filename);
	cfg->touch_debounce_count = (uint8_t) GetPrivateProfileIntW(
		L"chuniio",
		L"touch_debounce_count",
		TOUCH_DEBOUNCE_COUNT_DEFAULT,
		filename);
	cfg->stale_timeout_ms = (uint32_t) GetPrivateProfileIntW(
		L"chuniio",
		L"stale_timeout_ms",
		250,
		filename);
	cfg->debug_window = GetPrivateProfileIntW(
		L"chuniio",
		L"debug_window",
		1,
		filename) != 0;
	cfg->auto_baud = GetPrivateProfileIntW(
		L"chuniio",
		L"auto_baud",
		1,
		filename) != 0;

	cfg->air_higher_is_blocked = GetPrivateProfileIntW(
		L"chuniio",
		L"air_higher_is_blocked",
		1,
		filename) != 0;
	cfg->air_hysteresis = (uint16_t) GetPrivateProfileIntW(
		L"chuniio",
		L"air_hysteresis",
		80,
		filename);

	const int air_threshold_default = GetPrivateProfileIntW(
		L"chuniio",
		L"air_threshold",
		1800,
		filename);

	for (size_t i = 0; i < SENSOR_IR_COUNT; i++) {
		swprintf_s(key, _countof(key), L"air%u_threshold", (unsigned int) (i + 1));
		cfg->air_threshold[i] = (uint16_t) GetPrivateProfileIntW(
			L"chuniio",
			key,
			air_threshold_default,
			filename);
	}

	if (cfg->touch_off_threshold > cfg->touch_on_threshold) {
		cfg->touch_off_threshold = cfg->touch_on_threshold;
	}

	if (cfg->touch_debounce_count == 0) {
		cfg->touch_debounce_count = 1;
	}

	if (cfg->touch_baseline_alpha_shift > 10) {
		cfg->touch_baseline_alpha_shift = 10;
	}

	if (cfg->touch_noise_alpha_shift > 10) {
		cfg->touch_noise_alpha_shift = 10;
	}
}

static HANDLE chuni_io_open_serial_port(uint32_t com_port, uint32_t baud_rate)
{
	wchar_t path[32];
	swprintf_s(path, _countof(path), L"\\\\.\\COM%u", (unsigned int) com_port);

	HANDLE handle = CreateFileW(
		path,
		GENERIC_READ | GENERIC_WRITE,
		0,
		NULL,
		OPEN_EXISTING,
		0,
		NULL);

	if (handle == INVALID_HANDLE_VALUE) {
		return INVALID_HANDLE_VALUE;
	}

	DCB dcb;
	memset(&dcb, 0, sizeof(dcb));
	dcb.DCBlength = sizeof(dcb);

	if (!GetCommState(handle, &dcb)) {
		CloseHandle(handle);
		return INVALID_HANDLE_VALUE;
	}

	dcb.BaudRate = baud_rate;
	dcb.ByteSize = 8;
	dcb.StopBits = ONESTOPBIT;
	dcb.Parity = NOPARITY;

	if (!SetCommState(handle, &dcb)) {
		CloseHandle(handle);
		return INVALID_HANDLE_VALUE;
	}

	COMMTIMEOUTS timeouts;
	memset(&timeouts, 0, sizeof(timeouts));
	timeouts.ReadIntervalTimeout = MAXDWORD;
	timeouts.ReadTotalTimeoutConstant = 0;
	timeouts.ReadTotalTimeoutMultiplier = 0;
	timeouts.WriteTotalTimeoutConstant = 0;
	timeouts.WriteTotalTimeoutMultiplier = 0;

	if (!SetCommTimeouts(handle, &timeouts)) {
		CloseHandle(handle);
		return INVALID_HANDLE_VALUE;
	}

	PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

	return handle;
}

static void chuni_io_close_serial_port_locked(void)
{
	if (s_serial_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(s_serial_handle);
		s_serial_handle = INVALID_HANDLE_VALUE;
	}
}

static uint16_t chuni_io_touch_abs_delta_u16(uint16_t a, uint16_t b)
{
	return a >= b ? (uint16_t) (a - b) : (uint16_t) (b - a);
}

static uint16_t chuni_io_touch_compute_on_threshold_locked(size_t index)
{
	uint32_t threshold = s_cfg.touch_on_threshold
		+ ((uint32_t) s_sensor.touch_noise[index] * s_cfg.touch_noise_scale);

	if (threshold > 0xFFFFu) {
		threshold = 0xFFFFu;
	}

	return (uint16_t) threshold;
}

static uint16_t chuni_io_touch_compute_off_threshold_locked(size_t index)
{
	uint32_t threshold = s_cfg.touch_off_threshold
		+ (((uint32_t) s_sensor.touch_noise[index] * s_cfg.touch_noise_scale) / 2u);

	if (threshold > 0xFFFFu) {
		threshold = 0xFFFFu;
	}

	return (uint16_t) threshold;
}

static void chuni_io_handle_sensor_report_locked(const uint8_t *payload, size_t size)
{
	if (size != SENSOR_PACKET_PAYLOAD_SIZE || payload[0] != SENSOR_PACKET_VERSION) {
		return;
	}

	s_sensor.valid = true;
	s_sensor.seq = payload[1];
	s_sensor.ir_valid_mask = payload[2];

	size_t cursor = 4;

	for (size_t i = 0; i < SENSOR_TOUCH_COUNT; i++) {
		const uint16_t raw = (uint16_t) payload[cursor] | ((uint16_t) payload[cursor + 1] << 8);
		cursor += 2;

		s_sensor.touch_raw[i] = raw;

		if (!s_sensor.touch_baseline_valid[i]) {
			s_sensor.touch_baseline_valid[i] = true;
			s_sensor.touch_baseline[i] = raw;
			s_sensor.touch_noise[i] = 0;
			s_sensor.touch_press_count[i] = 0;
			s_sensor.touch_release_count[i] = 0;
			s_sensor.touch_active[i] = false;
			continue;
		}

		const uint16_t delta = chuni_io_touch_abs_delta_u16(raw, s_sensor.touch_baseline[i]);
		const uint16_t on_threshold = chuni_io_touch_compute_on_threshold_locked(i);
		const uint16_t off_threshold = chuni_io_touch_compute_off_threshold_locked(i);

		if (!s_sensor.touch_active[i]) {
			if (delta >= on_threshold) {
				if (s_sensor.touch_press_count[i] < UINT8_MAX) {
					s_sensor.touch_press_count[i]++;
				}

				if (s_sensor.touch_press_count[i] >= s_cfg.touch_debounce_count) {
					s_sensor.touch_active[i] = true;
					s_sensor.touch_press_count[i] = 0;
					s_sensor.touch_release_count[i] = 0;
				}
			} else {
				s_sensor.touch_press_count[i] = 0;
			}
		} else {
			if (delta <= off_threshold) {
				if (s_sensor.touch_release_count[i] < UINT8_MAX) {
					s_sensor.touch_release_count[i]++;
				}

				if (s_sensor.touch_release_count[i] >= s_cfg.touch_debounce_count) {
					s_sensor.touch_active[i] = false;
					s_sensor.touch_release_count[i] = 0;
					s_sensor.touch_press_count[i] = 0;
				}
			} else {
				s_sensor.touch_release_count[i] = 0;
			}
		}

		if (!s_sensor.touch_active[i]) {
			int32_t baseline = s_sensor.touch_baseline[i];
			baseline += ((int32_t) raw - baseline) >> s_cfg.touch_baseline_alpha_shift;
			if (baseline < 0) {
				baseline = 0;
			} else if (baseline > 0xFFFF) {
				baseline = 0xFFFF;
			}
			s_sensor.touch_baseline[i] = (uint16_t) baseline;

			int32_t noise = s_sensor.touch_noise[i];
			noise += ((int32_t) delta - noise) >> s_cfg.touch_noise_alpha_shift;
			if (noise < 0) {
				noise = 0;
			} else if (noise > 0xFFFF) {
				noise = 0xFFFF;
			}
			s_sensor.touch_noise[i] = (uint16_t) noise;
		}
	}

	for (size_t i = 0; i < SENSOR_IR_COUNT; i++) {
		s_sensor.ir_raw[i] =
			(uint16_t) payload[cursor] | ((uint16_t) payload[cursor + 1] << 8);
		cursor += 2;
	}

	s_sensor.last_update_ms = GetTickCount64();
}

static bool chuni_io_sensor_is_fresh_locked(uint64_t now_ms)
{
	if (!s_sensor.valid) {
		return false;
	}

	return now_ms - s_sensor.last_update_ms <= s_cfg.stale_timeout_ms;
}

static uint8_t chuni_io_touch_to_pressure_locked(size_t index)
{
	if (index >= SENSOR_TOUCH_COUNT) {
		return 0;
	}

	uint16_t raw = s_sensor.touch_raw[index];
	if (raw > TOUCH_FIRMWARE_MAX) {
		raw = TOUCH_FIRMWARE_MAX;
	}

	return (uint8_t) raw;
}

static bool chuni_io_ir_is_blocked_locked(size_t index)
{
	if (index >= SENSOR_IR_COUNT) {
		return false;
	}

	const uint8_t bit = (uint8_t) (1u << index);
	if ((s_sensor.ir_valid_mask & bit) == 0) {
		s_sensor.ir_blocked[index] = false;
		return false;
	}

	const uint16_t value = s_sensor.ir_raw[index];
	const uint16_t threshold = s_cfg.air_threshold[index];
	const uint16_t h = s_cfg.air_hysteresis;

	const uint16_t upper = (uint16_t) (((uint32_t) threshold + h) > 0xFFFFu
		? 0xFFFFu
		: (uint32_t) threshold + h);
	const uint16_t lower = threshold > h ? (uint16_t) (threshold - h) : 0;

	bool blocked = s_sensor.ir_blocked[index];

	if (s_cfg.air_higher_is_blocked) {
		if (!blocked) {
			blocked = value >= upper;
		} else {
			blocked = value >= lower;
		}
	} else {
		if (!blocked) {
			blocked = value <= lower;
		} else {
			blocked = value <= upper;
		}
	}

	s_sensor.ir_blocked[index] = blocked;
	return blocked;
}

static void packet_parser_reset(struct packet_parser *parser)
{
	parser->started = false;
	parser->esc_pending = false;
	parser->checksum = 0;
	parser->len = 0;
	parser->size = 0;
}

static bool packet_parser_push(
	struct packet_parser *parser,
	uint8_t byte,
	struct packet_frame *out)
{
	if (byte == SENSOR_PACKET_SYNC) {
		packet_parser_reset(parser);
		parser->started = true;
		parser->buf[0] = SENSOR_PACKET_SYNC;
		parser->len = 1;
		parser->checksum = SENSOR_PACKET_SYNC;
		return false;
	}

	if (!parser->started) {
		return false;
	}

	if (byte == SENSOR_PACKET_ESCAPE) {
		parser->esc_pending = true;
		return false;
	}

	if (parser->esc_pending) {
		parser->esc_pending = false;
		byte++;
	}

	if (parser->len >= _countof(parser->buf)) {
		packet_parser_reset(parser);
		return false;
	}

	parser->buf[parser->len++] = byte;
	parser->checksum += byte;

	if (parser->len == 3) {
		parser->size = parser->buf[2];
		if (parser->size > SENSOR_PACKET_MAX_PAYLOAD) {
			packet_parser_reset(parser);
			return false;
		}
	}

	if (parser->len < 4) {
		return false;
	}

	const size_t frame_len = (size_t) parser->size + 4;
	if (parser->len < frame_len) {
		return false;
	}

	if (parser->len != frame_len || parser->checksum != 0) {
		packet_parser_reset(parser);
		return false;
	}

	out->cmd = parser->buf[1];
	out->size = parser->size;
	if (out->size > 0) {
		memcpy(out->payload, &parser->buf[3], out->size);
	}

	packet_parser_reset(parser);
	return true;
}

#else

#include "chuniio.h"

uint16_t chuni_io_get_api_version(void)
{
	return 0x0101;
}

HRESULT chuni_io_jvs_init(void)
{
	return 0;
}

void chuni_io_jvs_poll(uint8_t *opbtn, uint8_t *beams)
{
	(void) opbtn;
	(void) beams;
}

void chuni_io_jvs_read_coin_counter(uint16_t *total)
{
	if (total != NULL) {
		*total = 0;
	}
}

void chuni_io_jvs_set_coin_blocker(bool open)
{
	(void) open;
}

HRESULT chuni_io_slider_init(void)
{
	return 0;
}

void chuni_io_slider_start(chuni_io_slider_callback_t callback)
{
	(void) callback;
}

void chuni_io_slider_stop(void)
{
}

void chuni_io_slider_set_leds(const uint8_t *rgb)
{
	(void) rgb;
}

#endif
