#include <inttypes.h>

unsigned char const* decode_roach2_spead_packet(unsigned char const* heap, uint64_t* data_size, uint64_t* frame_counter, uint64_t* band_select);