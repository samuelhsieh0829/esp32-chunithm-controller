#pragma once

#ifndef _HRESULT_DEFINED
typedef long HRESULT;
#endif

#include <stdbool.h>
#include <stdint.h>

/*
 * CHUNITHM custom IO API for segatools.
 * This DLL implementation consumes sensor packets from an ESP32 over COM.
 */

#if defined(_WIN32)
#if defined(CHUNI_IO_EXPORTS)
#define CHUNI_IO_API __declspec(dllexport)
#else
#define CHUNI_IO_API __declspec(dllimport)
#endif
#else
#define CHUNI_IO_API
#endif

typedef void (*chuni_io_slider_callback_t)(const uint8_t *state);

CHUNI_IO_API uint16_t chuni_io_get_api_version(void);

CHUNI_IO_API HRESULT chuni_io_jvs_init(void);
CHUNI_IO_API void chuni_io_jvs_poll(uint8_t *opbtn, uint8_t *beams);
CHUNI_IO_API void chuni_io_jvs_read_coin_counter(uint16_t *total);
CHUNI_IO_API void chuni_io_jvs_set_coin_blocker(bool open);

CHUNI_IO_API HRESULT chuni_io_slider_init(void);
CHUNI_IO_API void chuni_io_slider_start(chuni_io_slider_callback_t callback);
CHUNI_IO_API void chuni_io_slider_stop(void);
CHUNI_IO_API void chuni_io_slider_set_leds(const uint8_t *rgb);
