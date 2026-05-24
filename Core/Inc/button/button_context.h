#ifndef BUTTON_CONTEXT_H
#define BUTTON_CONTEXT_H

#include "main.h"
#include "bsp/bsp_key.h"
#include "lcd/display.h"
#include "wifi/esp8266.h"
#include "wifi/wifi_config.h"
#include "audio/i2s_mic.h"
#include "debug/debug_uart.h"
#include "lcd/lcd.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum
{
    STATE_IDLE = 0,
    STATE_RECORDING,
    STATE_REC_PAUSED,
    STATE_WAITING,
    STATE_HISTORY
} sys_state_t;

typedef enum
{
    KEY_EVENT_NONE = 0,
    KEY_EVENT_K1_PRESS,
    KEY_EVENT_K1_RELEASE,
    KEY_EVENT_K2_SHORT,
    KEY_EVENT_K2_LONG,
    KEY_EVENT_K2_DOUBLE
} key_event_t;

typedef enum
{
    MIC_PROBE_IDLE = 0,
    MIC_PROBE_RUNNING
} mic_probe_state_t;

typedef enum
{
    MIC_REC_TEST_IDLE = 0,
    MIC_REC_TEST_RUNNING
} mic_rec_test_state_t;

#define K2_LONG_PRESS_MS 800u
#define K2_DOUBLE_CLICK_MS 300u

#define AUDIO_UPLOAD_BUFFER_SAMPLES 16000u
#define AUDIO_UPLOAD_CHUNK_SAMPLES 1024u
#define AUDIO_UPLOAD_TRIGGER_SAMPLES AUDIO_UPLOAD_CHUNK_SAMPLES
#define AUDIO_UPLOAD_DOWNSAMPLE 4u
#define AUDIO_UPLOAD_GAIN_Q8 512

#define WIFI_COLOR_CONNECTED COLOR_ACCENT_GREEN
#define WIFI_COLOR_DISCONNECTED COLOR_ICON_MIC

#define MIC_PROBE_TIMEOUT_MS 250u
#define MIC_PROBE_MIN_NONZERO_SAMPLES 8u
#define MIC_PROBE_MIN_TOTAL_SAMPLES 64u

extern sys_state_t sys_state;
extern uint32_t k2_press_tick;
extern uint8_t k2_press_count;
extern uint32_t k2_last_release_tick;
extern uint8_t k2_was_pressed;
extern uint32_t rec_start_tick;
extern uint8_t k1_no_tcp_hint_latched;
extern int16_t audio_upload_buffer[AUDIO_UPLOAD_BUFFER_SAMPLES];
extern volatile uint16_t audio_upload_write_pos;
extern volatile uint16_t audio_upload_read_pos;
extern volatile uint8_t audio_upload_overflow;
extern volatile uint32_t audio_upload_dropped_samples;
extern int32_t audio_upload_downsample_acc;
extern uint16_t audio_upload_downsample_count;
extern volatile uint8_t rec_end_pending;
extern volatile uint8_t mic_rec_done_pending;
extern mic_probe_state_t mic_probe_state;
extern uint32_t mic_probe_start_tick;
extern volatile uint16_t mic_probe_nonzero_count;
extern volatile uint16_t mic_probe_sample_count;
extern volatile uint8_t mic_probe_collecting;
extern mic_rec_test_state_t mic_rec_test_state;
extern uint32_t mic_rec_test_start_tick;
extern uint32_t mic_rec_test_duration_ms;
extern uint8_t speaker_test_ok_pending;
extern uint8_t speaker_tone_done_pending;
extern uint8_t speaker_sweep_done_pending;
extern uint8_t speaker_volume_done_pending;

uint8_t tcp_ready_for_send(void);
uint8_t tcp_idle_for_heartbeat(void);
void audio_buffer_reset(void);
uint16_t audio_buffer_available(void);
int16_t audio_upload_apply_gain(int16_t sample);
void on_audio_data(const int16_t *buf, uint16_t len);
void audio_upload_poll(void);
void try_send_rec_end(void);
void try_send_mic_rec_done(void);
void try_send_speaker_test_ok(void);
void try_send_speaker_tone_done(void);
void try_send_speaker_sweep_done(void);
void try_send_speaker_volume_done(void);
void pump_speaker_test_response(uint32_t timeout_ms);
void mic_probe_start(void);
void mic_probe_poll(void);
void mic_rec_test_start(uint32_t duration_sec);
void mic_rec_test_poll(void);
void handle_tcp_downlink(void);
key_event_t detect_k2_event(void);
void handle_idle(key_event_t k2_ev);
void handle_recording(void);
void handle_rec_paused(key_event_t k2_ev);
void handle_history(key_event_t k2_ev);

#endif