#ifndef APP_SCENARIO_ALLON_SENSOR_TFLM_AF_TESTBENCH_RUN
#define APP_SCENARIO_ALLON_SENSOR_TFLM_AF_TESTBENCH_RUN

#include "spi_protocol.h"
#include "sd_card_testbench.h"

#ifdef __cplusplus
extern "C" {
#endif

int init_model(bool security_enable, bool privilege_enable);
void create_tensor(test_sample_t* sample) ;
void run_model();
void get_model_result(int8_t *model_output);
int cv_deinit();

#ifdef __cplusplus
}
#endif

#endif
