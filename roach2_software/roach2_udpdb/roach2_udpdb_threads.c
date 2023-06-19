#include "decode_spead.h"

// Are these needed:
//#include <sys/ioctl.h>
//#include <linux/if_packet.h>
//#include <net/if.h>
//#include <fcntl.h>
//#include <dirent.h>
//#include <signal.h>


// these definitely are:
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>
#include <stdatomic.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <sys/time.h>
#include <assert.h>

#include <dada_hdu.h>
#include <dada_def.h>
#include <multilog.h>
#include <futils.h>
#include <ascii_header.h>


#define STRLEN 1024
#define DADA_TIMESTR "%Y-%m-%d-%H:%M:%S"

#define PACKET_BUFFER_SIZE 4500
#define NUM_PACKET_BUFFERS 2000

typedef struct socket_thread_context_t {
    multilog_t* log;
    char ip_address[128];
    int portnum;
    atomic_uint buffer_write_position;
    unsigned int buffer_read_position;
    unsigned int number_of_overruns;
    unsigned char* buffer; // will be length PACKET_BUFFER_SIZE*NUM_PACKET_BUFFERS
} socket_thread_context_t;

void *socket_read_thread(void* thread_context);
unsigned char* get_next_packet_buffer(socket_thread_context_t* socket_thread_context);

//******
//
// Read packets from a port and wait for the frame counter to reset before
// copying them into a DADA buffer. Copy until specified number of seconds
// has elapsed.
//
//******

int main (int argc, char **argv)
{

    key_t dada_key = DADA_DEFAULT_BLOCK_KEY;

    // header parameter stuff
    char* header_file = malloc(STRLEN);
    char* obsid = malloc(STRLEN);
    char* utc_start = malloc(STRLEN);

    char* header_buf;
    char verbose = 0;
    char force_start_without_1pps = 0;
    char arg;
    struct timeval tv;
    struct timeval start_time;
    struct timeval end_time;


    multilog_t* log = multilog_open ("udp2db", 0); // dada logger

    multilog_add (log, stderr);

    multilog(log,LOG_DEBUG,"Debug verbosity\n");


    socket_thread_context_t* socket_thread_context = malloc(sizeof(socket_thread_context_t));
    memset(socket_thread_context,0,sizeof(socket_thread_context_t)); // initialise to zero.
    socket_thread_context->buffer = malloc(PACKET_BUFFER_SIZE*NUM_PACKET_BUFFERS);
    socket_thread_context->log = log;

    strncpy(socket_thread_context->ip_address,"10.0.3.1",128);


    
    //if (dada_bind_thread_to_core(0) < 0)
    //    multilog(log, LOG_WARNING, "receive_obs: failed to bind to core %d\n", 0);

    while ((arg = getopt(argc, argv, "i:k:p:vH:I:F")) != -1) {
        switch (arg) {
            case 'v':
                verbose=1;
                break;
            case 'H':
                strncpy(header_file,optarg,STRLEN);
                break;
            case 'I':
                strncpy(socket_thread_context->ip_address,optarg,128);
                break;
            case 'p':
                sscanf(optarg,"%d",&socket_thread_context->portnum);
                break;
            case 'F':
                force_start_without_1pps=1;
                break;
            case 'i':
                strncpy(obsid,optarg,STRLEN);
                break;
            case 'k':
                if (sscanf (optarg, "%x", &dada_key) != 1)
                {
                    multilog(log,LOG_ERR, "dada_dbdisk: could not parse key from %s\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
        }
    }


    // Part 1. Initialise everything ...

        // set up the dada stuff...

    dada_hdu_t* hdu = dada_hdu_create (log);
    multilog(log,LOG_DEBUG,"dada_hdu=%p\n",hdu);

    multilog(log,LOG_INFO, "dada key    : %x\n",dada_key);

    dada_hdu_set_key(hdu,dada_key);


    multilog(log,LOG_DEBUG,"Key set OK\n");
    fflush(stderr);

    if (dada_hdu_connect (hdu) < 0)
    {
        multilog(log,LOG_ERR,"Could not connect to dada hdu for key %x\n",dada_key);
        return EXIT_FAILURE;
    }  else {
        multilog(log,LOG_INFO, "Connected to dada hdu (%x)\n",dada_key);
    }

    if (dada_hdu_lock_write(hdu) < 0)
    {
        multilog(log,LOG_ERR,"Could not set write mode on dada hdu for key %x\n",dada_key);
        return EXIT_FAILURE;
    } else {
        multilog(log,LOG_INFO, "dada hdu set write mode ok (%x)\n",dada_key);
    }

    const uint64_t dada_block_size = ipcbuf_get_bufsz((ipcbuf_t*) hdu->data_block);

    multilog(log,LOG_INFO,"dada block size = %"PRIu64" bytes\n",dada_block_size);

    // Start to configure the header.
    uint64_t header_size = ipcbuf_get_bufsz (hdu->header_block);
    multilog(log, LOG_INFO, "header block size = %"PRIu64"\n", header_size);
    // Get the next header block to write to.
    header_buf = ipcbuf_get_next_write (hdu->header_block);

    // read the header into the file.
    if (fileread (header_file, header_buf, header_size) < 0)  {
        multilog (log, LOG_ERR, "Could not read header from %s\n", header_file);
        return EXIT_FAILURE;
    }


    // Part 1.1 start the socket reading thread...
    pthread_t socket_thread;
    pthread_create(&socket_thread,NULL, socket_read_thread, socket_thread_context);


    // Part 2. Wait for a frame counter reset to indicate synchronisation with 1PPS.
    char waiting=1;
    uint64_t wait_count=0;

    // variables to store the packet contents.
    uint64_t frame_counter=0;
    uint64_t band_select=0;
    uint64_t data_size=0;
    char const* data_pointer=0;
    uint64_t expect_frame_count=0;

    multilog(log,LOG_INFO,"Waiting for frame counter reset...\n");
    while (waiting) {
        // read from buffer

        unsigned char* packet_buffer = get_next_packet_buffer(socket_thread_context);
        data_pointer = decode_roach2_spead_packet(packet_buffer, &data_size, &frame_counter, &band_select);

        if (expect_frame_count == 0 ) expect_frame_count = frame_counter;
        else {
            expect_frame_count += 64;
        }
        
        if (frame_counter==0) { 
            break;
        }

        if ((wait_count %100000) == 0) {
            int64_t lost_packets = ((int64_t)frame_counter-(int64_t)expect_frame_count)/64;
            multilog(log,LOG_INFO,"Packets recieved: %07"PRIu64"  Last frame counter: %012"PRIu64" Expected %012"PRIu64" Lost packets = %"PRIi64" \n",wait_count,frame_counter,expect_frame_count,((int64_t)frame_counter-(int64_t)expect_frame_count)/64);
            if(wait_count > 400000 && force_start_without_1pps) {
                multilog(log,LOG_WARNING,"STARTING WITHOUT WAITING FOR 1PPS!!!!\n");
                break;
            }
        }

        ++wait_count;
    }

    // part 2.2 - set the start time and write the header to the dada buffer
    // We should have just started at the current UTC second.
    gettimeofday(&start_time, NULL);

    time_t rounded_start_time = start_time.tv_sec;
    double fractional_second = start_time.tv_usec/1e6;
    if (fractional_second > 0.5) {
        ++rounded_start_time; // round time up if we are above half a second.
        fractional_second -= 1.0;
    }

    multilog(log,LOG_INFO,"1PPS reset triggered at fractioal second %lfs\n",fractional_second);
    strftime(utc_start, STRLEN, DADA_TIMESTR, gmtime(&rounded_start_time));

    multilog(log,LOG_INFO,"UTC_START = %s\n",utc_start);

    /* write UTC_START to the header */
    if (ascii_header_set (header_buf, "UTC_START", "%s", utc_start) < 0) {
      multilog (log, LOG_ERR, "failed ascii_header_set UTC_START\n");
      return EXIT_FAILURE;
    }

    multilog (log, LOG_INFO, "UTC_START %s written to header\n", utc_start);


    // End of header writing. Mark header closed.
    if (ipcbuf_mark_filled (hdu->header_block, header_size) < 0)  {
        multilog (log, LOG_ERR, "Could not mark filled header block\n");
        return EXIT_FAILURE;
    }

    multilog(log, LOG_INFO, "BandSel %"PRIu64", Packet data size = %"PRIu64", dada block size = %"PRIu64"\n",band_select,data_size, dada_block_size);

    if (dada_block_size % data_size ) {
        multilog(log,LOG_ERR,"Require integer number of packets per block, but %"PRIu64"%"PRIu64"!=0.\n",dada_block_size,data_size);
        return EXIT_FAILURE;
    }
    const uint64_t packets_per_block = dada_block_size/data_size;

    multilog(log, LOG_INFO, "packets_per_block %"PRIu64"\n",packets_per_block);

    if (band_select!=0) {
        // @TODO: determinine if we ever will need to run with a narrower band. 
        // Band_select=0 is the full 400MHz band.
        multilog(log,LOG_ERR,"Require band_select=0. Is set to %"PRIu64".\n");
        return EXIT_FAILURE;
    }




    //write the first data packet
    ipcio_write (hdu->data_block, data_pointer, data_size);


    const uint_fast32_t frame_increment = 64; // This is only true for band_select=0
    const uint64_t expect_data_size=4096; // This is true only for band_select=0
    const uint64_t frame_increment_per_block = frame_increment * packets_per_block;

    uint64_t block_start_frame_counter = frame_counter;


    uint_fast8_t holdover_packet = 1; // Set to 1 to ensure the frame=0 packet is written to the buffer

    // Part 3. Capture some data!
    //
    char* empty_data_pointer = malloc(expect_data_size);
    memset(empty_data_pointer,0,expect_data_size);
    memcpy(empty_data_pointer,data_pointer,data_size); // use the first packet as filler... not good but whatever for now

    uint64_t dropped_packets=0;
    uint64_t packets_to_read = packets_per_block * 900;
    uint64_t packets_read = 1;
    uint64_t nextblock = packets_per_block;

    multilog(log,LOG_INFO,"Packets to read %"PRIu64"\n",packets_to_read);

    while (packets_read < packets_to_read) {

        if (packets_read > nextblock) {
            multilog(log,LOG_INFO,"New block. Dropped Packets so far  %"PRIu64"/%"PRIu64" %lf%%\n",dropped_packets,packets_read,100.0*(double)dropped_packets/(double)packets_read);

            nextblock += packets_per_block;
        }

        // get next packet
        unsigned char* packet_buffer = get_next_packet_buffer(socket_thread_context);
        uint64_t expected_frame_counter = frame_counter + frame_increment;
        data_pointer = decode_roach2_spead_packet(packet_buffer, &data_size, &frame_counter, &band_select);
        assert(band_select==0);
        assert(data_size==expect_data_size);

        // multilog(log,LOG_DEBUG," >> %"PRIu64" > %"PRIu64" >> %"PRIu64"\n",packets_to_read,packets_read,frame_counter);

        // Logic to decide if the packet is what we wanted or if we need to do something else.
        if (frame_counter > expected_frame_counter) {
            dropped_packets += (frame_counter - expected_frame_counter ) /frame_increment;
            //multilog(log,LOG_DEBUG," >> %"PRIu64" > %"PRIu64" >> %"PRIu64" packets_read=%"PRIu64"\n",frame_counter,frame_counter-expected_frame_counter,frame_increment,packets_read);
            for (uint64_t i = expected_frame_counter; i < frame_counter; i+=frame_increment) {
                // write some fake data packets where they were dropped.
                ipcio_write (hdu->data_block, packet_buffer, data_size);
            }
            packets_read += (frame_counter - expected_frame_counter ) /frame_increment;
        }
        if (frame_counter < expected_frame_counter) {
            multilog(log,LOG_WARNING,"out of sequence frame counter %"PRIu64" expected %"PRIu64"\n",frame_counter,expected_frame_counter);
            continue;
        }

        // copy the contents of this packet.
        ipcio_write (hdu->data_block, data_pointer, data_size);
        ++packets_read; // decrement packets remaining counter

    }

    gettimeofday(&end_time, NULL);

    double runtime = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_usec - start_time.tv_usec)/1e6;
    multilog(log,LOG_INFO,"Finished. Sent %"PRIu64" packets in %lf s. Total packets dropped: %"PRIu64", %lf%%\n",packets_read,runtime,dropped_packets,100.0*(double)dropped_packets/(double)packets_read);


    


    // Part 4. Some cleanup when we are finished.
    //
    // unlock write access from the HDU, performs implicit EOD
    if (dada_hdu_unlock_write (hdu) < 0) {
        multilog (log, LOG_ERR, "dada_hdu_unlock_write failed\n");
        return EXIT_FAILURE;
    }

    // disconnect from HDU
    if (dada_hdu_disconnect (hdu) < 0) {
        multilog (log, LOG_ERR, "could not unlock write on hdu\n");
    }

    // free local memory
    free(utc_start);
    free(header_file);
    free(obsid);

    return EXIT_SUCCESS;
}




void *socket_read_thread(void* thread_context){
    socket_thread_context_t* context = (socket_thread_context_t*)thread_context;
    multilog_t* log = context->log;
    struct timeval tv;

    // Set up the listening socket address
    multilog(log,LOG_INFO, "Listen IP   : %s\n",context->ip_address);
    multilog(log,LOG_INFO, "Listen Port : %d\n",context->portnum);

    struct sockaddr_in socket_address;
    memset(&socket_address,0,sizeof(socket_address)); // default set to zero.
    socket_address.sin_family=AF_INET; // set IP
    socket_address.sin_addr.s_addr=inet_addr(context->ip_address); // set ip address to listen on
    socket_address.sin_port = htons(context->portnum); // set port to listen on

    // create and bind the socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0); // UDP/IP
    int ret = bind(sock, (struct sockaddr *) &socket_address, sizeof(socket_address));
    if (ret != 0) {
        multilog(log,LOG_ERR,"error binding socket ERRNO=%d %s\n",errno,strerror(errno));
        return NULL;
    }

    multilog(log,LOG_INFO,"Socket bind ok\n");


    // set a socket timeout to detect no packets comming
    tv.tv_sec = 5; // 5 second
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);


    while(1) {
        // find the next location in the ring buffer
        unsigned char* packet_buffer = context->buffer + (context->buffer_write_position%NUM_PACKET_BUFFERS)*PACKET_BUFFER_SIZE;
        ssize_t size = recv(sock, (void*)packet_buffer,PACKET_BUFFER_SIZE,0);
        if (size == -1 ){
            if (errno==EAGAIN) {
                multilog(log,LOG_WARNING,"No packets recieved within 5 seconds... [ERRNO=%d '%s']\n",errno,strerror(errno));
                continue;
            } else {
                multilog(log,LOG_ERR,"error getting packet ERRNO=%d %s\n",errno,strerror(errno));
                continue;
            }
        }
        ++(context->buffer_write_position);
    }

    return NULL;

}


unsigned char* get_next_packet_buffer(socket_thread_context_t* socket_thread_context){
    while (socket_thread_context->buffer_read_position >= socket_thread_context->buffer_write_position) {
        usleep(4); // wait 4 microseconds for a new packet.
    }

    int overrun = (socket_thread_context->buffer_read_position-socket_thread_context->buffer_write_position)/NUM_PACKET_BUFFERS;
    socket_thread_context->buffer_write_position += overrun * NUM_PACKET_BUFFERS;
    socket_thread_context->number_of_overruns += overrun;
    unsigned char* packet_buffer = socket_thread_context->buffer + (socket_thread_context->buffer_read_position%NUM_PACKET_BUFFERS)*PACKET_BUFFER_SIZE;
    ++(socket_thread_context->buffer_read_position);
    return packet_buffer;
}
