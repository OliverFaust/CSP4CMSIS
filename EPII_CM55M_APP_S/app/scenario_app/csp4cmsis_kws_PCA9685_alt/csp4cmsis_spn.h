#ifndef APP_CSP4CMSIS_ALLON_SENSOR_TFLM_SPN
#define APP_CSP4CMSIS_ALLON_SENSOR_TFLM_SPN

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void RunProcessingChainTest(void);

void kws_report_detection(const char *label, int score_pct, uint32_t label_idx);
void kws_report_none(void);
void kws_report_missed_inference(int32_t prev_buf, int32_t a);

// Reported by PreprocessingProcess for each of the first 3 steps after
// (re)initialisation, during which cv_kws_preprocess_step() returns nullptr
// because the model's 98-frame window isn't yet fully populated with real
// audio. Replaces the old kws_report_skip_first, which was specific to the
// single-skip behaviour of the previous whole-window-assembly design.
void kws_report_priming(int32_t step);

// Reported by AcquisitionProcess once per batch (every 20 quarter-buffers).
// chan_send_wait_ms_accum is time spent blocked handing a chunk to
// PreprocessingProcess -- the pipeline-lag signal for that boundary.
void kws_report_acq_stats(int32_t missed, int32_t total,
                           uint32_t dma_wait_ms_accum,
                           uint32_t buf_asm_ms_accum,
                           uint32_t chan_send_wait_ms_accum,
                           uint32_t elapsed_ms, float realtime_factor);

// Reported by PreprocessingProcess once per batch (every 20 quarter-buffers
// received, including the 3 priming ones). chan_send_wait_ms_accum is time
// spent blocked handing a completed tensor to InferenceProcess.
void kws_report_prep_stats(int32_t processed,
                            uint32_t mfcc_ms_accum,
                            uint32_t chan_send_wait_ms_accum,
                            uint32_t elapsed_ms, float realtime_factor);

// Reported by InferenceProcess once per batch (every 20 tensors processed).
void kws_report_infer_stats(int32_t processed,
                             uint32_t copy_ms_accum,
                             uint32_t invoke_ms_accum,
                             uint32_t postproc_ms_accum,
                             uint32_t elapsed_ms, float realtime_factor);

#ifdef __cplusplus
}
#endif

#endif
