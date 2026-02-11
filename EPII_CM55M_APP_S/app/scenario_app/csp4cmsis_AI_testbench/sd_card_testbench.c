#include "sd_card_testbench.h"
#include <math.h> // Corrected: Using C math header for roundf()
#include <string.h> // Required for strcpy
#include "WE2_debug.h"

// Directory definitions
#define DRV ""
#define BULK_RESULT_COUNT 256

static FATFS fs; // Filesystem object

// Global variables to hold the directory paths
static char g_x_test_folder[MAX_PATH_LEN];
static char g_y_test_folder[MAX_PATH_LEN];

// Static variables for bulk writing
static int8_t bulk_results[BULK_RESULT_COUNT];
static uint32_t bulk_results_count = 0;

// Forward declarations for internal helper functions (optional, but good practice)
static FRESULT read_binary_file(const char *filepath, void *buffer, uint32_t size, uint32_t *bytes_read);


/**
 * @brief Initializes the SD card and FatFs filesystem.
 *
 * This function mounts the SD card, sets up necessary GPIO pinmuxes
 * for SPI communication, and initializes the test vector directories.
 *
 * @param x_test_folder The directory for input test vectors.
 * @param y_test_folder The directory for ground truth labels.
 * @return FRESULT FatFs result code (FR_OK if successful).
 */
FRESULT sd_card_init(const char *x_test_folder, const char *y_test_folder)
{
    FRESULT res;

    // Copy the folder names into the global variables
    strncpy(g_x_test_folder, x_test_folder, MAX_PATH_LEN - 1);
    g_x_test_folder[MAX_PATH_LEN - 1] = '\0'; // Ensure null-termination
    strncpy(g_y_test_folder, y_test_folder, MAX_PATH_LEN - 1);
    g_y_test_folder[MAX_PATH_LEN - 1] = '\0'; // Ensure null-termination


    // Configure GPIO pinmuxes for SPI communication with SD card
    // Assuming these pins are correct for your board
    hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_SPI_M_DO_1, 1);   // SPI MOSI
    hx_drv_scu_set_PB3_pinmux(SCU_PB3_PINMUX_SPI_M_DI_1, 1);   // SPI MISO
    hx_drv_scu_set_PB4_pinmux(SCU_PB4_PINMUX_SPI_M_SCLK_1, 1); // SPI Clock
    hx_drv_scu_set_PB5_pinmux(SCU_PB5_PINMUX_SPI_M_CS_1, 1);   // SPI Chip Select (Software controlled)

    dbg_printf(DBG_MORE_INFO, "Attempting to mount SD card...\r\n");

    // Mount the FatFs filesystem
    res = f_mount(&fs, DRV, 1); // 1 means immediate mount
    if (res != FR_OK)
    {
        dbg_printf(DBG_MORE_INFO, "f_mount failed with error: %d\r\n", res);
        return res;
    }
    dbg_printf(DBG_MORE_INFO, "SD card mounted successfully.\r\n");

    // Optional: Change to root directory if not already there,
    // or just assume paths are absolute from root for g_x_test_folder/g_y_test_folder
    // For this design, we'll assume g_x_test_folder and g_y_test_folder are relative to root,
    // or are absolute paths if defined as such.

    // Check if x_test and y_test directories exist, create if not (though Python script should handle this)
    // For read-only testbench, we might not need to create them, but useful for robustness.
    FILINFO fno;
    res = f_stat(g_x_test_folder, &fno);
    if (res != FR_OK) {
        dbg_printf(DBG_MORE_INFO, "Directory '%s' not found. Creating...\r\n", g_x_test_folder);
        res = f_mkdir(g_x_test_folder);
        if (res != FR_OK) {
            dbg_printf(DBG_MORE_INFO, "Failed to create directory '%s': %d\r\n", g_x_test_folder, res);
            return res;
        }
    }
    res = f_stat(g_y_test_folder, &fno);
    if (res != FR_OK) {
        dbg_printf(DBG_MORE_INFO, "Directory '%s' not found. Creating...\r\r\n", g_y_test_folder);
        res = f_mkdir(g_y_test_folder);
        if (res != FR_OK) {
            dbg_printf(DBG_MORE_INFO, "Failed to create directory '%s': %d\r\n", g_y_test_folder, res);
            return res;
        }
    }

    return FR_OK;
}


/**
 * @brief Helper function to read data from a binary file.
 *
 * @param filepath The path to the binary file.
 * @param buffer Pointer to the buffer to store the read data.
 * @param size The number of bytes to read.
 * @param bytes_read Pointer to a UINT to store the actual number of bytes read.
 * @return FRESULT FatFs result code.
 */
static FRESULT read_binary_file(const char *filepath, void *buffer, uint32_t size, uint32_t *bytes_read)
{
    FIL fil;
    FRESULT res;
    UINT br;

    res = f_open(&fil, filepath, FA_READ);
    if (res != FR_OK)
    {
        dbg_printf(DBG_MORE_INFO, "Failed to open file '%s' for reading. Error: %d\r\n", filepath, res);
        return res; // Return error if file cannot be opened
    }

    res = f_read(&fil, buffer, size, &br);
    if (res != FR_OK)
    {
        dbg_printf(DBG_MORE_INFO, "Failed to read from file '%s'. Error: %d\r\n", filepath, res);
    }
    else if (br != size)
    {
        dbg_printf(DBG_MORE_INFO, "Warning: Read %lu bytes from '%s', expected %lu.\r\n", br, filepath, size);
        res = FR_DENIED; // Or FR_RW_ERROR, or a custom error code
    }

    *bytes_read = br;
    f_close(&fil);
    return res;
}


/**
 * @brief Loads a test vector (X) and its corresponding ground truth (Y) from the SD card using hex directory structure.
 *
 * @param start_index The starting index to attempt loading from.
 * @param sample_data Pointer to a test_sample_t structure to store the loaded data.
 * @param actual_index_loaded Pointer to a uint32_t to store the actual index of the loaded sample.
 * @return FRESULT FR_OK if successful, FR_NO_FILE if not found, or other FatFs error codes.
 */
FRESULT load_next_test_vector(uint32_t start_index, test_sample_t *sample_data, uint32_t *actual_index_loaded)
{
    char x_filepath[MAX_PATH_LEN];
    char y_filepath[MAX_PATH_LEN];
    FRESULT res_x = FR_OK;
    FRESULT res_y = FR_OK;
    uint32_t current_index = start_index;
    uint32_t bytes_read_x = 0;
    uint32_t bytes_read_y = 0;

    if (sample_data == NULL || actual_index_loaded == NULL) {
        return FR_INVALID_PARAMETER;
    }

    // Loop through indices to find a valid pair
    while (current_index < NUM_TEST_SAMPLES)
    {
        // Generate hex-based directory structure paths
        uint8_t dir1 = (current_index >> 16) & 0xFF;
        uint8_t dir2 = (current_index >> 8) & 0xFF;

        // Construct file paths using hex directory structure
        xsprintf(x_filepath, "%s/%02x/%02x/x_test_%06lu.bin",
                 g_x_test_folder, dir1, dir2, current_index);
        xsprintf(y_filepath, "%s/%02x/%02x/y_test_%06lu.bin",
                 g_y_test_folder, dir1, dir2, current_index);

        dbg_printf(DBG_MORE_INFO, "Attempting to load sample %lu:\r\n", current_index);
        dbg_printf(DBG_MORE_INFO, "  X file: %s\r\n", x_filepath);
        dbg_printf(DBG_MORE_INFO, "  Y file: %s\r\n", y_filepath);

        // Try to read both X and Y files
        res_x = read_binary_file(x_filepath, sample_data->x_data, X_TEST_VECTOR_SIZE, &bytes_read_x);
        //res_y = read_binary_file(y_filepath, &(sample_data->y_data), Y_TEST_VECTOR_SIZE, &bytes_read_y);

        if (res_x == FR_OK && res_y == FR_OK)
        {
            // Both files found and read successfully
            sample_data->x_data_size = bytes_read_x;
            sample_data->y_data_size = bytes_read_y;
            *actual_index_loaded = current_index;
            //dbg_printf(DBG_MORE_INFO, "Successfully loaded sample %lu.\r\n", current_index);
            return FR_OK;
        }
        else
        {
            dbg_printf(DBG_MORE_INFO, "  Failed to load sample %lu. X_res: %d, Y_res: %d\r\n", current_index, res_x, res_y);
            current_index++;
        }
    }

    dbg_printf(DBG_MORE_INFO, "No more valid test vector pairs found within the range (0 to %lu).\r\n", NUM_TEST_SAMPLES -1);
    return FR_NO_FILE;
}

/**
 * @brief Saves the raw model output (int8_t vector) using the 2-Level Hex Hash Structure
 * * @param index Index of the test sample (determines directory structure)
 * @param model_output Pointer to the int8_t model output array
 * @param output_length Number of elements in the output array
 * @param file_prefix Prefix for the output filename (e.g., "result_test_")
 * @return FRESULT FR_OK if successful, or FatFs error code
 */
FRESULT save_result_vector(uint32_t index, int8_t *model_output, uint32_t output_length, const char *file_prefix)
{
    char result_path[MAX_PATH_LEN];
    FIL file;
    UINT bytes_written;
    FRESULT res;

    if (index >= NUM_TEST_SAMPLES || model_output == NULL || output_length == 0 || file_prefix == NULL) {
        return FR_INVALID_PARAMETER;
    }

    // Calculate directory levels from index
    uint8_t dir1 = (index >> 16) & 0xFF;
    uint8_t dir2 = (index >> 8) & 0xFF;

    // Construct result file path with variable prefix
    xsprintf(result_path, "%s/%02x/%02x/%s%06lu.bin",
             g_x_test_folder, dir1, dir2, file_prefix, index);

    dbg_printf(DBG_MORE_INFO, "Saving model output for sample %lu:\r\n", index);
    dbg_printf(DBG_MORE_INFO, "  Path: %s\r\n", result_path);
    dbg_printf(DBG_MORE_INFO, "  Output length: %lu elements\r\n", output_length);
    dbg_printf(DBG_MORE_INFO, "  First result value: raw=%d\r\n", model_output[0]);

    // Ensure directory exists
    char dir_path[MAX_PATH_LEN];
    xsprintf(dir_path, "%s/%02x/%02x", g_x_test_folder, dir1, dir2);
    // Try to create the directory, ignore if it already exists
    res = f_mkdir(dir_path);
    if (res != FR_OK && res != FR_EXIST) {
        dbg_printf(DBG_MORE_INFO, "  Directory creation failed: %d\r\n", res);
        return res;
    }

    // Open file for writing
    res = f_open(&file, result_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        dbg_printf(DBG_MORE_INFO, "  File open failed: %d\r\n", res);
        return res;
    }

    // Write raw int8_t data
    res = f_write(&file, model_output, output_length * sizeof(int8_t), &bytes_written);
    if (res != FR_OK || bytes_written != output_length * sizeof(int8_t)) {
        dbg_printf(DBG_MORE_INFO, "  Write failed: %d, bytes %u/%u\r\n",
                res, bytes_written, output_length * sizeof(int8_t));
        f_close(&file);
        return (res == FR_OK) ? FR_DISK_ERR : res;
    }

    f_close(&file);
    dbg_printf(DBG_MORE_INFO, "  Successfully saved %d bytes of model output\r\n", bytes_written);
    return FR_OK;
}

/**
 * @brief Collects model outputs and saves them in a single binary file once 256 results have been collected.
 *
 * @param index Index of the test sample.
 * @param model_output Pointer to the int8_t model output array.
 * @param output_length Number of elements in the output array (must be 1).
 * @param file_prefix Prefix for the output filename.
 * @return FRESULT FR_OK if successful, or FatFs error code.
 */
FRESULT save_result_vector_bulk(uint32_t index, int8_t *model_output, uint32_t output_length, const char *file_prefix)
{
    if (output_length != 1 || model_output == NULL) {
        return FR_INVALID_PARAMETER;
    }

    // Store the result in the bulk array
    if (bulk_results_count < BULK_RESULT_COUNT) {
        bulk_results[bulk_results_count++] = *model_output;
    }

    // If we have collected 256 results, write them to the file
    if (bulk_results_count == BULK_RESULT_COUNT) {
        char result_path[MAX_PATH_LEN];
        FIL file;
        UINT bytes_written;
        FRESULT res;

        // Create the directory path based on the last index
        uint8_t dir1 = (index >> 16) & 0xFF;
        uint8_t dir2 = (index >> 8) & 0xFF;

        xsprintf(result_path, "%s/%02x/%02x/%s.bin",
                 g_x_test_folder, dir1, dir2, file_prefix);

        dbg_printf(DBG_MORE_INFO, "Writing %d collected results to %s\r\n", BULK_RESULT_COUNT, result_path);

        // Ensure the directory exists
        char dir_path[MAX_PATH_LEN];
        xsprintf(dir_path, "%s/%02x/%02x", g_x_test_folder, dir1, dir2);
        res = f_mkdir(dir_path);
        if (res != FR_OK && res != FR_EXIST) {
            dbg_printf(DBG_MORE_INFO, "  Directory creation for bulk file failed: %d\r\n", res);
            return res;
        }

        // Open the file for writing
        res = f_open(&file, result_path, FA_WRITE | FA_CREATE_ALWAYS);
        if (res != FR_OK) {
            dbg_printf(DBG_MORE_INFO, "  Bulk file open failed: %d\r\n", res);
            return res;
        }

        // Write the entire bulk results array
        res = f_write(&file, bulk_results, sizeof(bulk_results), &bytes_written);
        if (res != FR_OK || bytes_written != sizeof(bulk_results)) {
            dbg_printf(DBG_MORE_INFO, "  Bulk write failed: %d, bytes %u/%u\r\n",
                    res, bytes_written, sizeof(bulk_results));
            f_close(&file);
            return (res == FR_OK) ? FR_DISK_ERR : res;
        }

        f_close(&file);
        dbg_printf(DBG_MORE_INFO, "  Successfully saved %d bytes in bulk.\r\n", bytes_written);

        // Reset the counter for the next batch
        bulk_results_count = 0;
    }

    return FR_OK;
}

