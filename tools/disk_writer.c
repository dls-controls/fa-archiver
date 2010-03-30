/* Writes buffer to disk. */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>

#include "error.h"
#include "buffer.h"
#include "disk.h"

#include "disk_writer.h"


static pthread_t writer_id;

static int disk_fd;                 // File descriptor of archive file
static off64_t data_start, data_size;


static struct disk_header header;
static struct disk_header *header_mmap;



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Block recording.                                                          */


static off64_t write_offset = 0;    // Where we are in the archive
static off64_t old_write_offset;
static int max_backlog = 0;


/* The step from old_write_offset to write_offset defines an interval for
 * which we will force the expiry of inter-block gaps.  Each block is
 * described as a half open interval [start, stop), and in all cases we can
 * guarantee that the start is no earlier than old_write_offset, so we want
 * to ensure we expire all intervals with end no later than the new
 * write_offset. */
static bool expired(off64_t offset)
{
    if (write_offset >= old_write_offset)
        /* Normal case, current write pointer and previous write pointer are
         * together. */
        return old_write_offset < offset  &&  offset <= write_offset;
    else
        /* Current write pointer has wrapped around since last flush. */
        return offset <= write_offset  ||  old_write_offset < offset;
}

/* Flushes old archive blocks and updates the end pointer of the oldest block
 * so that it is valid. */
static void expire_archive_blocks(void)
{
    /* Expire all older blocks that have completely fallen off. */
    while (header.h.block_count > 1  &&
           expired(header.blocks[header.h.block_count - 1].stop_offset))
        header.h.block_count -= 1;
    
    /* If the start of the oldest block has expired then bring it forward. */
    off64_t *old_start =
        &header.blocks[header.h.block_count - 1].start_offset;
    if (expired(*old_start)  ||  *old_start == old_write_offset)
        *old_start = write_offset;
    old_write_offset = write_offset;
}


static uint64_t get_now(void)
{
    struct timespec ts;
    ASSERT_IO(clock_gettime(CLOCK_REALTIME, &ts));
    return (unsigned) ts.tv_sec;
}


/* Updates the file header with the record of a new gap. */
static void start_archive_block(void)
{
    /* Very simple approach, simply push all the existing blocks down one and
     * record our new block at the start. */
    memmove(&header.blocks[1], &header.blocks[0],
        (MAX_HEADER_BLOCKS - 1) * sizeof(struct block_record));
    header.h.block_count += 1;
    if (header.h.block_count > MAX_HEADER_BLOCKS)
        header.h.block_count = MAX_HEADER_BLOCKS;

    uint64_t now = get_now();
    header.blocks[0].start_sec = now;
    header.blocks[0].stop_sec = now;
    header.blocks[0].start_offset = write_offset;
    header.blocks[0].stop_offset = -1;     // Will be overwritten!

    header.h.disk_status = 1;  // writing
}


static void update_backlog(int backlog)
{
    if (backlog > max_backlog)
        max_backlog = backlog;
}


static void write_header(void)
{
    struct flock flock = {
        .l_type = F_WRLCK, .l_whence = SEEK_SET,
        .l_start = 0, .l_len = sizeof(header)
    };

    ASSERT_IO(fcntl(disk_fd, F_SETLKW, &flock));
    memcpy(header_mmap, &header, DISK_HEADER_SIZE);
    ASSERT_IO(msync(header_mmap, DISK_HEADER_SIZE, MS_ASYNC));
        
    flock.l_type = F_UNLCK;
    ASSERT_IO(fcntl(disk_fd, F_SETLK, &flock));
}


/* Update header timestamp and write it out if the timestamp has changed.. */
static void update_header(bool force_write)
{
    expire_archive_blocks();
    uint64_t now = get_now();
    if (force_write  ||  now != header.blocks[0].stop_sec)
    {
        header.h.write_backlog = max_backlog;
        header.blocks[0].stop_sec = now;
        header.blocks[0].stop_offset = write_offset;
        max_backlog = 0;
        write_header();
    }
}




/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Disk writing thread.                                                      */

static struct reader_state *reader;
static bool writer_running;


static void * get_valid_read_block(struct reader_state *reader, bool archiving)
{
    int backlog;
    void *block = get_read_block(reader, &backlog);
    update_backlog(backlog);
    if (block == NULL)
    {
        /* No data to read.  If we are archiving at this point we'll insert a
         * break in the data record and then start a new archive block. */
        
        if (archiving)
            /* The next read may take some time, ensure the header is up to
             * date while we're waiting. */
            update_header(true);

        /* Ensure we leave with a valid read block in hand. */
        do {
            block = get_read_block(reader, &backlog);
            update_backlog(backlog);
        } while (writer_running && block == NULL);

        if (writer_running && archiving)
            start_archive_block();
    }
    return block;
}


static void * writer_thread(void *context)
{
    /* Start by getting the initial data block, ignoring any initial gap.
     * Start a fresh archive block at this point. */
    void *block = get_valid_read_block(reader, false);
    start_archive_block();
    ASSERT_IO(lseek(disk_fd, data_start + write_offset, SEEK_SET));
    
    while (writer_running)
    {
        ASSERT_write(disk_fd, block, fa_block_size);
        release_read_block(reader);
        
        write_offset += fa_block_size;
        if (write_offset >= data_size)
        {
            write_offset = 0;
            ASSERT_IO(lseek(disk_fd, data_start, SEEK_SET));
        }
        update_header(false);

        /* Go and get the next block to be written. */
        block = get_valid_read_block(reader, true);
    }
    
    return NULL;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Disk writing initialisation and startup.                                  */


static bool process_header(int write_buffer)
{
    uint64_t disk_size;
    bool ok =
        TEST_IO(
            header_mmap = mmap(0, DISK_HEADER_SIZE,
                PROT_READ | PROT_WRITE, MAP_SHARED, disk_fd, 0))  &&
        get_filesize(disk_fd, &disk_size)  &&
        validate_header(header_mmap, disk_size);
    if (ok)
    {
        memcpy(&header, header_mmap, DISK_HEADER_SIZE);
        if (header.h.block_count > 0)
            write_offset = header.blocks[0].stop_offset;
        else
            write_offset = 0;
        old_write_offset = write_offset;
        data_size = header.h.data_size;
        data_start = header.h.data_start;

        header.h.write_buffer = write_buffer;
    }
    return ok;
}

static void close_header(void)
{
    header.h.disk_status = 0;
    update_header(true);
}


bool initialise_disk_writer(const char *disk, int write_buffer)
{
    reader = open_reader(true);
    writer_running = true;
    
    return
        TEST_IO_(
            disk_fd = open(disk, O_RDWR | O_DIRECT | O_LARGEFILE),
            "Unable to open archive file \"%s\"", disk)  &&
        process_header(write_buffer)  &&
        TEST_0(pthread_create(&writer_id, NULL, writer_thread, NULL));
}

void terminate_disk_writer(void)
{
    printf("Waiting for writer\n");
    writer_running = false;
    stop_reader(reader);
    ASSERT_0(pthread_join(writer_id, NULL));
    close_reader(reader);
    close_header();
    
    printf("done\n");
}