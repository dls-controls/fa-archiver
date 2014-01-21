/* Code to interface to fa_sniffer device.
 *
 * Copyright (c) 2011 Michael Abbott, Diamond Light Source Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Contact:
 *      Dr. Michael Abbott,
 *      Diamond Light Source Ltd,
 *      Diamond House,
 *      Chilton,
 *      Didcot,
 *      Oxfordshire,
 *      OX11 0DE
 *      michael.abbott@diamond.ac.uk
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>
#include <limits.h>

#include "error.h"
#include "buffer.h"

#include "fa_sniffer.h"
#include "sniffer.h"
#include "replay.h"
#include "mask.h"
#include "disk.h"
#include "transform.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Special ESRF hack for converting corrector readings. */

/* Last row for initialisation. */
#define ESRF_CORRECTOR_COUNT    (14*8)
static struct fa_entry esrf_last_row[ESRF_CORRECTOR_COUNT];
#define ESRF_ROW_SIZE   (ESRF_CORRECTOR_COUNT * FA_ENTRY_SIZE)

/* Sign extend from 14 to 32 bits. */
static int32_t sign_extend(int32_t x)
{
    return (int32_t) ((uint32_t) x << (32-14)) >> 14;
}

static void extract_esrf_correctors(void *block, size_t block_size)
{
    const struct disk_header *header = get_header();
    ASSERT_OK(header->fa_entry_count >= 512);
    unsigned int row_count =
        block_size / FA_ENTRY_SIZE / header->fa_entry_count;

    struct fa_entry *row = block;
    struct fa_entry *last_row = esrf_last_row;
    for (unsigned int i = 0; i < row_count;  i ++)
    {
        memcpy(row + 256, last_row, ESRF_ROW_SIZE);
        /* X&Y data packed into ids 241 to 250. */
        for (int id = 241; id <= 248; id ++)
        {
            struct fa_entry *entry = &row[id];
            int ix_in = (entry->x >> 28) & 0xF;
            int ix_out = 2*ix_in + 14*(id - 241) + 256;

            row[ix_out].x = sign_extend(entry->x >> 14);
            row[ix_out].y = sign_extend(entry->y >> 14);
            row[ix_out+1].x = sign_extend(entry->x);
            row[ix_out+1].y = sign_extend(entry->y);
        }

        last_row = &row[256];
        row += header->fa_entry_count;
    }
    memcpy(esrf_last_row, last_row, ESRF_ROW_SIZE);
}


/* This is where the sniffer data will be written. */
static struct buffer *fa_block_buffer;

/* This will be initialised with the appropriate context to use. */
static const struct sniffer_context *sniffer_context;


static void *sniffer_thread(void *context)
{
    const size_t fa_block_size = buffer_block_size(fa_block_buffer);
    bool in_gap = false;            // Only report gap once
    while (true)
    {
        bool sniffer_ok = true;
        while (sniffer_ok)
        {
            void *buffer = get_write_block(fa_block_buffer);
            uint64_t timestamp;
            sniffer_ok = sniffer_context->read(
                buffer, fa_block_size, &timestamp);

            extract_esrf_correctors(buffer, fa_block_size);

            /* Ignore any error generated by releasing the write block, apart
             * from logging it -- any error here will generate a gap which will
             * be handled properly downstream anyway. */
            IGNORE(TEST_OK_(release_write_block(
                fa_block_buffer, !sniffer_ok, timestamp),
                "Disk writer has fallen behind, dropping sniffer data"));

            if (sniffer_ok == in_gap)
            {
                /* Log change in gap status. */
                if (sniffer_ok)
                    log_message("Block read successfully");
                else
                {
                    /* Try and pick up the reason for the failure. */
                    struct fa_status status;
                    if (sniffer_context->status(&status))
                        log_message(
                            "Unable to read block: "
                            "%d, %d, 0x%x, %d, %d, %d, %d, %d",
                            status.status, status.partner,
                            status.last_interrupt, status.frame_errors,
                            status.soft_errors, status.hard_errors,
                            status.running, status.overrun);
                    else
                        log_message("Unable to read block");
                }
            }
            in_gap = !sniffer_ok;
        }

        /* Pause before retrying.  Ideally should poll sniffer card for
         * active network here. */
        sleep(1);
        IGNORE(sniffer_context->reset());
    }
    return NULL;
}


bool get_sniffer_status(struct fa_status *status)
{
    return sniffer_context->status(status);
}

bool interrupt_sniffer(void)
{
    return sniffer_context->interrupt();
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Standard sniffer using true sniffer device. */

static const char *fa_sniffer_device;
static int fa_sniffer;
static bool ioctl_ok;
static int ioctl_version = 0;

#define IOCTL_TIMESTAMP_VERSION     2   // Supports timestamp interrogation


static bool reset_sniffer_device(void)
{
    if (ioctl_ok)
        /* If possible use the restart command to restart the sniffer. */
        return TEST_IO(ioctl(fa_sniffer, FASNIF_IOCTL_RESTART));
    else
        return
            /* Backwards compatible code: close and reopen the device. */
            TEST_IO(close(fa_sniffer))  &&
            TEST_IO(fa_sniffer = open(fa_sniffer_device, O_RDONLY));
}

static bool read_sniffer_device(
    struct fa_row *rows, size_t length, uint64_t *timestamp)
{
    void *buffer = rows;
    while (length > 0)
    {
        ssize_t rx = read(fa_sniffer, buffer, length);
        if (rx <= 0)
            return false;
        length -= (size_t) rx;
        buffer += (size_t) rx;
    }

    if (ioctl_version >= IOCTL_TIMESTAMP_VERSION)
    {
        struct fa_timestamp fa_timestamp;
        return
            TEST_IO(ioctl(
                fa_sniffer, FASNIF_IOCTL_GET_TIMESTAMP, &fa_timestamp))  &&
            TEST_OK_(fa_timestamp.residue == 0, "Block size mismatch")  &&
            DO_(*timestamp = fa_timestamp.timestamp);
    }
    else
        return DO_(*timestamp = get_timestamp());
}

static bool read_sniffer_status(struct fa_status *status)
{
    return TEST_IO_(ioctl(fa_sniffer, FASNIF_IOCTL_GET_STATUS, status),
        "Unable to read sniffer status");
}

static bool interrupt_sniffer_device(void)
{
    return
        TEST_OK_(ioctl_ok, "Interrupt not supported")  &&
        TEST_IO(ioctl(fa_sniffer, FASNIF_IOCTL_HALT));
}

static const struct sniffer_context sniffer_device = {
    .reset = reset_sniffer_device,
    .read = read_sniffer_device,
    .status = read_sniffer_status,
    .interrupt = interrupt_sniffer_device,
};

const struct sniffer_context *initialise_sniffer_device(
    const char *device_name, unsigned int fa_entry_count)
{
    fa_sniffer_device = device_name;
    bool ok = TEST_IO_(
        fa_sniffer = open(fa_sniffer_device, O_RDONLY),
        "Can't open sniffer device %s", fa_sniffer_device);
    ioctl_ok = ok  &&  TEST_IO_(
        ioctl_version = ioctl(fa_sniffer, FASNIF_IOCTL_GET_VERSION),
        "Sniffer device doesn't support ioctl interface");
    if (ioctl_ok)
        log_message("Sniffer ioctl version: %d", ioctl_version);
    if (ioctl_version >= IOCTL_TIMESTAMP_VERSION)
    {
        /* This API lets us set the FA entry count. */
        int current_count;
        ok = ok  &&
            TEST_IO(
                current_count = ioctl(
                    fa_sniffer, FASNIF_IOCTL_GET_ENTRY_COUNT)) &&
            IF_((unsigned int) current_count != fa_entry_count,
                /* If we need to change the entry count we need to close and
                 * reopen the sniffer handle to avoid getting mis-sized data. */
                TEST_IO_(ioctl(
                    fa_sniffer, FASNIF_IOCTL_SET_ENTRY_COUNT, &fa_entry_count),
                    "Unable to set sniffer count to %u", fa_entry_count) &&
                TEST_IO(close(fa_sniffer))  &&
                TEST_IO(fa_sniffer = open(fa_sniffer_device, O_RDONLY)));
    }
    else
        ok = ok  &&  TEST_OK_(fa_entry_count == 256, "Invalid FA entry count");
    return ok ? &sniffer_device : NULL;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Empty sniffer device, never delivers data, useful for read-only archiver. */

static bool reset_empty_sniffer(void) { return true; }
static bool read_empty_sniffer(
    struct fa_row *block, size_t block_size, uint64_t *timestamp)
{
    return false;
}
static bool status_empty_sniffer(struct fa_status *status)
{
    return FAIL_("No status for empty sniffer");
}
static bool interrupt_empty_sniffer(void) { return true; }

static const struct sniffer_context empty_sniffer = {
    .reset = reset_empty_sniffer,
    .read = read_empty_sniffer,
    .status = status_empty_sniffer,
    .interrupt = interrupt_empty_sniffer,
};

const struct sniffer_context *initialise_empty_sniffer(void)
{
    return &empty_sniffer;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


static pthread_t sniffer_id;

void configure_sniffer(
    struct buffer *buffer, const struct sniffer_context *sniffer)
{
    fa_block_buffer = buffer;
    sniffer_context = sniffer;
}

bool start_sniffer(bool boost_priority)
{
    pthread_attr_t attr;
    return
        TEST_0(pthread_attr_init(&attr))  &&
        IF_(boost_priority,
            /* If requested boost the thread priority and configure FIFO
             * scheduling to ensure that this thread gets absolute maximum
             * priority. */
            TEST_0(pthread_attr_setinheritsched(
                &attr, PTHREAD_EXPLICIT_SCHED))  &&
            TEST_0(pthread_attr_setschedpolicy(&attr, SCHED_FIFO))  &&
            TEST_0(pthread_attr_setschedparam(
                &attr, &(struct sched_param) { .sched_priority = 1 })))  &&
        TEST_0_(pthread_create(&sniffer_id, &attr, sniffer_thread, NULL),
            "Priority boosting requires real time thread support")  &&
        TEST_0(pthread_attr_destroy(&attr));
}

void terminate_sniffer(void)
{
    log_message("Waiting for sniffer...");
    pthread_cancel(sniffer_id);     // Ignore complaint if already halted
    ASSERT_0(pthread_join(sniffer_id, NULL));
    log_message("done");
}
