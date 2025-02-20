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

// This is an example application where the stack is running on an a standalone MCU,
// typically used in combination with another MCU where the main application (for instance sensor reading)
// in running. The application accesses the stack using the serial modem interface.

#include "hwleds.h"
#include "hwsystem.h"
#include "hwuart.h"
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>

#include "log.h"
#include "modem.h"
#include "d7ap.h"
#include "scheduler.h"
#include "timer.h"

#define TX_INTERVAL_TICKS (TIMER_TICKS_PER_SEC * 2)

#define FILE_ID 0x40

// This example application shows how to integrate a serial D7 modem

static uart_handle_t* modem_uart;


// define the D7 interface configuration used for sending the file data to
static session_config_t session_config   = 
{
    .interface_type = DASH7,
    .d7ap_session_config = {
        .qos = {
          .qos_resp_mode = SESSION_RESP_MODE_NO,
          .qos_retry_mode = SESSION_RETRY_MODE_NO,
          .qos_stop_on_error = false,
          .qos_record = false
        },
        .dormant_timeout = 0,
        .addressee = {
          .ctrl = {
            .nls_method = AES_NONE,
            .id_type = ID_TYPE_NOID,
          },
          .access_class = 0x01,
          .id = 0
        }
    }
};

void send_counter() {
  static uint8_t counter = 0;
  log_print_string("counter %i", counter);
  modem_send_unsolicited_response(FILE_ID, 0, 1, &counter, &session_config);
  counter++;
}

void command_completed_cb(bool with_error,  uint8_t tag_id) {
  log_print_string("command completed!");
  timer_post_task_delay(&send_counter, TX_INTERVAL_TICKS);
}

void return_file_data_cb(uint8_t file_id, uint32_t offset, uint32_t size, uint8_t* buffer) {
  if(file_id == D7A_FILE_UID_FILE_ID && size == D7A_FILE_UID_SIZE) {
    log_print_string("received modem uid:");
    log_print_data(buffer, size);
  }
}

void modem_rebooted_cb(system_reboot_reason_t reboot_reason) {
  log_print_string("modem rebooted (reason %i)\n", (uint8_t)reboot_reason);
}

modem_callbacks_t callbacks = (modem_callbacks_t){
  .command_completed_callback = &command_completed_cb,
  .return_file_data_callback = &return_file_data_cb,
  .modem_rebooted_callback = &modem_rebooted_cb
};


void bootstrap() {
  modem_init();
  modem_cb_init(&callbacks);

  sched_register_task(&send_counter);
  sched_post_task(&send_counter);
  log_print_string("Device booted\n");
}

