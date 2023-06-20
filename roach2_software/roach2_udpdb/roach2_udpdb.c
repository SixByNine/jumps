/**
 *
 * roach2_udpdb
 * Michael Keith 2023
 *
 * This code performs the operation of copying data streaming from the ROACH2 in pulsar mode.
 *
 * Designed for the ROACH2 system in use at Jodrell Bank Observatory.
 *
 * The code uses a separate thread to read from the socket and fill an internal ring-buffer
 * and the main thread copies from the internal ring buffer into a psrdada buffer.
 *
 * The code uses the frame_counter in the SPEAD packet to keep track of where each data packet
 * should be stored.
 *
 * By default the code discards data until the frame counter resets to zero, which it assumes
 * happens exactly on the 1PPS. It will then set the start time in the header to the nearest
 * UTC second.
 *
 */


// define _GNU_SOURCE needed to enable some threading stuff
#define _GNU_SOURCE

#include "decode_spead.h"

// standard libraries
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

// threading
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>

//networking
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

// time
#include <sys/time.h>

// psrdada buffers
#include <dada_hdu.h>
#include <dada_def.h>
#include <multilog.h>
#include <futils.h>
#include <ascii_header.h>




#define STRLEN 1024
#define DADA_TIMESTR "%Y-%m-%d-%H:%M:%S"

// max size of packets that can be read.
#define PACKET_BUFFER_SIZE 4500
// number of packets in the internal buffer.
#define NUM_PACKET_BUFFERS 16000

typedef struct socket_thread_context_t {
    multilog_t* log; // psrdada thread-safe logger
    char ip_address[128]; // local IP address to listen on
    int portnum; // port to listen on
    int socket_listen_cpu_core; // CPU core on which to listen for packets.
    atomic_int buffer_write_position; // number of packets recieved.
    int buffer_read_position; // number of packets read.
    int number_of_overruns; // times that we have overrun the internal buffer
    unsigned char* buffer; // the internal buffer will be length PACKET_BUFFER_SIZE*NUM_PACKET_BUFFERS
} socket_thread_context_t;

void *socket_receive_thread(void* thread_context);
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

    // dada ringbuffer key
    key_t dada_key = DADA_DEFAULT_BLOCK_KEY;

    // header parameter stuff
    char* header_file = malloc(STRLEN);
    char* obsid = malloc(STRLEN);
    char* utc_start = malloc(STRLEN);


    // control parameters
    char force_start_without_1pps = 0;
    double requested_integration_time=300.0; // seconds.

    // for parsing arguments
    char arg;

    // structs for storing start and end time.
    struct timeval start_time;
    struct timeval end_time;


    // Set up logging...
    multilog_t* log = multilog_open ("udp2db", 0); // dada logger
    multilog_add (log, stderr);
    multilog(log,LOG_DEBUG,"Debug verbosity\n");

    // set up the context for the socket thread. this actually stores a bunch of useful state information.
    socket_thread_context_t* socket_thread_context = malloc(sizeof(socket_thread_context_t));
    memset(socket_thread_context,0,sizeof(socket_thread_context_t)); // initialise to zero.
    // allocate the internal ring buffer.
    socket_thread_context->buffer = malloc(PACKET_BUFFER_SIZE*NUM_PACKET_BUFFERS);
    socket_thread_context->log = log;

    // set a default value
    strncpy(socket_thread_context->ip_address,"10.0.3.1",128);


    while ((arg = getopt(argc, argv, "c:i:k:p:FH:I:T:")) != -1) {
        switch (arg) {
            case 'c':
                sscanf(optarg,"%d",&socket_thread_context->socket_listen_cpu_core);
                break;
            case 'T':
                sscanf(optarg,"%lf",&requested_integration_time);
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
    char* header_buf = ipcbuf_get_next_write (hdu->header_block);

    // read the header parameters from the file.
    if (fileread (header_file, header_buf, header_size) < 0)  {
        multilog (log, LOG_ERR, "Could not read header from %s\n", header_file);
        return EXIT_FAILURE;
    }

    // Part 1.1 start the socket rx thread...
    pthread_t socket_thread;
    pthread_create(&socket_thread,NULL, socket_receive_thread, socket_thread_context);

    // bind the socket rx thread to an appropriate core
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(socket_thread_context->socket_listen_cpu_core, &cpuset);
    pthread_setaffinity_np(socket_thread, sizeof(cpuset), &cpuset);


    // Part 2. Wait for a frame counter reset to indicate synchronisation with 1PPS.
    char waiting=1;
    uint64_t wait_count=0; // this stores how many packets we have waited for.

    // variables to store the packet contents.
    uint64_t frame_counter=0;
    uint64_t band_select=0;
    uint64_t data_size=0;
    char* data_pointer=0;
    // this helps track lost packets...
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

        if ((wait_count %100000) == 0 ){
            int64_t lost_packets = ((int64_t)frame_counter-(int64_t)expect_frame_count)/64;
            multilog(log,LOG_INFO,"Packets recieved: %07"PRIu64" (Rx %d Rd %d) Last frame counter: %012"PRIu64" Expected %012"PRIu64" Lost packets = %"PRIi64" (%.4lf%%) overruns %u\n",
                    wait_count,
                    socket_thread_context->buffer_write_position,
                    socket_thread_context->buffer_read_position,
                    frame_counter,
                    expect_frame_count,
lost_packets,100*(double)lost_packets/(double)wait_count,
                    socket_thread_context->number_of_overruns);
            if(force_start_without_1pps && (wait_count > 100000)) {
                // this allows us to force start without trigering for testing only.
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
        // Band_select=0 is the full 512MHz band.
        multilog(log,LOG_ERR,"Require band_select=0. Is set to %"PRIu64".\n");
        return EXIT_FAILURE;
    }

    // write the first data packet
    ipcio_write (hdu->data_block, data_pointer, data_size);

    const uint_fast32_t frame_increment = 64; // This is only true for band_select=0
    const uint64_t expect_data_size=4096; // This is true only for band_select=0

    double seconds_per_frame = 0.0625e-6; // 0.0625 microseconds.

    uint64_t block_start_frame_counter = frame_counter;


    uint_fast8_t holdover_packet = 1; // Set to 1 to ensure the frame=0 packet is written to the buffer

    // Part 3. Capture some data!
    //
    char* empty_data_pointer = malloc(expect_data_size);
    // @TODO: Decide what to use for empty packets. Here we copy the first packet, though this seems bad.
    memcpy(empty_data_pointer,data_pointer,data_size);

    // Not sure if there is any need to read integer number of blocks, but I guess it doesn't make much difference.
    uint64_t dropped_packets=0;
    uint64_t blocks_to_read = (requested_integration_time / seconds_per_frame)/frame_increment/packets_per_block+1;
    uint64_t packets_to_read = blocks_to_read*packets_per_block;
    uint64_t packets_read = 1;
    uint64_t nextblock = packets_per_block;

    multilog(log,LOG_INFO,"Packets to read %"PRIu64"\n",packets_to_read);

    while (packets_read < packets_to_read) {

        if (packets_read > nextblock) {
            multilog(log,LOG_INFO,"New block.  (Rx-Rd %d Overruns:%d) Dropped Packets so far  %"PRIu64"/%"PRIu64" %lf%%\n",socket_thread_context->buffer_write_position-socket_thread_context->buffer_read_position, socket_thread_context->number_of_overruns, dropped_packets,packets_read,100.0*(double)dropped_packets/(double)packets_read);

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



#define VLEN 16
void *socket_receive_thread(void* thread_context){
    socket_thread_context_t* context = (socket_thread_context_t*)thread_context;
    multilog_t* log = context->log;
    struct timeval tv;

    multilog(log, LOG_INFO, "bind to core %d\n", context->socket_listen_cpu_core);


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
    int size = 32 * 1024 * 1024; // 2MB
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, (socklen_t)sizeof(int));

    //    int disable = 1;
    //    setsockopt(sock, SOL_SOCKET, SO_NO_CHECK, (void*)&disable, sizeof(disable));


    // read and ignore a bunch of packets at the start
    for (unsigned i = 0; i < 100000 ; ++i ){
        unsigned char* packet_buffer = context->buffer + (context->buffer_write_position%NUM_PACKET_BUFFERS)*PACKET_BUFFER_SIZE;
        ssize_t size = recv(sock, (void*)packet_buffer,PACKET_BUFFER_SIZE,0);
    }



    // Can try using recvmmsg may be more efficient
 //       struct mmsghdr msgs[VLEN];
 //       struct iovec iovecs[VLEN];
    while(1) {
        // find the next location in the ring buffer
        unsigned char* packet_buffer = context->buffer + (context->buffer_write_position%NUM_PACKET_BUFFERS)*PACKET_BUFFER_SIZE;
        ssize_t retval = recv(sock, (void*)packet_buffer,PACKET_BUFFER_SIZE,0);
/*           memset(msgs, 0, sizeof(msgs));
           for (int i=0; i < VLEN; ++i){
           iovecs[i].iov_base         = context->buffer + ((context->buffer_write_position+i)%NUM_PACKET_BUFFERS)*PACKET_BUFFER_SIZE;
           iovecs[i].iov_len          = PACKET_BUFFER_SIZE;
           msgs[i].msg_hdr.msg_iov    = &iovecs[i];
           msgs[i].msg_hdr.msg_iovlen = 1;
           }
           int retval = recvmmsg(sock, msgs, VLEN, 0, &tv);
*/
        if (retval == -1 ){
            if (errno==EAGAIN) {
                multilog(log,LOG_WARNING,"No packets recieved within 5 seconds... [ERRNO=%d '%s']\n",errno,strerror(errno));
                continue;
            } else {
                multilog(log,LOG_ERR,"error getting packet ERRNO=%d %s\n",errno,strerror(errno));
                continue;
            }
        }
        //context->buffer_write_position += retval;
        ++(context->buffer_write_position);
    }

    return NULL;

}


unsigned char* get_next_packet_buffer(socket_thread_context_t* socket_thread_context){
    while (socket_thread_context->buffer_read_position >= socket_thread_context->buffer_write_position) {
        usleep(4); // wait 4 microseconds for a new packet.
    }

    int overrun = ((socket_thread_context->buffer_write_position)-(socket_thread_context->buffer_read_position))/NUM_PACKET_BUFFERS;
    if (overrun) {
        multilog(socket_thread_context->log,LOG_WARNING,"OVERRUN!!! %d - %d = %d\n",socket_thread_context->buffer_write_position,socket_thread_context->buffer_read_position,(socket_thread_context->buffer_write_position)-(socket_thread_context->buffer_read_position));
    }
    socket_thread_context->buffer_read_position += overrun * NUM_PACKET_BUFFERS;
    socket_thread_context->number_of_overruns += overrun;

    unsigned char* packet_buffer = socket_thread_context->buffer + (socket_thread_context->buffer_read_position%NUM_PACKET_BUFFERS)*PACKET_BUFFER_SIZE;
    ++(socket_thread_context->buffer_read_position);
    return packet_buffer;
}



