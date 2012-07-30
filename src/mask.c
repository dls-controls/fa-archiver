/* Filter mask routines.
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
#include <stdarg.h>
#include <unistd.h>
#include <string.h>

#include "error.h"
#include "fa_sniffer.h"
#include "parse.h"

#include "mask.h"


unsigned int count_mask_bits(
    const struct filter_mask *mask, unsigned int fa_entry_count)
{
    unsigned int count = 0;
    for (unsigned int bit = 0; bit < fa_entry_count; bit ++)
        if (test_mask_bit(mask, bit))
            count ++;
    return count;
}


unsigned int format_raw_mask(
    const struct filter_mask *mask, unsigned int fa_entry_count, char *buffer)
{
    for (unsigned int i = fa_entry_count / 8; i > 0; i --)
        buffer += sprintf(buffer, "%02X", mask->mask[i - 1]);
    return 4 * fa_entry_count;
}



static bool parse_id(
    const char **string, unsigned int fa_entry_count, unsigned int *id)
{
    return
        parse_uint(string, id)  &&
        TEST_OK_(*id < fa_entry_count, "id %u out of range", *id);
}


/* Parses a mask in the form generated by format_raw_mask(), namely a sequence
 * of hex digits. */
static bool parse_raw_mask(
    const char **string, unsigned int fa_entry_count, struct filter_mask *mask)
{
    unsigned int count = fa_entry_count / 4;        // 4 bits per nibble
    for (unsigned int i = count; i > 0; )
    {
        i -= 1;
        unsigned int ch = *(*string)++;
        unsigned int nibble;
        if ('0' <= ch  &&  ch <= '9')
            nibble = ch - '0';
        else if ('A' <= ch  &&  ch <= 'F')
            nibble = ch - 'A' + 10;
        else
            return FAIL_("Unexpected character in mask");
        // 2 nibbles per byte
        mask->mask[i / 2] |= (uint8_t) (nibble << (4 * (i % 2)));
    }
    return true;
}


bool parse_mask(
    const char **string, unsigned int fa_entry_count, struct filter_mask *mask)
{
    memset(mask->mask, 0, sizeof(mask->mask));

    if (read_char(string, 'R'))
        return parse_raw_mask(string, fa_entry_count, mask);
    else
    {
        bool ok = true;
        do {
            unsigned int id;
            ok = parse_id(string, fa_entry_count, &id);
            if (ok)
            {
                unsigned int end_id = id;
                if (read_char(string, '-'))
                    ok =
                        parse_id(string, fa_entry_count, &end_id)  &&
                        TEST_OK_(id <= end_id,
                            "Range %d-%d is empty", id, end_id);
                for (unsigned int i = id; ok  &&  i <= end_id; i ++)
                    set_mask_bit(mask, i);
            }
        } while (ok  &&  read_char(string, ','));

        return ok;
    }
}


/* Support functions for format_mask() to help safely write values into a
 * string. */
static bool write_string(char **string, size_t *length, const char *value)
{
    size_t value_len = strlen(value);
    if (value_len + 1 < *length)
    {
        memcpy(*string, value, value_len + 1);
        *string += value_len;
        *length -= value_len;
        return true;
    }
    else
        return false;
}

static bool write_uint(char **string, size_t *length, unsigned int value)
{
    char buffer[24];
    sprintf(buffer, "%d", value);
    return write_string(string, length, buffer);
}

static bool write_range(
    char **string, size_t *length,
    unsigned int start, unsigned int end, bool first)
{
    return
        IF_(!first, write_string(string, length, ","))  &&
        write_uint(string, length, start)  &&
        IF_(end > start,
            write_string(string, length, "-")  &&
            write_uint(string, length, end));
}

bool format_readable_mask(
    const struct filter_mask *mask, unsigned int fa_entry_count,
    char *string, size_t length)
{
    bool ok = true;
    bool in_range = false;
    bool first = true;
    unsigned int range_start = 0;
    *string = '\0';
    for (unsigned int id = 0; ok  &&  id < fa_entry_count; id ++)
    {
        bool set = test_mask_bit(mask, id);
        if (set  &&  !in_range)
        {
            /* Starting a new range of values.  Write the first number. */
            in_range = true;
            range_start = id;
        }
        else if (!set  &&  in_range)
        {
            /* End of range, now write it out. */
            ok = write_range(&string, &length, range_start, id - 1, first);
            in_range = false;
            first = false;
        }
    }
    if (ok  &&  in_range)
        ok = write_range(
            &string, &length, range_start, fa_entry_count - 1, first);
    return ok;
}


unsigned int format_mask(
    const struct filter_mask *mask, unsigned int fa_entry_count, char *buffer)
{
    if (format_readable_mask(mask, fa_entry_count, buffer, fa_entry_count / 4))
        return (unsigned int) strlen(buffer);
    else
    {
        buffer[0] = 'R';
        return format_raw_mask(mask, fa_entry_count, buffer + 1);
    }
}
