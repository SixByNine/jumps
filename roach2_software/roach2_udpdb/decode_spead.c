#include "decode_spead.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>


/*
 * Decode relevent parameters from the SPEAD header. These 
 *
 * SPEAD headers are described here:
 * https://casper.astro.berkeley.edu/wiki/SPEAD
 *
 * Important to note that values are all 'big endian', i.e. most significant byte first.
 *
 * BUT - I suspect that the ROACH2 firmware we have does not actually implement this since it seems to conflict with the SPEAD
 * documentation - for example the heap counter 0x0001 always seems to be set to zero, whe this is supposed to be unique for each packet.
 *
 * This code is specifically written to deal with the data packets from the ROACH2 firmware used at JBO.
 *
 *       0x0001 should be heap counter, but seems to always be zero - can safely ignore
 *       0x0002 should be heap size, but seems to exclude the data section so more of the header size.
 *       0x0003 should be heap offset - can safely ignore
 *       0x0004 packet payload length - For us this is just the data size I think.
 *       0x1601 frame counter
 *       0x1700 'band select'
 *       0x1800 The actual data.
 *
 * The packets seem to have only one item stored outside of the header.
 *
 * If needed, some optimisation is possible by just assuming all packets look the same.
 *
 * band select seems to be a flag that specifies the mode of the ROACH firmware.
 *
 * Table from the old hebe2 code:
 *     Values (Band Sel / Bwidth(MHz) /i Heap Size / Frames per heap
 *     0/400/512/64, 2/350/518/74, 4/300/516/86, 6/250/512/103
 *     8/200/512/128, 10/150/513/171, 12/100/512/256, 14/50/512/512*
 *
 */
char* decode_roach2_spead_packet(unsigned char* heap, uint64_t* data_size, uint64_t* frame_counter, uint64_t* band_select) {

    const uint8_t magic = heap[0];

    if (magic != 0x53) {
        // this does not seem to be a SPEAD packet.
        return 0;
    }


    const unsigned item_width = 8; // we could read this from the SPEAD header as the sum of bytes 3 and 4, but it is constant.
    const unsigned number_of_items = (uint64_t)heap[7] | ((uint64_t)heap[6] << 8); // number of items in heap, 16 bit value, bytes 6 and 7.

    uint64_t data_offset = 0;

    // Scan through all the items in the header to get the ones we need. Probably the order is the same for all packets,
    // but if the overhead is not too great this seems a fine way to do it.
    for (unsigned item_counter = 0; item_counter < number_of_items; ++item_counter) {
        unsigned char const* item_pointer = heap + 8 + item_counter*item_width;
        // first bit is 'item mode'. We expect everything except for the data to be 1.
        unsigned char item_mode = (item_pointer[0]&0x80);

        // this is what item we have. 
        // 0x0001 should be heap counter, but seems to always be zero - can safely ignore
        // 0x0002 heap size. Not quite sure why this is smaller than the size of the packet, but maybe I don't understand it.
        // 0x0003 should be heap offset - can safely ignore
        // 0x0004 packet payload length - For us this is just the data size I think.
        // 0x1601 frame counter
        // 0x1700 'band select'
        // 0x1800 The actual data.
        uint64_t item_identifier =   (uint64_t)item_pointer[2] |
                                   ( (uint64_t)item_pointer[1]<<8) |
                                   (((uint64_t)item_pointer[0]&0x7f) << 16);

         uint64_t* item_content = 0;
        switch (item_identifier) {
            case 0x0004:
                item_content = data_size;
                    break;
            case 0x1601:
                item_content = frame_counter;
                    break;
            case 0x1700:
                item_content = band_select;
                break;
            case 0x1800:
                item_content = &data_offset;
                break;
        } // switch

        if (item_content) { // if we have an item to fill.

        // this is either the content of the packet or the data unpacked from 40-bits big endian
        *item_content = ((uint64_t)item_pointer[3] << 32) |
                        ((uint64_t)item_pointer[4] << 24) |
                        ((uint64_t)item_pointer[5] << 16) |
                        ((uint64_t)item_pointer[6] << 8)  |
                        ((uint64_t)item_pointer[7]     );
        } // if the item was useful

    } // for each item in header.

    assert(data_offset==0);

    // the data starts after the header, and after the item pointers, plus whatever offset is specified (probably zero).
    char* data_pointer = heap + 8 + number_of_items*item_width + data_offset;


    return data_pointer;
}


