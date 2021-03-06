/*
 * ADC test: Try to get as many samples as possible
 * using fast interleave mode and DMA
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/timer.h>

#include "usb_serial.h"
#include "cr4_fft.h"
#include "sqrt.h"

#define MIC_RCC  RCC_GPIOA
#define MIC_PORT GPIOA
#define MIC_PIN  GPIO0

#define FFT_LEN 1024
#define SAMPLE_BUF_LEN 1024
uint16_t _adc_samples[SAMPLE_BUF_LEN];
uint32_t _fft_data[SAMPLE_BUF_LEN];
uint32_t _fft_result[SAMPLE_BUF_LEN];
uint16_t _fft_window[SAMPLE_BUF_LEN];

#define C_REAL(X) (X & 0xffff)
#define C_IMAG(X) (X >> 16)


void fft_magnitude(uint32_t* values, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        int16_t rl = C_REAL(values[i]);
        int16_t im = C_IMAG(values[i]);
        uint32_t mag = fast_sqrt(rl*rl + im*im);
        values[i] = mag;
    }
}


/*
 * Precalculate hamming window weights, so we can just
 * multiply adc values with the window.
 */
void fft_hamming_init(uint16_t* window, size_t len)
{
    for(size_t i = 0; i < SAMPLE_BUF_LEN; i++) {
        window[i] = 0;
    }


    for(size_t i = 0; i < len; i++) {
        window[i] = (0.53 - 0.46 * cos((2.0*M_PI*i) / (len-1))) * 65535;
    }
}

void fft_hamming_apply(uint32_t* values, uint16_t* window, size_t len)
{
    for(size_t i = 0; i < len; i++) {
        values[i] = ((values[i] * window[i]) >> 16) & 0xffff;
    }
}


void adc_gain(uint16_t* values, size_t len, uint16_t gain)
{
    // gain 0, 100, 255, .. -> 0.0, 1.0, 2.55, ..
    for(size_t i = 0; i < len; i++) {
        int32_t v = 2048 + \
            ((((int32_t)values[i] - 2048) * (int32_t)gain) / 100);
        // clipping
        if (v > 4096) {
            v = 4096;
        }
        else if (v < 0) {
            v = 0;
        }
        values[i] = (uint16_t)v;
    }
}

void adc_gpio_init()
{
    // Enable GPIOA
    rcc_periph_clock_enable(MIC_RCC);

    // Configure pin as input
    gpio_set_mode(MIC_PORT,
                  GPIO_MODE_INPUT,
                  GPIO_CNF_INPUT_ANALOG,
                  MIC_PIN);
}

void adc_timer_init()
{
    rcc_periph_clock_enable(RCC_TIM2);
    timer_reset(TIM2);

    // No div, edge aligned, up counting
    timer_set_mode(TIM2,
                   TIM_CR1_CKD_CK_INT,
                   TIM_CR1_CMS_EDGE,
                   TIM_CR1_DIR_UP);

    timer_set_prescaler(TIM2, 0);

    // 72 MHz clock, 40 kHz sampling freq,
    // 1800 cycles
    timer_set_period(TIM2, 1800);
    timer_set_oc_value(TIM2, TIM_OC2, 1800);

    // Try 30 kHz
    // timer_set_period(TIM2, 2400);
    // timer_set_oc_value(TIM2, TIM_OC2, 2400);

    // Try 20 kHz
    // timer_set_period(TIM2, 3600);
    // timer_set_oc_value(TIM2, TIM_OC2, 3600);

    // Try 6 kHz
    // timer_set_period(TIM2, 12000);
    // timer_set_oc_value(TIM2, TIM_OC2, 12000);

    // Enable output compare event
    timer_set_oc_mode(TIM2,  TIM_OC2, TIM_OCM_PWM1);
    timer_disable_oc_clear(TIM2, TIM_OC2);
    timer_enable_oc_output(TIM2, TIM_OC2);
}


void adc_timer_start()
{
    TIM2_CR1 |= TIM_CR1_CEN;
}


/*
 * Initialize single ADC triggered by TIM1 for a
 * fixed sampling rate of 4 kHz
 */
void adc_init()
{
    // Setup GPIO
    adc_gpio_init();

    // Setup trigger timer
    adc_timer_init();

    // Configure ADC1
    rcc_periph_clock_enable(RCC_ADC1);

    // ADC should not run during configuration
    adc_power_off(ADC1);

    // Configure ADCs
    adc_disable_scan_mode(ADC1);
    adc_set_right_aligned(ADC1);

    // adc_set_sample_time_on_all_channels(ADC1, ADC_SMPR_SMP_1DOT5CYC);
    adc_set_sample_time_on_all_channels(ADC1, ADC_SMPR_SMP_55DOT5CYC);

    // Single conversion on external trigger
    adc_enable_external_trigger_regular(ADC1, ADC_CR2_EXTSEL_TIM2_CC2);
    adc_set_single_conversion_mode(ADC1);

    // Set channels
    uint8_t channels[] = {0,};
    adc_set_regular_sequence(ADC1, 1, channels);

    // Enable DMA
    adc_enable_dma(ADC1);

    // Power on
    adc_power_on(ADC1);

    // Wait a bit
    for (uint32_t i = 0; i < 800000; i++) {
        __asm__("nop");
    }

    // Calibrate
    adc_reset_calibration(ADC1);
    adc_calibrate(ADC1);

    // Start
    adc_timer_start();
}


void dma_init()
{
    // We use DMA2, Channel 1
    rcc_periph_clock_enable(RCC_DMA1);

    // Disable for configuration
    dma_disable_channel(DMA1, DMA_CHANNEL1);

    // Set source and dst address
    dma_set_memory_address(DMA1, DMA_CHANNEL1,     (uint32_t)&_adc_samples);
    dma_set_peripheral_address(DMA1, DMA_CHANNEL1, (uint32_t)&ADC1_DR);

    // Setup DMA2 controller:
    // We transfer from peripheral ADC1, a single half word
    dma_set_peripheral_size(DMA1, DMA_CHANNEL1, DMA_CCR_PSIZE_16BIT);
    dma_set_memory_size(DMA1, DMA_CHANNEL1, DMA_CCR_MSIZE_16BIT);

    // We read into mem
    dma_set_read_from_peripheral(DMA1, DMA_CHANNEL1);

    // Increment addr
    dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL1);

    // Enable IRQ
    nvic_enable_irq(NVIC_DMA1_CHANNEL1_IRQ);
    dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL1);
}


void adc_dma_transfer_start()
{
    dma_set_number_of_data(DMA1, DMA_CHANNEL1, FFT_LEN);
    dma_enable_channel(DMA1, DMA_CHANNEL1);
}


void dma1_channel1_isr()
{
    // Check Transfer complete interrupt flag
    if (dma_get_interrupt_flag(DMA1, DMA_CHANNEL1, DMA_TCIF)) {
        dma_disable_channel(DMA1, DMA_CHANNEL1);

        // Add gain to signal
        adc_gain(_adc_samples, SAMPLE_BUF_LEN, 150);

        // Fill buffer
        for(uint16_t i = 0; i < SAMPLE_BUF_LEN; i++) {
            // remap [0.0, 0.5, 1.0] represented as [0, 2048, 4096]
            // to [0, 65535] -> floor(65535 / 2048)
            _fft_data[i] = (15 * _adc_samples[i % FFT_LEN]) & 0xffff;
        }

        // FFT
        fft_hamming_apply(_fft_data, _fft_window, 1024);
        cr4_fft_1024_stm32((void*)_fft_result, (void*)_fft_data, 1024);

        fft_magnitude(_fft_result, FFT_LEN/2);
        for (int i = 0; i < FFT_LEN/2; i++) {
            printf("%d %lu\r\n", i, _fft_result[i]);
        }

        /*
        uint16_t max = 0;
        uint32_t avg = 0;

        for(uint16_t i = 0; i < SAMPLE_BUF_LEN; i++) {
            if (max < _adc_samples[i]) {
                max = _adc_samples[i];
            }
            avg += _adc_samples[i];
        }
        avg /= SAMPLE_BUF_LEN;
        printf("%d %d\r\n", max, max - avg);
        */


        /*
        // Just printout the samples
        for (int i = 0; i < 1024; i++) {
            printf("%d %d\r\n", i, _adc_samples[i]);
        }
        */


        // Clear transfer complete.
        dma_clear_interrupt_flags(DMA1, DMA_CHANNEL1, DMA_TCIF);

        // Enable Transfer
        adc_dma_transfer_start();
    }
}



int main(void)
{
	int i = 0;
    // const char* line;

    // Clock Setup
    rcc_clock_setup_in_hse_8mhz_out_72mhz();

    // Initialize USB
	usb_serial_init();

    // Initialize DMA
    dma_init();

    // Initialize ADC
    adc_init();

    // Init window
    fft_hamming_init(_fft_window, 1024);

    // Start fetching data
    printf("Starting ADC read\r\n");
    adc_dma_transfer_start();

	while (1) {
        /*
        if( i % 100000  == 0 ) {
            // Read ADC
            printf("Fnord 42 :: %d %d\r\n", _adc_samples[0], _adc_samples[1]);
        }
        */

        i++;
    }
}
