/* * OSS-7 - An opensource implementation of the DASH7 Alliance Protocol for ultra
 * lowpower wireless sensor communication
 *
 * Copyright 2015 University of Antwerp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "machine/gpio.h"

#include "scheduler.h"
#include "bootstrap.h"
#include "platform.h"
#include "button.h"
#include "fs.h"

#include "hwgpio.h"
#include "hwleds.h"
#include "hwwatchdog.h"

#include "hwblockdevice.h"
#include "blockdevice_ram.h"

#include "button.h"
#include "cortus_gpio.h"

/*** Cortus FPGA only supports a simple RAM-based blockdevice ***/

extern uint8_t d7ap_permanent_files_data[FRAMEWORK_FS_PERMANENT_STORAGE_SIZE];
extern uint8_t d7ap_volatile_files_data[FRAMEWORK_FS_VOLATILE_STORAGE_SIZE];

static blockdevice_ram_t permanent_bd = (blockdevice_ram_t){
 .base.driver = &blockdevice_driver_ram,
 .size = FRAMEWORK_FS_PERMANENT_STORAGE_SIZE,
 .buffer = d7ap_permanent_files_data
};

static blockdevice_ram_t volatile_bd = (blockdevice_ram_t){
 .base.driver = &blockdevice_driver_ram,
 .size = FRAMEWORK_FS_VOLATILE_STORAGE_SIZE,
 .buffer = d7ap_volatile_files_data
};

blockdevice_t * const permanent_blockdevice = (blockdevice_t* const) &permanent_bd;
blockdevice_t * const volatile_blockdevice = (blockdevice_t* const) &volatile_bd;


void __platform_init()
{
    __gpio_init();
    __led_init();
    __ubutton_init();

#ifdef USE_CC1101
    // configure the interrupt pins here, since hw_gpio_configure_pin() is MCU
    // specific and not part of the common HAL API
    hw_gpio_configure_pin(CC1101_GDO0_PIN, true, gpioModeInput, 0);
    hw_gpio_configure_pin(CC1101_GDO2_PIN, true, gpioModeInput, 0);
    // hw_gpio_configure_pin(CC1101_SPI_PIN_CS, false, gpioModePushPull, 1);
#endif

#ifdef USE_SX127X
    // configure the interrupt pins here, since hw_gpio_configure_pin() is MCU
    // specific and not part of the common HAL API
    hw_gpio_configure_pin(SX127x_DIO0_PIN, true, gpioModeInput, 0);
    hw_gpio_configure_pin(SX127x_DIO1_PIN, true, gpioModeInput, 0);
#endif

    //__watchdog_init();

    /* Initialize NVRAM */


    /* Initialize NOR flash */
    blockdevice_init(permanent_blockdevice);
}

int main (void)
{

    __platform_init();

    __framework_bootstrap();

    //__platform_post_framework_init();

    scheduler_run();

    return 0;
}
