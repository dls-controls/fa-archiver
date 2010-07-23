/* Filter mask routines. */

/* The filter mask is used to specify a list of PVs.  The syntax of a filter
 * mask can be written as:
 *
 *      mask = id [ "-" id ] [ "," mask]
 *
 * Here each id identifies a particular BPM and must be a number in the range
 * 0 to 255 and id1-id2 identifies an inclusive range of BPMs.
 */


#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "error.h"
#include "sniffer.h"

#include "mask.h"


#define WRITE_BUFFER_SIZE       (1 << 16)


int count_mask_bits(filter_mask_t mask)
{
    int count = 0;
    for (int bit = 0; bit < FA_ENTRY_COUNT; bit ++)
        if (test_mask_bit(mask, bit))
            count ++;
    return count;
}


int format_mask(filter_mask_t mask, char *buffer)
{
    for (int i = sizeof(filter_mask_t) / 4; i > 0; i --)
        buffer += sprintf(buffer, "%08x", mask[i - 1]);
    return (sizeof(filter_mask_t) / 4) * 8;
}

void print_mask(FILE *out, filter_mask_t mask)
{
    char buffer[2 * sizeof(filter_mask_t) + 1];
    fwrite(buffer, format_mask(mask, buffer), 1, out);
}



static bool read_id(const char *original, const char **string, int *id)
{
    char *next;
    *id = strtol(*string, &next, 0);
    return
        TEST_OK_(next > *string,
            "Number missing at \"%s\" (+%d)", original, *string - original)  &&
        DO_(*string = next)  &&
        TEST_OK_(0 <= *id  &&  *id < FA_ENTRY_COUNT, "id %d out of range", *id);
}


bool parse_mask(const char *string, filter_mask_t mask)
{
    const char *original = string;    // Just for error reporting
    memset(mask, 0, sizeof(filter_mask_t));

    bool ok = true;
    while (ok)
    {
        int id;
        ok = read_id(original, &string, &id);
        if (ok)
        {
            if (*string == '-')
            {
                string ++;
                int end;
                ok = read_id(original, &string, &end)  &&
                    TEST_OK_(id <= end, "Range %d-%d is empty", id, end);
                for (int i = id; ok  &&  i <= end; i ++)
                    set_mask_bit(mask, i);
            }
            else
                set_mask_bit(mask, id);
        }

        if (*string != ',')
            break;
        string ++;
    }

    return
        ok  &&
        TEST_OK_(*string == '\0',
            "Unexpected characters at \"%s\" (+%d)",
            original, string - original);
}


int copy_frame(void *to, void *from, filter_mask_t mask)
{
    int32_t *from_p = from;
    int32_t *to_p = to;
    int copied = 0;
    for (size_t i = 0; i < sizeof(filter_mask_t) / 4; i ++)
    {
        uint32_t m = mask[i];
        for (int j = 0; j < 32; j ++)
        {
            if ((m >> j) & 1)
            {
                *to_p++ = from_p[0];
                *to_p++ = from_p[1];
                copied += 8;
            }
            from_p += 2;
        }
    }
    return copied;
}


bool write_frames(int file, filter_mask_t mask, void *frame, int count)
{
    int out_frame_size = count_mask_bits(mask) * FA_ENTRY_SIZE;
    while (count > 0)
    {
        char buffer[WRITE_BUFFER_SIZE];
        size_t buffered = 0;
        while (count > 0  &&  buffered + out_frame_size <= WRITE_BUFFER_SIZE)
        {
            copy_frame(buffer + buffered, frame, mask);
            frame = (char *) frame + FA_FRAME_SIZE;
            buffered += out_frame_size;
            count -= 1;
        }

        size_t written = 0;
        while (buffered > 0)
        {
            size_t wr;
            if (!TEST_IO(wr = write(file, buffer + written, buffered)))
                return false;
            written += wr;
            buffered -= wr;
        }
    }
    return true;
}
