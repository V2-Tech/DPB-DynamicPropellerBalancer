#include "application_def.h"
#include "application.h"

app_steps _app_step = IDLE;

/***********************************
 *      FUNCTION DEFINITIONS       *
 ***********************************/
uint8_t app_init(Motor *motor, gpio_num_t opto_gpio_num, QueueHandle_t xQueueSysInput_handle, QueueHandle_t xQueueSysOutput_handle, FIFOBuffer<acc_sensor_data> *pDataBuffer, fft_chart_data *pFFTOuput)
{
    uint8_t ret = ESP_OK;

    if ((xQueueSysInput_handle == 0) || (xQueueSysOutput_handle == 0) || (motor == 0) || (pDataBuffer == 0) || (pFFTOuput == 0))
    {
        return ESP_FAIL;
    }

    _xQueueSysInput = xQueueSysInput_handle;
    _xQueueSysOutput = xQueueSysOutput_handle;
    _pDataBuffer = pDataBuffer;
    _pMotor = motor;
    _pFFTOuput = pFFTOuput;

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    io_conf.pin_bit_mask = (1ULL << opto_gpio_num);
    io_conf.mode = GPIO_MODE_INPUT;
    // io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE; // enable pull-down mode
    // io_conf.pull_up_en = GPIO_PULLUP_ENABLE;  // disable pull-up mode

    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_EDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(opto_gpio_num, (gpio_isr_t)opto_isr_handler, (void *)opto_gpio_num));
    gpio_intr_disable(GPIO_OPT_SENSOR);

    /*
        _vibeRecTimer = xTimerCreate("VibeRecorderTimer", // Just a text name, not used by the kernel.
                                     VIBE_RECORD_TIME_MS, // The timer period in ticks.
                                     pdFALSE,             // The timers will auto-reload themselves when they expire.
                                     0,                   // Assign each timer a unique id equal to its array index.
                                     vibeTimerCallback    // Each timer calls the same callback when it expires.
        );
    */
    return ret;
}

void app_loop()
{
    command_data command;

    /* Check incoming commands to execute */
    if (xQueueReceive(_xQueueSysInput, &command, portMAX_DELAY) == pdPASS)
    {
        app_exe(command);
    }
}

void app_exe(command_data command)
{
    switch (command.command)
    {
    case APP_CMD:
        if (command.value == IDLE)
        {
            app_reset();
        }
        if (command.value == POS_SEARCH)
        {
            app_start();
        }
        if (command.value == VIBES_REC)
        {
            app_rec_timer_start();
        }
        if (command.value == FILTERING)
        {
            ;
        }
        if (command.value == ANALYSING)
        {
            app_fft();
        }
        break;

    case MOTOR_CMD:
        _pMotor->set_throttle(command.value);
        break;

    default:
        break;
    }
}

void app_start()
{
    if (_app_step > IDLE)
    {
        return;
    }

    /* Start motor rotation and wait until reference blade hits the pos sensor */
    _pMotor->set_throttle(150);
    gpio_intr_enable(GPIO_OPT_SENSOR);

    _app_step = POS_SEARCH;
}

void app_reset()
{
    _pMotor->set_throttle(0);
    gpio_intr_disable(GPIO_OPT_SENSOR);
    _app_step = IDLE;
}

void app_rec_timer_start()
{
    /* Pos sensor hitted: vibration recording ongoing */
    // xTimerStart(_vibeRecTimer, 0);
    _app_step = VIBES_REC;
}

void app_fft(void)
{
    _pMotor->set_throttle(0);
    _app_step = ANALYSING;

    esp_err_t ret;
    int32_t N = ACC_DATA_BUFFER_SIZE;

    // Window coefficients
    __attribute__((aligned(16)))
    float_t wind[ACC_DATA_BUFFER_SIZE];

    // Init FFT filter
    ret = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    if (ret != ESP_OK)
    {
        char *TAG = "app_fft";
        ESP_LOGE(TAG, "Not possible to initialize FFT. Error = %i", ret);
        return;
    }

    // Generate hann window
    dsps_wind_hann_f32(wind, N);

    // Input signal
    __attribute__((aligned(16))) static float y_filtered[ACC_DATA_BUFFER_SIZE];
    // working complex array
    __attribute__((aligned(16))) static float y_cf[ACC_DATA_BUFFER_SIZE * 2];

    // Generate input signal
    // dsps_tone_gen_f32(x1, N, 1, -0.45, 0);

    // Convert input vectors to complex vectors
    ESP_LOGW(TAG, "Signal accZ");
    for (int32_t i = 0; i < N; i++)
    {
        acc_sensor_data tempData;

        tempData = _pDataBuffer->pop();
        _pFFTOuput[0].fft_data[i] = tempData.accel_data[0];
        _pFFTOuput[1].fft_data[i] = tempData.accel_data[1];
        _pFFTOuput[2].fft_data[i] = tempData.accel_data[2];

        printf("%.0f\n", _pFFTOuput[1].fft_data[i]);
    }

    float coeffs_lpf[5];
    float w_lpf[5] = {0, 0};
    // Calculate iir filter coefficients
    ret = dsps_biquad_gen_lpf_f32(coeffs_lpf, 0.25, 2);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Operation error = %i", ret);
        return;
    }
    // Filter signal
    ret = dsps_biquad_f32(_pFFTOuput[1].fft_data, y_filtered, N, coeffs_lpf, w_lpf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Operation error = %i", ret);
        return;
    }
    ESP_LOGI(TAG, "Filtered signal:");
    for (int32_t i = 0; i < N; i++)
    {
        printf("%.0f\n", y_filtered[i]);
    }

    for (int32_t i = 0; i < N; i++)
    {
        _pFFTOuput[0].fft_data[i] = _pFFTOuput[0].fft_data[i] * wind[i];
        _pFFTOuput[1].fft_data[i] = _pFFTOuput[1].fft_data[i] * wind[i];
        _pFFTOuput[2].fft_data[i] = _pFFTOuput[2].fft_data[i] * wind[i];

        // y_cf[i * 2 + 0] = x1[i] * wind[i];
        y_cf[i * 2 + 0] = y_filtered[i] * wind[i];
        y_cf[i * 2 + 1] = 0;
    }

    /* // FFT Radix-4
    dsps_fft4r_fc32(_pFFTOuput[0].fft_data, N >> 1);
    // Bit reverse
    dsps_bit_rev4r_fc32(_pFFTOuput[0].fft_data, N >> 1);
    // Convert one complex vector with length N/2 to one real spectrum vector with length N/2
    dsps_cplx2real_fc32(_pFFTOuput[0].fft_data, N >> 1);

    // FFT Radix-4
    dsps_fft4r_fc32(_pFFTOuput[1].fft_data, N >> 1);
    // Bit reverse
    dsps_bit_rev4r_fc32(_pFFTOuput[1].fft_data, N >> 1);
    // Convert one complex vector with length N/2 to one real spectrum vector with length N/2
    dsps_cplx2real_fc32(_pFFTOuput[1].fft_data, N >> 1);

    // FFT Radix-4
    dsps_fft4r_fc32(_pFFTOuput[2].fft_data, N >> 1);
    // Bit reverse
    dsps_bit_rev4r_fc32(_pFFTOuput[2].fft_data, N >> 1);
    // Convert one complex vector with length N/2 to one real spectrum vector with length N/2
    dsps_cplx2real_fc32(_pFFTOuput[2].fft_data, N >> 1); */

    // FFT
    dsps_fft2r_fc32(y_cf, N);
    // Bit reverse
    dsps_bit_rev_fc32(y_cf, N);
    // Convert one complex vector to two complex vectors
    dsps_cplx2reC_fc32(y_cf, N);
    for (int i = 0; i < N / 2; i++)
    {
        printf("%.0f;%.0f\n", y_cf[i * 2 + 0], y_cf[i * 2 + 1]);
    }

    // Transfor to power spectrum
    for (int i = 0; i < N / 2; i++)
    {
        _pFFTOuput[0].fft_data[i] = 10 * log10f((_pFFTOuput[0].fft_data[i * 2 + 0] * _pFFTOuput[0].fft_data[i * 2 + 0] + _pFFTOuput[0].fft_data[i * 2 + 1] * _pFFTOuput[0].fft_data[i * 2 + 1] + 0.0000001) / N);
        _pFFTOuput[1].fft_data[i] = 10 * log10f((_pFFTOuput[1].fft_data[i * 2 + 0] * _pFFTOuput[1].fft_data[i * 2 + 0] + _pFFTOuput[1].fft_data[i * 2 + 1] * _pFFTOuput[1].fft_data[i * 2 + 1] + 0.0000001) / N);
        _pFFTOuput[2].fft_data[i] = 10 * log10f((_pFFTOuput[2].fft_data[i * 2 + 0] * _pFFTOuput[2].fft_data[i * 2 + 0] + _pFFTOuput[2].fft_data[i * 2 + 1] * _pFFTOuput[2].fft_data[i * 2 + 1] + 0.0000001) / N);

        y_cf[i] = 10 * log10f((y_cf[i * 2 + 0] * y_cf[i * 2 + 0] + y_cf[i * 2 + 1] * y_cf[i * 2 + 1]) / N);
    }

    ESP_LOGW(TAG, "Signal y_cf");
    dsps_view(y_cf, N / 2, 64, 20, -180, 120, '|');
    y_cf[0] = y_cf[1];
}

void vibeTimerCallback(TimerHandle_t pxTimer)
{
    /* Vibration recording completed: stop motor and start FFT analisys */
    acceleration_stop_read();
    _pMotor->set_throttle(0);
}

void IRAM_ATTR opto_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken;
    uint32_t gpio_num = (uint32_t)arg;

    vTaskNotifyGiveIndexedFromISR(senseTaskHandle, 0, &xHigherPriorityTaskWoken);

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
