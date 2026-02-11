#ifndef SD_CARD_TESTBENCH_H
#define SD_CARD_TESTBENCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "xprintf.h"
#include "ff.h"
#include "hx_drv_gpio.h"
#include "hx_drv_scu.h"
#include <string.h> // For strncpy, strlen, etc.

// Placeholder values - These should ideally come from your Python-generated model_test_info.h
// Make sure to include the actual generated file in your build system.
#ifndef NUM_TEST_SAMPLES
#define NUM_TEST_SAMPLES 1000000 // Example: Assume 100 total test samples
#endif

#ifndef MODEL_INPUT_TIMESTEPS
#define MODEL_INPUT_TIMESTEPS 40 // Example: Timesteps per sample
#endif

#ifndef MODEL_INPUT_FEATURES
#define MODEL_INPUT_FEATURES 1   // Example: Features per timestep
#endif

// Define max path length for file names
#define MAX_PATH_LEN 64
// Define buffer size for a single X test vector (float32)
#define X_TEST_VECTOR_SIZE (MODEL_INPUT_TIMESTEPS * MODEL_INPUT_FEATURES * sizeof(float))
// Define buffer size for a single Y test vector (float32) - assuming a single float output label
#define Y_TEST_VECTOR_SIZE (1 * sizeof(float))


/**
 * @brief Struct to hold a single test sample (input and ground truth).
 */
typedef struct {
    float x_data[MODEL_INPUT_TIMESTEPS * MODEL_INPUT_FEATURES]; // Input features for one sample
    float y_data; // Ground truth label for one sample (assuming single float)
    uint32_t x_data_size; // Size in bytes of x_data
    uint32_t y_data_size; // Size in bytes of y_data
} test_sample_t;


/**
 * @brief Initializes the SD card and FatFs filesystem.
 *
 * This function mounts the SD card and sets up necessary GPIO pinmuxes
 * for SPI communication.
 *
 * @return FRESULT FatFs result code (FR_OK if successful).
 */
FRESULT sd_card_init(const char *x_test_folder, const char *y_test_folder);

/**
 * @brief Loads a test vector (X) and its corresponding ground truth (Y) from the SD card.
 *
 * This function attempts to load test vectors starting from the given index.
 * If a file pair (x_test_row_N.bin and y_test_row_N.bin) is not found, it
 * increments the index and tries the next pair until a valid pair is found
 * or all samples have been attempted.
 *
 * @param start_index The starting index to attempt loading from.
 * @param sample_data Pointer to a test_sample_t structure to store the loaded data.
 * @param actual_index_loaded Pointer to a uint32_t to store the actual index of the loaded sample.
 * @return FRESULT FR_OK if a test vector pair was successfully loaded,
 * FR_NO_FILE if no more valid test vectors could be found,
 * or other FatFs error codes.
 */
FRESULT load_next_test_vector(uint32_t start_index, test_sample_t *sample_data, uint32_t *actual_index_loaded);
FRESULT save_result_vector(uint32_t index, int8_t *model_output, uint32_t output_length, const char *file_prefix);
FRESULT save_result_vector_bulk(uint32_t index, int8_t *model_output, uint32_t output_length, const char *file_prefix);

#ifdef __cplusplus
}
#endif

#endif // SD_CARD_TESTBENCH_H

