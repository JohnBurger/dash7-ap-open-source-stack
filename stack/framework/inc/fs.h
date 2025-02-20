/*! \file fs.h
 *

 *  \copyright (C) Copyright 2019 University of Antwerp and others (http://mosaic-lopow.github.io/dash7-ap-open-source-stack/)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/*! \file fs.h
 * \addtogroup Fs
 * \ingroup framework
 * @{
 * \brief Filesystem APIs used as an abstraction level for underneath FS usage (eg LittleFS)
 * \author	philippe.nunes@cortus.com
 */

#ifndef FS_H_
#define FS_H_

#include "stdint.h"

#ifndef FRAMEWORK_FS_FILE_COUNT
#define FRAMEWORK_FS_FILE_COUNT 80
#endif

#ifndef FRAMEWORK_FS_USER_FILE_COUNT
#define FRAMEWORK_FS_USER_FILE_COUNT 33
#endif

#ifndef FRAMEWORK_FS_PERMANENT_STORAGE_SIZE
#define FRAMEWORK_FS_PERMANENT_STORAGE_SIZE 1800
#endif

#ifndef FRAMEWORK_FS_VOLATILE_STORAGE_SIZE
#define FRAMEWORK_FS_VOLATILE_STORAGE_SIZE 1024
#endif

#define FS_MAGIC_NUMBER { 0x34, 0xC2, 0x00, 0x00 } // first 2 bytes fixed, last 2 byte for version
#define FS_MAGIC_NUMBER_SIZE 4

typedef enum
{
    FS_STORAGE_TRANSIENT = 0, // The content is not kept in memory. It cannot be read back.
    FS_STORAGE_VOLATILE = 1,  // The content is kept in a volatile memory of the device. It is accessible for read, and is lost on power off.
    FS_STORAGE_RESTORABLE = 2, // The content is kept in a volatile memory of the device. It is accessible for read, and can be backed-up upon request in a permanent storage location. It is restored from the permanent location on device power on.
    FS_STORAGE_PERMANENT = 3  // The content is kept in a permanent memory of the device. It is accessible for read and write
} fs_storage_class_t;

typedef struct  __attribute__((__packed__))
{
    uint32_t length;
    fs_storage_class_t storage : 2;
    uint8_t rfu : 6; //FIXME: 'valid' field or invalid storage qualifier?
} fs_file_stat_t;


/* \brief The callback function for when a user file is modified
 *
 * \param file_id		The id of the modified user file
 * **/
typedef void (*fs_modified_file_callback_t)(uint8_t file_id);

/**
 * @brief Descriptor of a file system
 */
typedef struct __attribute__((__packed__)) {
  uint8_t magic_number[FS_MAGIC_NUMBER_SIZE];   /**< used to validate FS contents */
  size_t nfiles; /**< Number of files */
  uint8_t* files_length; /**< Files length array */
  uint16_t* files_offset; /**< Files offset array */
  uint8_t* files_data; /**< Files data array */
} fs_systemfiles_t;

void fs_init(fs_systemfiles_t* provisioning);
int fs_init_file(uint8_t file_id, fs_storage_class_t storage, const uint8_t* initial_data, uint32_t length);
int fs_read_file(uint8_t file_id, uint32_t offset, uint8_t* buffer, uint32_t length);
int fs_write_file(uint8_t file_id, uint32_t offset, const uint8_t* buffer, uint32_t length);
fs_file_stat_t *fs_file_stat(uint8_t file_id);

bool fs_register_file_modified_callback(uint8_t file_id, fs_modified_file_callback_t callback);

#endif /* FS_H_ */

/** @}*/
