/*! \file d7ap_fs.c
 *

 *  \copyright (C) Copyright 2019 University of Antwerp and others (http://mosaic-lopow.github.io/dash7-ap-open-source-stack/)
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
 *
 *  \author glenn.ergeerts@uantwerpen.be
 *  \author maarten.weyn@uantwerpen.be
 *
 */

#include "string.h"
#include "debug.h"
#include "fs.h"
#include "d7ap.h"
#include "d7ap_fs.h"

#include "hwsystem.h"
#include "version.h"
#include "key.h"
#include "log.h"

///////////////////////////////////////
// The d7a file header is concatenated with the file data.
// filebody is [$header][$data]
// where length(header) = 12
//     length($data) == $header.length == $header.allocated_length
///////////////////////////////////////



#if defined(FRAMEWORK_LOG_ENABLED) && defined(MODULE_D7AP_FS_LOG_ENABLED)
#define DPRINT(...) log_print_stack_string(LOG_STACK_NWL, __VA_ARGS__)
#define DPRINT_DATA(...) log_print_data(__VA_ARGS__)
#else
#define DPRINT(...)
#define DPRINT_DATA(...)
#endif

#define D7A_PROTOCOL_VERSION_MAJOR 1
#define D7A_PROTOCOL_VERSION_MINOR 1

extern fs_systemfiles_t d7ap_systemfiles;

#define FS_STORAGE_COUNT (sizeof(d7ap_fs)/sizeof(d7ap_fs[0]))

#define FS_ENABLE_SSR_FILTER 0 // TODO always enabled? cmake param?

#define IS_SYSTEM_FILE(file_id) (file_id <= 0x3F)

static d7ap_fs_d7aactp_callback_t d7aactp_callback = NULL;


static inline bool is_file_defined(uint8_t file_id)
{
    fs_file_stat_t *stat = fs_file_stat(file_id);
    return (stat != NULL);
}

static void execute_d7a_action_protocol(uint8_t action_file_id, uint8_t interface_file_id)
{
  if(!d7aactp_callback) return;

  assert(is_file_defined(action_file_id));
  // TODO interface_file_id is optional, how do we code this in file header?
  // for now we assume it's always used
  assert(is_file_defined(interface_file_id));

  d7ap_session_config_t fifo_config;
  d7ap_fs_read_file(interface_file_id, 0, (uint8_t*)&fifo_config, sizeof(d7ap_session_config_t));
  uint32_t action_len = d7ap_fs_get_file_length(action_file_id);
  uint8_t action[action_len];
  fs_read_file(action_file_id, sizeof(d7ap_fs_file_header_t), action, action_len);

  // invoke application callback to parse and execute the action. Result is then given to the D7ASP layer
  d7aactp_callback(&fifo_config, action, action_len);
}

void d7ap_fs_init()
{
  //init fs with the D7A specific system files
  fs_init(&d7ap_systemfiles);

  // TODO platform specific
  // TODO set FW version

  uint8_t uid[8];
  uint8_t uid_not_set[8] = { 0 };
  d7ap_fs_read_uid(uid);
  if(memcmp(uid, uid_not_set, 8) == 0) {
    // initializing UID
    uint64_t id = hw_get_unique_id();
    uint64_t id_be = __builtin_bswap64(id);
    fs_write_file(D7A_FILE_UID_FILE_ID, sizeof(d7ap_fs_file_header_t), (const uint8_t*)&id_be, D7A_FILE_UID_SIZE);
  }

  // always update firmware version file upon boot
  uint8_t firmware_version[D7A_FILE_FIRMWARE_VERSION_SIZE] = {
    D7A_PROTOCOL_VERSION_MAJOR, D7A_PROTOCOL_VERSION_MINOR,
  };

  memcpy(firmware_version + 2, _APP_NAME, D7A_FILE_FIRMWARE_VERSION_APP_NAME_SIZE);
  memcpy(firmware_version + 2 + D7A_FILE_FIRMWARE_VERSION_APP_NAME_SIZE, _GIT_SHA1, D7A_FILE_FIRMWARE_VERSION_GIT_SHA1_SIZE);
  fs_write_file(D7A_FILE_FIRMWARE_VERSION_FILE_ID, sizeof(d7ap_fs_file_header_t), firmware_version, D7A_FILE_FIRMWARE_VERSION_SIZE);
}

int d7ap_fs_init_file(uint8_t file_id, const d7ap_fs_file_header_t* file_header, const uint8_t* initial_data)
{
  int rtc;
  DPRINT("FS init %i, alloc %i", file_id, file_header->allocated_length);
  assert(file_id >= 0x40); // only user files allowed
  assert(file_id - 0x40 < FRAMEWORK_FS_USER_FILE_COUNT);
  // TODO only volatile for now, return error when permanent storage requested

  d7ap_fs_file_header_t file_header_big_endian;
  memcpy(&file_header_big_endian, file_header, sizeof (d7ap_fs_file_header_t));
  file_header_big_endian.length = __builtin_bswap32(file_header_big_endian.length);
  file_header_big_endian.allocated_length = __builtin_bswap32(file_header_big_endian.allocated_length);

  uint8_t default_data[sizeof(d7ap_fs_file_header_t) + file_header->allocated_length];
  memset(default_data, 0, sizeof(d7ap_fs_file_header_t) + file_header->allocated_length);
  memcpy(default_data, (uint8_t *)&file_header_big_endian, sizeof (d7ap_fs_file_header_t));
  if(initial_data != NULL)
    memcpy(default_data + sizeof (d7ap_fs_file_header_t), initial_data, file_header->length);

  rtc = fs_init_file(file_id, file_header->file_properties.storage_class,
		             (const uint8_t *)default_data, sizeof(d7ap_fs_file_header_t) + file_header->allocated_length);
  if (rtc != 0)
    return rtc;
}

int d7ap_fs_init_file_with_d7asp_interface_config(uint8_t file_id, const d7ap_session_config_t* fifo_config)
{
  // TODO store in file 0x18-0x1F - reserved for D7AALP ?
  assert(!IS_SYSTEM_FILE(file_id));
  assert(!is_file_defined(file_id));

  d7ap_fs_file_header_t file_header = (d7ap_fs_file_header_t){
    .file_properties.action_protocol_enabled = 0,
    .file_properties.storage_class = FS_STORAGE_PERMANENT,
    .file_permissions = 0, // TODO
    .length = sizeof(d7ap_session_config_t),
    .allocated_length = sizeof(d7ap_session_config_t),
  };

  return (d7ap_fs_init_file(file_id, &file_header, (const uint8_t *)fifo_config));
}

int d7ap_fs_read_file(uint8_t file_id, uint32_t offset, uint8_t* buffer, uint32_t length)
{
  int rtc;
  d7ap_fs_file_header_t header;

  DPRINT("FS RD %i", file_id);

  if(!is_file_defined(file_id)) return -ENOENT;

  rtc = d7ap_fs_read_file_header(file_id, &header);
  if (rtc != 0)
    return rtc;

  if(header.length < offset + length)
    return -EINVAL;

  rtc = fs_read_file(file_id, sizeof(d7ap_fs_file_header_t) + offset, buffer, length);
  if (rtc != 0)
    return rtc;

  if(header.file_properties.action_protocol_enabled == true
     && header.file_properties.action_condition == D7A_ACT_COND_READ)
  {
    execute_d7a_action_protocol(header.action_file_id, header.interface_file_id);
  }

  return 0;
}

int d7ap_fs_read_file_header(uint8_t file_id, d7ap_fs_file_header_t* file_header)
{
  int rtc;
  if(!is_file_defined(file_id)) return -ENOENT;

  rtc = fs_read_file(file_id, 0, (uint8_t *)file_header, sizeof(d7ap_fs_file_header_t));
  if (rtc != 0)
    return rtc;

  // convert to little endian (native)
  file_header->length = __builtin_bswap32(file_header->length);
  file_header->allocated_length = __builtin_bswap32(file_header->allocated_length);
  return 0;
}

int d7ap_fs_write_file_header(uint8_t file_id, d7ap_fs_file_header_t* file_header)
{
  if(!is_file_defined(file_id)) return -ENOENT;

  // Input of data shall be in big-endian ordering
  file_header->length = __builtin_bswap32(file_header->length);
  file_header->allocated_length = __builtin_bswap32(file_header->allocated_length);

  return (fs_write_file(file_id, 0, (const uint8_t*)file_header, sizeof(d7ap_fs_file_header_t)));
}

int d7ap_fs_write_file(uint8_t file_id, uint32_t offset, const uint8_t* buffer, uint32_t length)
{
  int rtc;
  d7ap_fs_file_header_t header;

  DPRINT("FS WR %i", file_id);

  if(!is_file_defined(file_id)) return -ENOENT;

  rtc = d7ap_fs_read_file_header(file_id, &header);
  if (rtc != 0)
    return rtc;

  if(header.allocated_length < offset + length)
    return -EINVAL;

  rtc = fs_write_file(file_id, sizeof(d7ap_fs_file_header_t) + offset, buffer, length);
  if (rtc != 0)
    return rtc;

  //update header
  if (header.length != offset + length)
  {
    header.length = offset + length;
    rtc = d7ap_fs_write_file_header(file_id, &header);
    if (rtc != 0)
      return rtc;
  }

  if(header.file_properties.action_protocol_enabled == true
    && header.file_properties.action_condition == D7A_ACT_COND_WRITE) // TODO ALP_ACT_COND_WRITEFLUSH?
  {
    execute_d7a_action_protocol(header.action_file_id, header.interface_file_id);
  }

  return 0;
}

int d7ap_fs_read_uid(uint8_t *buffer)
{
  return (d7ap_fs_read_file(D7A_FILE_UID_FILE_ID, 0, buffer, D7A_FILE_UID_SIZE));
}

int d7ap_fs_read_vid(uint8_t *buffer)
{
  return (d7ap_fs_read_file(D7A_FILE_VID_FILE_ID, 0, buffer, ID_TYPE_VID_LENGTH));
}

int d7ap_fs_read_nwl_security_key(uint8_t *buffer)
{
  return d7ap_fs_read_file(D7A_FILE_NWL_SECURITY_KEY, 0, buffer, D7A_FILE_NWL_SECURITY_KEY_SIZE);
}

int d7ap_fs_read_nwl_security(dae_nwl_security_t *nwl_security)
{
  int rtc;

  rtc = d7ap_fs_read_file(D7A_FILE_NWL_SECURITY, 0, (uint8_t*)nwl_security, D7A_FILE_NWL_SECURITY_SIZE);
  if (rtc == 0)
    nwl_security->frame_counter = (uint32_t)__builtin_bswap32(nwl_security->frame_counter); // correct endianess

  return rtc;
}

int d7ap_fs_write_nwl_security(dae_nwl_security_t *nwl_security)
{
  if(!is_file_defined(D7A_FILE_NWL_SECURITY)) return -ENOENT;

  dae_nwl_security_t sec;
  memcpy(&sec, nwl_security, sizeof (sec));
  sec.frame_counter = (uint32_t)__builtin_bswap32(nwl_security->frame_counter); // correct endianess
  return (d7ap_fs_write_file(D7A_FILE_NWL_SECURITY, 0, (uint8_t*)&sec, D7A_FILE_NWL_SECURITY_SIZE));
}

int d7ap_fs_read_nwl_security_state_register(dae_nwl_ssr_t *node_security_state)
{
  int rtc;

  if(!is_file_defined(D7A_FILE_NWL_SECURITY_STATE_REG)) return -ENOENT;

  rtc = (d7ap_fs_read_file(D7A_FILE_NWL_SECURITY_STATE_REG, 0,
                           (uint8_t*)node_security_state, sizeof(dae_nwl_ssr_t)));
  if (rtc != 0)
    return rtc;

   // correct endiannes
  for(uint8_t i = 0; i < node_security_state->trusted_node_nb; i++)
  {
    node_security_state->trusted_node_table[i].frame_counter = (uint32_t)__builtin_bswap32(node_security_state->trusted_node_table[i].frame_counter);
  }

  return rtc;
}

static int write_security_state_register_entry(dae_nwl_trusted_node_t *trusted_node, uint8_t trusted_node_nb)
{
  assert(trusted_node_nb <= FRAMEWORK_FS_TRUSTED_NODE_TABLE_SIZE);

  uint16_t entry_offset = (D7A_FILE_NWL_SECURITY_SIZE + D7A_FILE_UID_SIZE)*(trusted_node_nb - 1) + 2;
  // correct endiannes before writing
  dae_nwl_trusted_node_t node;
  memcpy(&node, trusted_node, sizeof(dae_nwl_trusted_node_t));
  node.frame_counter = __builtin_bswap32(node.frame_counter);
  return (d7ap_fs_write_file(D7A_FILE_NWL_SECURITY, entry_offset, (uint8_t*)&node, sizeof(dae_nwl_trusted_node_t)));
}

int d7ap_fs_add_nwl_security_state_register_entry(dae_nwl_trusted_node_t *trusted_node,
                                                  uint8_t trusted_node_nb)
{
  // TODO test
  assert(trusted_node_nb <= FRAMEWORK_FS_TRUSTED_NODE_TABLE_SIZE);
  if(!is_file_defined(D7A_FILE_NWL_SECURITY_STATE_REG)) return -ENOENT;

  // first add the new entry ...
  write_security_state_register_entry(trusted_node, trusted_node_nb);
  // ... and finally update the node count
  return (d7ap_fs_write_file(D7A_FILE_NWL_SECURITY, 1, &trusted_node_nb, 1));
}

int d7ap_fs_update_nwl_security_state_register(dae_nwl_trusted_node_t *trusted_node,
                                               uint8_t trusted_node_index)
{
  if(!is_file_defined(D7A_FILE_NWL_SECURITY_STATE_REG)) return -ENOENT;

  return (write_security_state_register_entry(trusted_node, trusted_node_index));
}

int d7ap_fs_read_access_class(uint8_t access_class_index, dae_access_profile_t *access_class)
{
  assert(access_class_index < 15);
  assert(is_file_defined(D7A_FILE_ACCESS_PROFILE_ID + access_class_index));
  int result = d7ap_fs_read_file(D7A_FILE_ACCESS_PROFILE_ID + access_class_index, 0, (uint8_t*)access_class, D7A_FILE_ACCESS_PROFILE_SIZE);
  for(int i=0; i<SUBBANDS_NB; i++) {
    access_class->subbands[i].channel_index_start = __builtin_bswap16(access_class->subbands[i].channel_index_start);
    access_class->subbands[i].channel_index_end = __builtin_bswap16(access_class->subbands[i].channel_index_end);
  }
  return result;
}

int d7ap_fs_write_access_class(uint8_t access_class_index, dae_access_profile_t* access_class)
{
  assert(access_class_index < 15);
  assert(is_file_defined(D7A_FILE_ACCESS_PROFILE_ID + access_class_index));
  dae_access_profile_t* temp_access_class;
  memcpy(&temp_access_class, &access_class, sizeof(dae_access_profile_t));
  for(int i=0; i<SUBBANDS_NB; i++) {
    temp_access_class->subbands[i].channel_index_start = __builtin_bswap16(temp_access_class->subbands[i].channel_index_start);
    temp_access_class->subbands[i].channel_index_end = __builtin_bswap16(temp_access_class->subbands[i].channel_index_end);
  }
  return d7ap_fs_write_file(D7A_FILE_ACCESS_PROFILE_ID + access_class_index, 0, (uint8_t*)temp_access_class, D7A_FILE_ACCESS_PROFILE_SIZE);
}

uint8_t d7ap_fs_read_dll_conf_active_access_class()
{
  uint8_t access_class;
  d7ap_fs_read_file(D7A_FILE_DLL_CONF_FILE_ID, 0, &access_class, 1);
  return access_class;
}

int d7ap_fs_write_dll_conf_active_access_class(uint8_t access_class)
{
  return (d7ap_fs_write_file(D7A_FILE_DLL_CONF_FILE_ID, 0, &access_class, 1));
}

uint32_t d7ap_fs_get_file_length(uint8_t file_id)
{
  d7ap_fs_file_header_t header;

  assert(is_file_defined(file_id));

  d7ap_fs_read_file_header(file_id, &header);
  return header.length;
}

bool d7ap_fs_register_d7aactp_callback(d7ap_fs_d7aactp_callback_t d7aactp_cb)
{
  d7aactp_callback = d7aactp_cb;
  return true;
}
