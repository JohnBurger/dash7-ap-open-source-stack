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

#include "scheduler.h"
#include "bootstrap.h"
#include "hwgpio.h"
#include "hwuart.h"
#include "hwleds.h"
#include "hwlcd.h"
#include "hwusb.h"
#include "ezr32lg_mcu.h"
#include "hwdebug.h"
#include "hwwatchdog.h"
#include "platform.h"
#include "userbutton.h"
#include "platform_sensors.h"
#include "em_gpio.h"
#include <debug.h>

#include "em_cmu.h"
#include "em_chip.h"


#include "console.h"

void SWO_SetupForPrint(void)
{
  /* Enable GPIO clock. */
  CMU->HFPERCLKEN0 |= CMU_HFPERCLKEN0_GPIO;

  /* Enable Serial wire output pin */
  GPIO->ROUTE |= GPIO_ROUTE_SWOPEN;

#if defined(_EFM32_GIANT_FAMILY) || defined(_EFM32_LEOPARD_FAMILY) || defined(_EFM32_WONDER_FAMILY)
  /* Set location 0 */
  GPIO->ROUTE = (GPIO->ROUTE & ~(_GPIO_ROUTE_SWLOCATION_MASK)) | GPIO_ROUTE_SWLOCATION_LOC0;

  /* Enable output on pin - GPIO Port F, Pin 2 */
  GPIO->P[5].MODEL &= ~(_GPIO_P_MODEL_MODE2_MASK);
  GPIO->P[5].MODEL |= GPIO_P_MODEL_MODE2_PUSHPULL;
#else
  /* Set location 1 */
  GPIO->ROUTE = (GPIO->ROUTE & ~(_GPIO_ROUTE_SWLOCATION_MASK)) |GPIO_ROUTE_SWLOCATION_LOC1;
  /* Enable output on pin */
  GPIO->P[2].MODEH &= ~(_GPIO_P_MODEH_MODE15_MASK);
  GPIO->P[2].MODEH |= GPIO_P_MODEH_MODE15_PUSHPULL;
#endif

  /* Enable debug clock AUXHFRCO */
  CMU->OSCENCMD = CMU_OSCENCMD_AUXHFRCOEN;

  /* Wait until clock is ready */
  while (!(CMU->STATUS & CMU_STATUS_AUXHFRCORDY));

  /* Enable trace in core debug */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  ITM->LAR  = 0xC5ACCE55;
  ITM->TER  = 0x0;
  ITM->TCR  = 0x0;
  TPI->SPPR = 2;
  TPI->ACPR = 0xf;
  ITM->TPR  = 0x0;
  DWT->CTRL = 0x400003FF;
  ITM->TCR  = 0x0001000D;
  TPI->FFCR = 0x00000100;
  ITM->TER  = 0x1;
}


void __platform_init()
{
	__ezr32lg_mcu_init();
    __gpio_init();
    __led_init();
    __lcd_init();
    console_init();



//#ifdef USE_CC1101
    //TODO: add calls to hw_gpio_configure_pin for the pins used by the CC1101 driver
    //(and possibly the spi interface)

    // configure the interrupt pins here, since hw_gpio_configure_pin() is MCU specific and not part of the common HAL API
    //hw_gpio_configure_pin(CC1101_GDO0_PIN, true, gpioModeInput, 0); // TODO pull up or pull down to prevent floating
    //hw_gpio_configure_pin(CC1101_GDO2_PIN, true, gpioModeInput, 0) // TODO pull up or pull down to prevent floating // TODO not used for now
//#endif
    __hw_debug_init();

    __watchdog_init(); // TODO configure from cmake?
}

void __platform_post_framework_init()
{
    __ubutton_init();

#ifdef PLATFORM_USE_USB_CDC
    __usb_init_cdc();
#endif

#ifdef PLATFORM_USE_SWO
    SWO_SetupForPrint();
#endif
}

int main()
{
    // Only when using bootloader
	//SCB->VTOR=0x4000;

    //initialise the platform itself
	__platform_init();
    //do not initialise the scheduler, this is done by __framework_bootstrap()
    __framework_bootstrap();
    //initialise platform functionality that depends on the framework
    __platform_post_framework_init();
    scheduler_run();
    return 0;
}
