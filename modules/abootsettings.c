/**
   @file abootsettings.c

   This plug-in can control aboot's device info data in emmc.
   User can change e.g. device lock value for aboot.
   <p>

   Copyright (c) 2017 - 2020 Jolla Ltd.
   Copyright (c) 2020 Open Mobile Platform LLC.

   @author Marko Lemmetty <marko.lemmetty@jollamobile.com>
   @author Simo Piiroinen <simo.piiroinen@jollamobile.com>

   This file is part of Dsme.

   Dsme is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License
   version 2.1 as published by the Free Software Foundation.

   Dsme is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with Dsme.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dbusproxy.h"
#include "dsme_dbus.h"

#include "../include/dsme/modules.h"
#include "../include/dsme/modulebase.h"
#include "../include/dsme/logging.h"

#include <dsme/state.h>

#include <glib.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/fs.h>

#include "../include/dsme/musl-compatibility.h"

#define PFIX                     "abootsettings: "
// Device info magic string (from aboot)
#define DEVICE_MAGIC             "ANDROID-BOOT!"
#define DEVICE_MAGIC_SIZE        13
// Panel ID length in l500d (from aboot)
#define MAX_PANEL_ID_LEN         64
// Magic size in emmc (from aboot)
#define DEVICE_MAGIC_DATA_SIZE16 16
// Version (from aboot)
#define DEVICE_INFO_VERSION_1    1
#define DEVICE_INFO_VERSION_2    2  // android 6
#define DEVICE_INFO_VERSION_3    3  // android 6 VBOOT MOTA
// DBus return value
#define ABOOTSET_RET_OK          1
// Block size is 512 bytes, so dev info will fit to 1k buffer
#define DEVINFO_BUF_SIZE         1024
// Ini file path
#define ABOOTSET_INI             "/etc/dsme/abootsettings.ini"

#define MAX_VERSION_LEN          64

// -------------------------------------
// device info defines and variables etc.
// -------------------------------------
typedef struct device_info device_info;
// Support for SFOS aboot device info versions 1, 2 and 3.
struct device_info
{
    unsigned char   magic[DEVICE_MAGIC_SIZE]; // 13 bytes
    int32_t         is_unlocked;
    int32_t         is_tampered;
    int32_t         is_verified;
    int32_t         is_unlock_critical;
    int32_t         charger_screen_enabled;
    char            display_panel[MAX_PANEL_ID_LEN];
    char            bootloader_version[MAX_VERSION_LEN];
    char            radio_version[MAX_VERSION_LEN];
    int32_t         verity_mode;       // 1 = enforcing, 0 = logging
    uint32_t        devinfo_version;   // Androids device info version
};

static device_info device;
// SFOS devcie info version for passport.
static int32_t     device_info_version = 0;
static int         partition = -1;
static int         block_size = 0;
// Start of the device info data in partition.
static off_t       devinfo_data_offset = 0;
// Partition name is read from ini file.
static gchar*      partition_name = NULL;

// -------------------------------------
// D-Bus defines
// -------------------------------------

// Bind methods when D-Bus is connected.
static bool        dbus_methods_bound = false;
// Is plug-in initialized.
static bool        abootsettings_init = false;

// --------------------------------------
// Functions
// --------------------------------------

static bool open_partition(int flag);
static void close_partition();
static bool set_emmc_block_size();
static bool set_file_offset();
static bool get_unlocked_value(int* unlocked);
static bool set_unlocked_value(int value);
static bool write_device_info_to_disk();
static bool read_device_info_from_disk();
static bool decode_device_info(device_info* dev, char* buf);
static int  encode_device_info(device_info* dev, char* buf);

/* ========================================================================= *
 * D-Bus Query API
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * get_locked
 *
 * Function to read device info lock status from emmc.
 * If function return '1' deivce is locked (fastboot flash is disabled),
 * othervice device is unlocked.
 * -------------------------------------------------------------------------
 */
static void get_locked(const DsmeDbusMessage* request,
                       DsmeDbusMessage** reply)
{
    int unlocked = 0;

    dsme_log(LOG_DEBUG, PFIX"get_locked");

    // Read device info and get unlocked value.
    if( !get_unlocked_value(&unlocked) )
    {
        dsme_log(LOG_ERR, PFIX"Error: Failed to read dev info");

        *reply = dsme_dbus_reply_error( request, DBUS_ERROR_IO_ERROR,
                                       "Failed to read device info" );
        return;
    }

    dsme_log(LOG_DEBUG, PFIX"return locked to client");

    *reply = dsme_dbus_reply_new(request);
    // Since this function returns locked status,
    // we need to invert unlocked value.
    dsme_dbus_message_append_int(*reply, !unlocked);

}

/* -------------------------------------------------------------------------
 * set_locked
 *
 * Function to set device info lock value.
 * if locked = 1  -> fastboot flash disabled,
 * if locked = 0  -> fastboot flash enabled
 * -------------------------------------------------------------------------
 */
static void set_locked(const DsmeDbusMessage* request,
                       DsmeDbusMessage** reply)
{
    int locked = -1;

    dsme_log(LOG_DEBUG, PFIX"set_locked");

    // Get input value
    locked = dsme_dbus_message_get_int(request);

    // Check that input value is 0 or 1.
    if( locked != 0 && locked != 1 )
    {
        dsme_log(LOG_ERR, PFIX"Error: Invalid input value");

        *reply = dsme_dbus_reply_error(request, DBUS_ERROR_INVALID_ARGS,
                                       "Invalid input value");
        return;
    }

    // Read device info from partition and write new unlocked value to emmc.
    // Since we have lock value we need to invert it.
    if( !set_unlocked_value(!locked) )
    {
        dsme_log(LOG_ERR, PFIX"Error: Failed to write dev info");

        *reply = dsme_dbus_reply_error(request, DBUS_ERROR_IO_ERROR,
                                       "Failed to write device info");
        return;
    }

    dsme_log(LOG_DEBUG, PFIX"return OK");

    *reply = dsme_dbus_reply_new(request);
    // Return message to client.
    dsme_dbus_message_append_int(*reply, ABOOTSET_RET_OK);
}

static const char abootsettings_service[]   = "org.sailfishos.abootsettings";
static const char abootsettings_interface[] = "org.sailfishos.abootsettings";
static const char abootsettings_path[]      = "/org/sailfishos/abootsettings";

static const dsme_dbus_binding_t dbus_methods_array[] =
{
    // method calls
    {
        .method = get_locked,
        .name   = "get_locked",
        .args   =
            "    <arg direction=\"out\" name=\"state\" type=\"i\"/>\n"
    },
    {
        .method = set_locked,
        .name   = "set_locked",
        .priv   = true,
        .args   =
            "    <arg direction=\"in\" name=\"state\" type=\"i\"/>\n"
            "    <arg direction=\"out\" name=\"success\" type=\"i\"/>\n"
    },
    // sentinel
    {
        .name   = 0,
    }
};

/* ========================================================================= *
 * Internal DSME event handling
 * ========================================================================= */

DSME_HANDLER(DSM_MSGTYPE_DBUS_CONNECTED, client, msg)
{
    dsme_log(LOG_DEBUG, PFIX"DSM_MSGTYPE_DBUS_CONNECTED");

    // Check that plug-in is initialized.
    if( abootsettings_init )
    {
        dsme_log(LOG_DEBUG, PFIX"bind methods");
        dsme_dbus_bind_methods(&dbus_methods_bound,
                               abootsettings_service,
                               abootsettings_path,
                               abootsettings_interface,
                               dbus_methods_array);
    }
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_DISCONNECT, client, msg)
{
    dsme_log(LOG_DEBUG, PFIX"DSM_MSGTYPE_DBUS_DISCONNECT");
}

module_fn_info_t message_handlers[] =
{
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_CONNECTED),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_DISCONNECT),
    { 0 }
};

/* ========================================================================= *
 * Plugin init and fini
 * ========================================================================= */

void module_init(module_t* handle)
{
    GKeyFile* key_file = NULL;

    dsme_log(LOG_DEBUG, PFIX"module_init");

    key_file = g_key_file_new();

    if ( key_file )
    {
        GError* error = NULL;
        GKeyFileFlags flags = G_KEY_FILE_NONE;

        // Open ini file and read partition path.
        if ( g_key_file_load_from_file(key_file, ABOOTSET_INI, flags, &error) )
        {
            partition_name = g_key_file_get_string(key_file,
                                                   "deviceinfo",
                                                   "partition",
                                                    &error);
            if ( partition_name )
            {
                // Ok, we have partition name.
                // Let DSME register functions to DBus.
                abootsettings_init = true;
            }
            else
            {
                dsme_log(LOG_ERR, PFIX"%s: deviceinfo partition not defined",
                         ABOOTSET_INI);
            }
        }
        else
        {
            dsme_log(error->code == G_FILE_ERROR_NOENT ? LOG_DEBUG : LOG_ERR,
                     PFIX"%s: INI file could not be loaded: %s",
                     ABOOTSET_INI, error->message);
        }

        g_key_file_free(key_file);
        g_clear_error(&error);
    }

    dsme_log(LOG_DEBUG, PFIX"module_init done");
}

void module_fini(void)
{
    dsme_log(LOG_DEBUG, PFIX"module_fini");

    dsme_dbus_unbind_methods(&dbus_methods_bound,
                             abootsettings_service,
                             abootsettings_path,
                             abootsettings_interface,
                             dbus_methods_array);

    abootsettings_init = false;

    g_free(partition_name);
    partition_name = NULL;
}

/* ========================================================================= *
 * Device info funtions
 * ========================================================================= */

/*-----------------------------------------------------------------------------
 * open_partition
 * Flags O_RDONLY / O_RDWR
 *-----------------------------------------------------------------------------
 */
static bool open_partition(int flag)
{
    dsme_log(LOG_DEBUG, PFIX"open_partition");

    // If we have file descriptor open, return true.
    if( partition != -1 )
    {
        return true;
    }

    if( partition_name == NULL )
    {
        return false;
    }

    partition = open(partition_name, flag);

    if( partition == -1 )
    {
        dsme_log(LOG_ERR, PFIX"Error: Can't open partition (%d)",
            partition);
        return false;
    }

    dsme_log(LOG_DEBUG, PFIX"Partition open successful");
    return true;
}

/*-----------------------------------------------------------------------------
 * close_partition
 *-----------------------------------------------------------------------------
 */
static void close_partition()
{
    if( partition != -1 )
    {
        dsme_log(LOG_DEBUG, PFIX"Close partition (%d)", partition);
        close(partition);
        partition = -1;
    }
}

/*-----------------------------------------------------------------------------
 * set_emmc_block_size
 *
 * Function to get eMMC sector size. BLKPBSZGET should return physical sector
 * size in linux. We assume that this is same as eMMC block size used in aboot.
 *-----------------------------------------------------------------------------
 */
static bool set_emmc_block_size()
{
    int ret = 0;
    block_size = 0;

    dsme_log(LOG_DEBUG, PFIX"set_emmc_block_size");

    if( partition == -1 )
    {
        dsme_log(LOG_ERR, PFIX"Error: partition not open");
        return false;
    }

    ret = ioctl(partition, BLKPBSZGET, &block_size);

    if( ret < 0 || block_size <= 0 )
    {
        dsme_log(LOG_ERR, PFIX"Error: ioctl = %d, block size = %d",
            ret, block_size);
        block_size = 0;
        return false;
    }

    dsme_log(LOG_DEBUG, PFIX"block size = %d", block_size);

    return true;
}

/*-----------------------------------------------------------------------------
 * set_file_offset
 *
 * Set file pointer to last block of partition.
 *-----------------------------------------------------------------------------
 */
static bool set_file_offset()
{
    off_t partition_size = 0;
    devinfo_data_offset = 0;

    dsme_log(LOG_DEBUG, PFIX"set_file_offset");

    if( partition == -1 )
    {
        dsme_log(LOG_ERR, PFIX"Error: partition not open");
        return false;
    }

    if( !set_emmc_block_size() )
    {
        dsme_log(LOG_ERR, PFIX"Error: failed to get block size");
        return false;
    }

    // Get partition size.
    partition_size = lseek(partition, 0, SEEK_END);

    dsme_log(LOG_DEBUG, PFIX"Partition size = %llu",
        (unsigned long long)partition_size);

    // Set file pointer to start.
    lseek(partition, 0, SEEK_SET);

    if( partition_size <= block_size )
    {
        dsme_log(LOG_ERR, PFIX"Error: Partition size");
        return false;
    }

    // Calc. file offset for last block in aboot partition.
    devinfo_data_offset = (partition_size - block_size);

    if( devinfo_data_offset <= 0 )
    {
         dsme_log(LOG_ERR, PFIX"Error: offset null");
         return false;
    }
    dsme_log(LOG_DEBUG, PFIX"Offset = %llu",
        (unsigned long long)devinfo_data_offset);

    return true;
}

/* --------------------------------------------------------------------------
 * encode_device_info
 *
 * Function to encode device info. This will use same impl. as in aboot.
 * See encode_device_info in aboot.c
 * --------------------------------------------------------------------------
 */
static int encode_device_info(device_info *dev, char* buf)
{
    int size = 0;
    int32_t value = 0;

    dsme_log(LOG_DEBUG, PFIX"decode_device_info");

    if( buf == NULL || dev == NULL )
    {
        return 0;
    }

    // Check that we support this version and copy data to buffer
    if( device_info_version == DEVICE_INFO_VERSION_1 ||
        device_info_version == DEVICE_INFO_VERSION_2 ||
        device_info_version == DEVICE_INFO_VERSION_3 )
    {
        // Copy version number (4 bytes)

        value = device_info_version;
        memcpy(buf, &value, sizeof value);
        size = sizeof value;

        // Copy magic (16 bytes)

        memcpy(buf+size, dev->magic, DEVICE_MAGIC_SIZE);
        size += DEVICE_MAGIC_DATA_SIZE16;

        // Copy is_unlocked (4 bytes)

        value = dev->is_unlocked;
        memcpy(buf+size, &value, sizeof value);
        size += sizeof value;

        // Copy is_tampered (4 bytes)

        value = dev->is_tampered;
        memcpy(buf+size, &value, sizeof value);
        size += sizeof value;

        if ( device_info_version == DEVICE_INFO_VERSION_2 )
        {
            // Copy is_unlock_critical (4 bytes)

            value = dev->is_unlock_critical;
            memcpy(buf+size, &value, sizeof value);
            size += sizeof value;
        }
        else
        {
            // Copy is_verified (4 bytes)

            value = dev->is_verified;
            memcpy(buf+size, &value, sizeof value);
            size += sizeof value;
        }

        // Copy charger_screen_enabled (4 bytes)

        value = dev->charger_screen_enabled;
        memcpy(buf+size, &value, sizeof value);
        size += sizeof value;

        // Copy display_panel (64 bytes)

        memcpy(buf+size, dev->display_panel, MAX_PANEL_ID_LEN);
        size += MAX_PANEL_ID_LEN;

        if ( device_info_version == DEVICE_INFO_VERSION_2 ||
             device_info_version == DEVICE_INFO_VERSION_3 )
        {
            // Copy bootloader_version (64 bytes)

            memcpy(buf+size, dev->bootloader_version, MAX_VERSION_LEN);
            size += MAX_VERSION_LEN;

            // Copy radio_version (64 bytes)

            memcpy(buf+size, dev->radio_version, MAX_VERSION_LEN);
            size += MAX_VERSION_LEN;
        }

        if ( device_info_version == DEVICE_INFO_VERSION_2 )
        {
            uint32_t u_value = 0;

            // Copy verity_mode (4 bytes)

            value = dev->verity_mode;
            memcpy(buf+size, &value, sizeof value);
            size += sizeof value;

            // Copy devinfo_version (4 bytes)

            u_value = dev->devinfo_version;
            memcpy(buf+size, &u_value, sizeof u_value);
            size += sizeof u_value;
        }
    }
    else
    {
        // No support for this version.
        dsme_log(LOG_ERR, PFIX"Error: This version not supported");
        return 0;
    }

    dsme_log(LOG_DEBUG, PFIX"encoded size = %d", size);

    return size;
}

/* ------------------------------------------------------------------------
 * decode_device_info
 *
 * Function to decode device info data. This will use same impl. as in aboot.
 * See decode_device_info in aboot.c
 * ------------------------------------------------------------------------
 */
static bool decode_device_info(device_info* dev, char* buf)
{
    int size = 0;
    int32_t value = 0;

    dsme_log(LOG_DEBUG, PFIX"decode_device_info");

    // Copy version number

    memcpy(&value, buf, sizeof value);
    size = sizeof value;
    dsme_log(LOG_DEBUG, PFIX"Device info version (%d)", value);

    // Save current device info version
    device_info_version = value;

    // Check that we can support this version and read data from buffer.
    if( device_info_version == DEVICE_INFO_VERSION_1 ||
        device_info_version == DEVICE_INFO_VERSION_2 ||
        device_info_version == DEVICE_INFO_VERSION_3 )
    {
        // Copy magic.
        // Copy 13 bytes to buffer
        memcpy( dev->magic, buf+size, DEVICE_MAGIC_SIZE );
        // Move index 16 bytes, since data size in disk is 16 bytes
        size += DEVICE_MAGIC_DATA_SIZE16;

        // Check that we have magic
        if( memcmp(dev->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE) )
        {
            dsme_log(LOG_ERR, PFIX"Device magic not found");
            return false;
        }

        // Copy is_unlocked

        memcpy(&value, buf+size, sizeof value);
        size += sizeof value;

        // Check that value is "bool" e.g. 0 or 1
        // Note that in aboot bool is integer
        if( value == 0 || value == 1 )
        {
            dev->is_unlocked = value;
        }
        else
        {
            dsme_log(LOG_ERR, PFIX"is_unlocked value not in range");
            return false;
        }

        // Copy is_tampered

        memcpy(&value, buf+size, sizeof value);
        size += sizeof value;

        if( value == 0 || value == 1 )
        {
            dev->is_tampered = value;
        }
        else
        {
            dsme_log(LOG_ERR, PFIX"is_tampered value not in range");
            return false;
        }

        if ( device_info_version == DEVICE_INFO_VERSION_2 )
        {
            // Copy is_unlock_critical (4 bytes)

            memcpy(&value, buf+size, sizeof value);
            size += sizeof value;

            if( value == 0 || value == 1 )
            {
                dev->is_unlock_critical = value;
            }
            else
            {
                dsme_log(LOG_ERR, PFIX"is_unlock_critical value not in range");
                return false;
            }
        }
        else
        {
            // Copy is_verified

            memcpy(&value, buf+size, sizeof value);
            size += sizeof value;

            if( value == 0 || value == 1 )
            {
                dev->is_verified = value;
            }
            else
            {
                dsme_log(LOG_ERR, PFIX"is_verified value not in range");
                return false;
            }
        }

        // Copy charger_screen_enabled

        memcpy(&value, buf+size, sizeof value);
        size += sizeof value;

        if( value == 0 || value == 1 )
        {
            dev->charger_screen_enabled = value;
        }
        else
        {
            dsme_log(LOG_ERR, PFIX"charger_screen value not in range");
            return false;
        }

        // Copy display_panel (64 bytes)

        memcpy(dev->display_panel, buf+size, MAX_PANEL_ID_LEN );
        size += MAX_PANEL_ID_LEN;

        if ( device_info_version == DEVICE_INFO_VERSION_2 ||
             device_info_version == DEVICE_INFO_VERSION_3 )
        {
            // Copy bootloader_version (64 bytes)

            memcpy(dev->bootloader_version, buf+size, MAX_VERSION_LEN );
            size += MAX_VERSION_LEN;

            // Copy radio_version (64 bytes)

            memcpy(dev->radio_version, buf+size, MAX_VERSION_LEN );
            size += MAX_VERSION_LEN;
        }

        if ( device_info_version == DEVICE_INFO_VERSION_2 )
        {
            uint32_t u_value = 0;

            // Copy verity_mode (4 bytes)

            memcpy(&value, buf+size, sizeof value);
            size += sizeof value;

            if( value == 0 || value == 1 )
            {
                dev->verity_mode = value;
            }
            else
            {
                dsme_log(LOG_ERR, PFIX"verity_mode value not in range");
                return false;
            }

            // Copy devinfo_version (4 bytes)

            memcpy(&u_value, buf+size, sizeof u_value);
            dev->devinfo_version = u_value;
        }

        return true;
    }
    else
    {
      dsme_log(LOG_ERR, PFIX"Error: Version not supported");
      return false;
    }
}

/* -------------------------------------------------------------------------
 * read_device_info_from_disk
 * -------------------------------------------------------------------------
 */
static bool read_device_info_from_disk()
{
    char data[DEVINFO_BUF_SIZE];
    memset(data, 0, DEVINFO_BUF_SIZE);

    dsme_log(LOG_DEBUG, PFIX"read_device_info_from_disk");

    // Set file pointer to offset e.g. last block of partition.
    if( lseek(partition, devinfo_data_offset, SEEK_SET) < 0 )
    {
        dsme_log(LOG_ERR, PFIX"Error: Failed to seek to offser");
        return false;
    }

    if( block_size > DEVINFO_BUF_SIZE )
    {
        dsme_log(LOG_ERR, PFIX"Error: block size too big");
        return false;
    }

    // Read device info.
    if( read(partition, &data, block_size) < block_size )
    {
        dsme_log(LOG_ERR, PFIX"Error: Failed to read");
        return false;
    }

    return decode_device_info(&device, data);
}

/* -------------------------------------------------------------------------
 * write_device_info_to_disk
 * -------------------------------------------------------------------------
 */
static bool write_device_info_to_disk()
{
    int byte_count = 0;
    char data[DEVINFO_BUF_SIZE];
    char* data_ptr = &data[0];
    memset(data, 0, DEVINFO_BUF_SIZE);

    dsme_log(LOG_DEBUG, PFIX"write_device_info");

    // Encode device info to buffer.
    if( encode_device_info(&device, data) == 0 )
    {
        dsme_log(LOG_ERR, PFIX"Error: encoded failed");
        return false;
    }

    // Set file pointer to last block of aboot partition.
    if( lseek(partition, devinfo_data_offset, SEEK_SET) < 0 )
    {
        dsme_log(LOG_ERR, PFIX"Error: failed to seek offset");
        return false;
    }

    byte_count = block_size;

    while( byte_count > 0 )
    {
        int written = TEMP_FAILURE_RETRY(write(partition,
                                               data_ptr,
                                               byte_count));
        if( written < 0 )
        {
            dsme_log(LOG_ERR, PFIX"Error: failed to write: %m");
            return false;
        }
        data_ptr += written;
        byte_count -= written;
    }

    dsme_log(LOG_DEBUG, PFIX"Device info write successful");
    return true;
}

/* ------------------------------------------------------------------------
 * get_unlocked_value
 * ------------------------------------------------------------------------
 */
static bool get_unlocked_value(int* unlocked)
 {
    bool retOk = false;

    dsme_log(LOG_DEBUG, PFIX"get_unlocked_value");

    if( !open_partition(O_RDONLY) )
    {
        return false;
    }

    // Set file offset for reading device info data.
    if( set_file_offset() )
    {
        if( read_device_info_from_disk() )
        {
            *unlocked = (int)device.is_unlocked;
            retOk = true;

            dsme_log(LOG_DEBUG, PFIX" [ is_unlocked = %d ]",
                (int)device.is_unlocked);
        }
    }

    close_partition();
    return retOk;
 }

/* ------------------------------------------------------------------------
 * set_unlocked_value
 * ------------------------------------------------------------------------
 */
static bool set_unlocked_value(int value)
 {
    bool retOk = false;

    dsme_log(LOG_DEBUG, PFIX"set_unlocked_value");

    if( !open_partition(O_RDWR) )
    {
        return false;
    }

    // Set file offset to last block of partition.
    if( set_file_offset() )
    {
        // Read data from partition.
        if( read_device_info_from_disk() )
        {
            device.is_unlocked = value;

            dsme_log(LOG_DEBUG, PFIX" [ is_unlocked = %d ]",
                (int)device.is_unlocked );

            if( write_device_info_to_disk() )
            {
                retOk = true;
            }
        }
    }

    close_partition();
    return retOk;
 }

//EOF
