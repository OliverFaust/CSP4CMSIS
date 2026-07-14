#ifndef KWS_PDM_RECORD_H
#define KWS_PDM_RECORD_H

#include <stdint.h>
#include <stdbool.h>
#include "spi_protocol.h"

#define APP_BLOCK_FUNC() do { \
    __asm volatile("b    ."); \
} while (0)

/*******************************************************************************
 * audio data is a 16-bits data (2bytes)
 * for 16Khz of 1 seconds mono audio, takes 2*16*1000 bytes
 * create a block of two chained 8000-byte LLI segments (BLK_NUM=2) to save
 * 0.5 second of mono audio data per ring slot (each segment individually
 * meets hx_drv_pdm_dma_lli_transfer's <8192-byte limit; BLK_NUM chains them)
 * create 8 blocks to save 4 seconds of data (8 x 0.5s)
 ******************************************************************************/
#define QUARTER_SECOND_MONO_BYTES   8000    // 0.25 sec; size of ONE chained LLI segment (stays under the <8192-byte hx_drv_pdm_dma_lli_transfer per-segment limit regardless of BLK_NUM)
#define BLK_NUM                     2       // 0.5 sec: TWO 8000-byte LLI segments chained per ring slot -- the DMA hardware assembles the pair, not software. See the 0.5s reference implementation (pre-CSP4CMSIS) this was restored from.
#define AUDIO_LEN                   16000   // model input size stays fixed at 1 s
#define NUM_BUFF                    8       // 8 x 0.5s = 4s of ring buffer (still plenty)

#ifdef __cplusplus
extern "C" {
#endif

int kws_pdm_record_app(void);

/* Shared PDM audio state: filled by kws_pdm_record_app()'s DMA callback,
 * consumed by the KWS processing task now living in csp4cmsis_spn.cpp. */
extern int16_t audio_buf[NUM_BUFF][BLK_NUM*QUARTER_SECOND_MONO_BYTES/2];
extern volatile int32_t w_buf_idx;
extern volatile int32_t r_buf_idx;
extern volatile bool kws_processing_complete;
extern struct_kws_algoResult algoresult_kws_pdm_record;

/* Ground-truth measurement of the real DMA completion callback interval --
 * see app_pdm_dma_rx_cb() in csp4cmsis_kws_iic.c. */
extern volatile uint32_t g_dma_cb_count;
extern volatile uint32_t g_dma_cb_interval_accum_ms;
extern volatile uint32_t g_dma_cb_interval_count;

void kws_processing_callback(void);

#ifdef __cplusplus
}
#endif

#endif /* KWS_PDM_RECORD_H_ */