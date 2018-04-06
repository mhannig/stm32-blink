
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/spi.h>
#include <libopencm3/cm3/nvic.h>

#include "sh1106.h"

inline void sh1106_tx(uint8_t type, uint8_t b);
inline void sh1106_set_col_start_addr();

void sh1106_init_clocks()
{
    rcc_periph_clock_enable(SH1106_RCC_GPIO_SPI);
    rcc_periph_clock_enable(SH1106_RCC_GPIO_CTRL);
    rcc_periph_clock_enable(SH1106_RCC_SPI);
    rcc_periph_clock_enable(RCC_AFIO);
}

void sh1106_init_gpio()
{
    // Setup CTRL
    // This is required to switch between a command and bitmap data
    gpio_set_mode(SH1106_GPIO_CTRL,
                 GPIO_MODE_OUTPUT_50_MHZ,
                 GPIO_CNF_OUTPUT_PUSHPULL,
                 SH1106_DC | SH1106_RST);


    // Setup SPI GPIO, See configuration in sh1106.h 
    // for pin mapping.
    gpio_set_mode(SH1106_GPIO_SPI,
                  GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_ALTFN_PUSHPULL,
                  SH1106_SCK | SH1106_MOSI);

    gpio_set_mode(SH1106_GPIO_SPI,
                  GPIO_MODE_INPUT,
                  GPIO_CNF_INPUT_FLOAT,
                  SH1106_MISO);

    gpio_set_mode(SH1106_GPIO_SPI,
                  GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_PUSHPULL,
                  SH1106_NSS);

}


void sh1106_init_spi()
{
    spi_reset(SPI1);

    // Initialize master:
    //   Baudrate: 72e6 / 64 = 1125000
    //   Clock Polarity: HI
    //   Clock Phase: falling edge
    //   Dataframe: 8bit
    //   Bit order: MSB first
    spi_init_master(SPI1,
                    SPI_CR1_BAUDRATE_FPCLK_DIV_256,
                    SPI_CR1_CPOL_CLK_TO_1_WHEN_IDLE,
                    SPI_CR1_CPHA_CLK_TRANSITION_2,
                    SPI_CR1_DFF_8BIT,
                    SPI_CR1_MSBFIRST);

    spi_enable_software_slave_management(SPI1);
    spi_set_nss_high(SPI1);

    spi_enable(SPI1);
}


/*
 * Transmit a byte using spi. Set the data / command bit
 * accordingly.
 */
inline void sh1106_tx(uint8_t type, uint8_t b)
{
    // We are using 4-wire SPI
    gpio_clear(SH1106_GPIO_SPI, SH1106_NSS); // !CS

    if(type == SH1106_CMD) {
        gpio_set(SH1106_GPIO_CTRL, SH1106_DC);
    } else {
        gpio_clear(SH1106_GPIO_CTRL, SH1106_DC);
    }

    spi_send(SH1106_SPI, b);

    gpio_set(SH1106_GPIO_SPI,  SH1106_NSS);
    gpio_set(SH1106_GPIO_CTRL, SH1106_DC);

}


/*
 * Display initialization
 */
void sh1106_init_display()
{
    // 4 wire spi
    gpio_set(SH1106_GPIO_SPI, SH1106_NSS); 
    gpio_clear(SH1106_RCC_GPIO_CTRL, SH1106_DC);
    gpio_set(SH1106_RCC_GPIO_CTRL, SH1106_RST);

                                 // Display initialization sequence
    sh1106_tx(SH1106_CMD, 0xAE); // --turn off oled panel
    sh1106_tx(SH1106_CMD, 0x02); // ---set low column address
    sh1106_tx(SH1106_CMD, 0x10); // ---set high column address
    sh1106_tx(SH1106_CMD, 0x40); // --set start line address
                                 //   Set Mapping RAM Display Start Line (0x00~0x3F)
    sh1106_tx(SH1106_CMD, 0x81); // --set contrast control register
    sh1106_tx(SH1106_CMD, 0xA0); // --Set SEG/Column Mapping

    sh1106_tx(SH1106_CMD, 0xC0); // Set COM/Row Scan Direction
    sh1106_tx(SH1106_CMD, 0xA6); // --set normal display
    sh1106_tx(SH1106_CMD, 0xA8); // --set multiplex ratio(1 to 64)
    sh1106_tx(SH1106_CMD, 0x3F); // --1/64 duty
    sh1106_tx(SH1106_CMD, 0xD3); // -set display offset

                                 // Shift Mapping RAM Counter (0x00~0x3F)
    sh1106_tx(SH1106_CMD, 0x00); // -not offset
    sh1106_tx(SH1106_CMD, 0xd5); // --set display clock divide ratio/oscillator
                                 //   frequency
    sh1106_tx(SH1106_CMD, 0x80); // --set divide ratio, Set Clock as 100 Frames/Sec
    sh1106_tx(SH1106_CMD, 0xD9); // --set pre-charge period

    sh1106_tx(SH1106_CMD, 0xF1); // Set Pre-Charge as 15 Clocks & Discharge as 1 Clock
    sh1106_tx(SH1106_CMD, 0xDA); // --set com pins hardware configuration
    sh1106_tx(SH1106_CMD, 0x12);
    sh1106_tx(SH1106_CMD, 0xDB); // --set vcomh

    sh1106_tx(SH1106_CMD, 0x40); // Set VCOM Deselect Level
    sh1106_tx(SH1106_CMD, 0x20); // -Set Page Addressing Mode (0x00/0x01/0x02)
    sh1106_tx(SH1106_CMD, 0x02); //
    sh1106_tx(SH1106_CMD, 0xA4); // Disable Entire Display On (0xa4/0xa5)
    sh1106_tx(SH1106_CMD, 0xA6); // Disable Inverse Display On (0xa6/a7)
    sh1106_tx(SH1106_CMD, 0xAF); // --turn on oled panel
}

inline void sh1106_set_col_start_addr()
{
    sh1106_tx(SH1106_CMD, 0x02);
    sh1106_tx(SH1106_CMD, 0x10);
}

