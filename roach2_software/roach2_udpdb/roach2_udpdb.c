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
 *
 * To avoid packet drops, make sure to set
 * sysctl -w net.core.rmem_max=26214400
 *
 */


// define _GNU_SOURCE needed to enable some threading stuff
#define _GNU_SOURCE

#include "decode_spead.h"
#include "default_header.h"

// standard libraries
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>

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



#define MAX(a,b) (((a)>(b))?(a):(b))


#define STRLEN 1024
#define DADA_TIMESTR "%Y-%m-%d-%H:%M:%S"

// max size of packets that can be read.
#define PACKET_BUFFER_SIZE 4500
// number of packets in the internal buffer.
#define NUM_PACKET_BUFFERS 16000

typedef struct local_context_t {
    multilog_t* log; // psrdada thread-safe logger
    char ip_address[128]; // local IP address to listen on
    int portnum; // port to listen on
    int socket_listen_cpu_core; // CPU core on which to listen for packets.
    atomic_int_fast64_t buffer_write_position; // number of packets recieved.
    int_fast64_t buffer_read_position; // number of packets read.
    int number_of_overruns; // times that we have overrun the internal buffer
    unsigned char* buffer; // the internal buffer will be length PACKET_BUFFER_SIZE*NUM_PACKET_BUFFERS

    // monitor variables
    int64_t packet_count; int64_t dropped_packets;
    int64_t block_count; int64_t packets_to_read; double seconds_per_packet;
    int64_t buffer_lag; int64_t max_buffer_lag; int64_t recent_buffer_lag;
} local_context_t;

void *socket_receive_thread(void* thread_context);
unsigned char* get_next_packet_buffer(local_context_t* local_context);
unsigned char* get_random_packet_buffer(local_context_t* local_context);


char* monitor_string; // Global seems the best way :(
void monitor(int monitor_fd, char* state,local_context_t* context);

int band_select_to_frames_per_heap(uint64_t band_select);
int band_select_to_data_size(uint64_t band_select);

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
    char* header_file = 0;
    char* source_name = malloc(STRLEN);
    char* telescope_id = malloc(STRLEN);
    char* receiver_name = malloc(STRLEN);
    char* receiver_basis = malloc(STRLEN);
    char* utc_start = malloc(STRLEN);
    double centre_frequency = 1532.0; // this is wrong, but will be updated later
    double bandwidth = -256.0;
    strncpy(receiver_basis,"Circular",STRLEN);
    strncpy(receiver_name,"Unknown",STRLEN);
    strncpy(telescope_id,"",STRLEN);
    // control parameters
    char force_start_without_1pps = 0;
    double requested_integration_time=300.0; // seconds.
    char* control_fifo = NULL;
    char* monitor_fifo = NULL;
    monitor_string = malloc(STRLEN); // allocate memory for the monitor string

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
    local_context_t* local_context = malloc(sizeof(local_context_t));
    memset(local_context,0,sizeof(local_context_t)); // initialise to zero.
    // allocate the internal ring buffer.
    local_context->buffer = malloc(PACKET_BUFFER_SIZE*NUM_PACKET_BUFFERS);
    local_context->log = log;

    // set a default value
    strncpy(local_context->ip_address,"10.0.3.1",128);


    while ((arg = getopt(argc, argv, "b:c:f:i:k:lp:r:s:t:C:FH:I:M:T:")) != -1) {
        switch (arg) {
            case 'f': // centre frequency
                sscanf(optarg,"%lf",&centre_frequency);
                break;
            case 'b': // bandwidth
                sscanf(optarg,"%lf",&bandwidth);
                break;
            case 'c':
                sscanf(optarg,"%d",&local_context->socket_listen_cpu_core);
                break;
            case 't':
                strncpy(telescope_id,optarg,STRLEN);
                break;
            case 's':
                strncpy(source_name,optarg,STRLEN);
                break;
            case 'r':
                strncpy(receiver_name,optarg,STRLEN);
                break;
            case 'l':
                strncpy(receiver_basis,"Linear",STRLEN);
                break;
            case 'C':
                control_fifo = malloc(strlen(optarg)+1);
                strncpy(control_fifo, optarg,strlen(optarg)+1);
                break;
            case 'M':
                monitor_fifo = malloc(strlen(optarg)+1);
                strncpy(monitor_fifo, optarg,strlen(optarg)+1);
                break;
            case 'T':
                sscanf(optarg,"%lf",&requested_integration_time);
                break;
            case 'H':
                header_file=malloc(strlen(optarg)+1);
                memcpy(header_file,optarg,strlen(optarg)+1);
                break;
            case 'I':
                strncpy(local_context->ip_address,optarg,128);
                break;
            case 'p':
                sscanf(optarg,"%d",&local_context->portnum);
                break;
            case 'F':
                force_start_without_1pps=1;
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
    // open monitor and control pipes

    int monitor_fd=-1;
    if (monitor_fifo!=NULL){
        monitor_fd = open(monitor_fifo,O_NONBLOCK|O_WRONLY);
        if (monitor_fd < 0){
        multilog(log,LOG_ERR,"opening monitor pipe '%s' errno=%d %s\n",monitor_fifo,errno,strerror(errno));
        }
    }
    int control_fd=-1;
    if (control_fifo!=NULL){
        control_fd = open(control_fifo,O_NONBLOCK|O_RDONLY);
        if (control_fd < 0){
            multilog(log,LOG_ERR,"opening control pipe '%s' errno=%d %s\n",control_fifo,errno,strerror(errno));
        }

    }



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

    if (header_file != 0) {
        // read the header parameters from the file.
        if (fileread (header_file, header_buf, header_size) < 0)  {
            multilog (log, LOG_ERR, "Could not read header from %s\n", header_file);
            return EXIT_FAILURE;
        }
    } else {
        // use hardcoded default file
        if (default_header_ascii_len > header_size){
            multilog (log, LOG_ERR, "Header block size too small for default header parameters! %d bytes < %d bytes\n", header_size,default_header_ascii_len);
            return EXIT_FAILURE;
        }
        memset(header_buf,0,header_size);
        memcpy(header_buf,default_header_ascii,default_header_ascii_len);
    }

    if (ascii_header_set (header_buf, "FREQ", "%.8lf", centre_frequency) < 0) {
        multilog (log, LOG_ERR, "failed ascii_header_set FREQ\n");
        return EXIT_FAILURE;
    }

    if (ascii_header_set (header_buf, "BW", "%.8lf", bandwidth) < 0) {
        multilog (log, LOG_ERR, "failed ascii_header_set BW\n");
        return EXIT_FAILURE;
    }
    if (ascii_header_set (header_buf, "SOURCE", "%s", source_name) < 0) {
        multilog (log, LOG_ERR, "failed ascii_header_set SOURCE\n");
        return EXIT_FAILURE;
    }
    if (ascii_header_set (header_buf, "RECEIVER", "%s", receiver_name) < 0) {
        multilog (log, LOG_ERR, "failed ascii_header_set RECEIVER\n");
        return EXIT_FAILURE;
    }

    if (ascii_header_set (header_buf, "BASIS", "%s", receiver_basis) < 0) {
        multilog (log, LOG_ERR, "failed ascii_header_set BASIS\n");
        return EXIT_FAILURE;
    }


    // Part 1.1 start the socket rx thread...
    pthread_t socket_thread;
    pthread_create(&socket_thread,NULL, socket_receive_thread, local_context);

    // bind the socket rx thread to an appropriate core
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(local_context->socket_listen_cpu_core, &cpuset);
    pthread_setaffinity_np(socket_thread, sizeof(cpuset), &cpuset);


    // Part 2. Wait for a frame counter reset to indicate synchronisation with 1PPS.

    // variables to store the packet contents.
    uint64_t frame_counter=0;
    uint64_t band_select=0;
    uint64_t data_size=0;
    char* data_pointer=0;
    // this helps track lost packets...
    uint64_t expected_frame_counter=0;

    multilog(log,LOG_INFO,"Waiting for frame counter reset...\n");
    while (1) {
        // read from buffer
        unsigned char* packet_buffer = get_next_packet_buffer(local_context);
        data_pointer = decode_roach2_spead_packet(packet_buffer, &data_size, &frame_counter, &band_select);
        if(data_pointer==0){
            multilog(log,LOG_WARNING,"Invalid packet recieved\n");
            continue;
        }

        uint64_t frame_increment = band_select_to_frames_per_heap(band_select);

        if (frame_counter==0) {
            // this is what we were waiting for! break out of this look and start working.
            break;
        }

        if (expected_frame_counter == 0 ) expected_frame_counter = frame_counter;

        if (frame_counter > expected_frame_counter) {
            local_context->dropped_packets += (frame_counter - expected_frame_counter ) / frame_increment;
        }

        if ((local_context->packet_count %100000) == 0 ){
            monitor(monitor_fd, "WAITING", local_context);
            multilog(log,LOG_INFO,"Waiting for 1PPS. lag: % 3d max_lag: % 3d block_lag: % 3d overruns: %d packet_loss: %"PRId64"/%"PRId64" (%lg%%)\n",
                    local_context->buffer_lag,
                    local_context->max_buffer_lag, 
                    local_context->recent_buffer_lag,
                    local_context->number_of_overruns, 
                    local_context->dropped_packets,
                    local_context->packet_count,
                    100.0*(double)(local_context->dropped_packets)/(double)(local_context->packet_count));
            local_context->recent_buffer_lag = 0;

            if(force_start_without_1pps && (local_context->packet_count > 100000)) {
                // this allows us to force start without trigering for testing only.
                multilog(log,LOG_WARNING,"STARTING WITHOUT WAITING FOR 1PPS!!!!\n");
                break;
            }
        }

        ++(local_context->packet_count); // increment packet counter
        expected_frame_counter += frame_increment; // expect the next frame

    }

    // reset appropriate counters...
    local_context->max_buffer_lag    = 0;
    local_context->recent_buffer_lag = 0;
    local_context->number_of_overruns= 0;
    local_context->dropped_packets   = 0;
    local_context->packet_count    = 0;

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



    const uint_fast32_t frame_increment = band_select_to_frames_per_heap(band_select);
    const uint64_t expected_data_size     = band_select_to_data_size(band_select);
    const uint64_t packets_per_block = dada_block_size/expected_data_size;
    double seconds_per_frame = 0.0625e-6; // 0.0625 microseconds.
    local_context->seconds_per_packet = seconds_per_frame*frame_increment;

    multilog(log, LOG_INFO, "BandSel %"PRIu64", Packet data size = %"PRIu64", dada block size = %"PRIu64"\n",band_select,data_size, dada_block_size);

    if (data_size != expected_data_size) {
        multilog (log, LOG_ERR, "packet data size does not match expected data size %"PRIu64"%!="PRIu64"\n",data_size,expected_data_size);
        return EXIT_FAILURE;
    }

    if (dada_block_size % expected_data_size ) {
        multilog(log,LOG_ERR,"Require integer number of packets per block, but %"PRIu64"%"PRIu64"!=0.\n",dada_block_size,expected_data_size);
        return EXIT_FAILURE;
    }

    // @TODO: set frequency parameters in header


    // ....
    // ....
    // ....
    // ....


    // End of header writing. Mark header closed.
    if (ipcbuf_mark_filled (hdu->header_block, header_size) < 0)  {
        multilog (log, LOG_ERR, "Could not mark filled header block\n");
        return EXIT_FAILURE;
    }


    multilog(log, LOG_INFO, "packets_per_block %"PRIu64"\n",packets_per_block);


    // Part 3. Capture some data!

    // Not sure if there is any need to read integer number of blocks, but I guess it doesn't make much difference.
    uint64_t blocks_to_read = (requested_integration_time / seconds_per_frame)/frame_increment/packets_per_block+1;
    local_context->packets_to_read = blocks_to_read*packets_per_block;
    uint64_t nextblock = packets_per_block;


    // write the first data packet to the dada buffer.
    ipcio_write (hdu->data_block, data_pointer, data_size);

    // set up for the next frame.
    expected_frame_counter = frame_counter + frame_increment;
    local_context->packet_count = 1;

    multilog(log,LOG_INFO,"Packets to read %"PRIu64"\n",local_context->packets_to_read);

    while (local_context->packet_count < local_context->packets_to_read) {

        if (local_context->packet_count > nextblock) {
            monitor(monitor_fd, "RUNNING", local_context);
            multilog(log,LOG_INFO,"New block. lag: % 3d max_lag: % 3d block_lag: % 3d overruns: %d packet_loss: %"PRId64"/%"PRId64" (%lg%%)\n",
                    local_context->buffer_lag,
                    local_context->max_buffer_lag, 
                    local_context->recent_buffer_lag,
                    local_context->number_of_overruns, 
                    local_context->dropped_packets,
                    local_context->packet_count,
                    100.0*(double)(local_context->dropped_packets)/(double)(local_context->packet_count));
            local_context->recent_buffer_lag = 0;
            nextblock += packets_per_block;
        }

        // get next packet
        unsigned char* packet_buffer = get_next_packet_buffer(local_context);
        data_pointer = decode_roach2_spead_packet(packet_buffer, &data_size, &frame_counter, &band_select);
        if(data_pointer==0){
            multilog(log,LOG_WARNING,"Invalid packet recieved\n");
            continue;
        }

        assert(band_select==0);
        assert(data_size==expected_data_size);

        // multilog(log,LOG_DEBUG," >> %"PRIu64" > %"PRIu64" >> %"PRIu64"\n",packets_to_read,local_context->packet_count,frame_counter);

        // Logic to decide if the packet is what we wanted or if we need to do something else.
        if (frame_counter > expected_frame_counter) {
            uint64_t ndropped = (frame_counter - expected_frame_counter ) /frame_increment;
            local_context->dropped_packets += ndropped;
            for (uint64_t i = 0 ; i < ndropped; ++i) {
                // write some fake data packets where they were dropped.
                // we grab a random packet from the buffer to use as data.
                // be careful not to overwrite any important variables for the actual packet we are working on!
                unsigned char* junk_packet_buffer = get_random_packet_buffer(local_context);
                uint64_t junk_data_size,junk_frame_counter,junk_band_select;
                char* junk_data_pointer = decode_roach2_spead_packet(junk_packet_buffer, &junk_data_size, &junk_frame_counter, &junk_band_select);
                assert(data_size==expected_data_size);
                ipcio_write (hdu->data_block, junk_data_pointer, junk_data_size);
            }
            multilog(log,LOG_WARNING,"Injected %d randomly sampled packets... %"PRIu64"/%"PRIu64"\n",ndropped,frame_counter,expected_frame_counter);
            local_context->packet_count += ndropped;
            expected_frame_counter = frame_counter; // we caught up the frames, set the expected frame counter to the current one...
        }
        if (frame_counter < expected_frame_counter) {
            // @TODO: How do we actually handle out of sequence packets?
            if (frame_counter == 0){
                // we must have re-set the frame counter.
                multilog(log,LOG_ERR,"Unexpected frame counter reset. Timing integrity lost. Aborting observation\n");
                break;
            } else {
                multilog(log,LOG_WARNING,"Discarding out of sequence packet. frame counter %"PRIu64" expected %"PRIu64"\n",frame_counter,expected_frame_counter);
                continue;
            }
        }

        // copy the contents of this packet.
        ipcio_write (hdu->data_block, data_pointer, data_size);
        ++(local_context->packet_count); // increment packet counter
        expected_frame_counter += frame_increment; // expect the next frame

    }

    gettimeofday(&end_time, NULL);

    double runtime = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_usec - start_time.tv_usec)/1e6;
    multilog(log,LOG_INFO,"Finished. Sent %"PRIu64" packets in %lf s. Total packets dropped: %"PRIu64", %lf%%\n",local_context->packet_count,runtime,local_context->dropped_packets,100.0*(double)local_context->dropped_packets/(double)local_context->packet_count);

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
    free(source_name);
    free(telescope_id);
    free(receiver_name);
    free(receiver_basis);
    free(utc_start);
    if (header_file != 0) {
        free(header_file);
    }
    free(monitor_string);

    return EXIT_SUCCESS;
}



#define VLEN 16
void *socket_receive_thread(void* thread_context){
    local_context_t* context = (local_context_t*)thread_context;
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


unsigned char* get_next_packet_buffer(local_context_t* local_context){
    while (local_context->buffer_read_position >= local_context->buffer_write_position) {
        usleep(4); // wait 4 microseconds for a new packet.
    }

    // monitoring stuff to check max buffer lag
    local_context->buffer_lag = (local_context->buffer_write_position)-(local_context->buffer_read_position);
    local_context->max_buffer_lag = MAX(local_context->buffer_lag,local_context->max_buffer_lag); // MAX macro
    local_context->recent_buffer_lag = MAX(local_context->buffer_lag,local_context->recent_buffer_lag);

    int overrun = local_context->buffer_lag/NUM_PACKET_BUFFERS;

    if (overrun) {
        multilog(local_context->log,LOG_WARNING,"OVERRUN!!! %"PRIdFAST64" - %"PRIdFAST64" = %d\n",local_context->buffer_write_position,local_context->buffer_read_position,(local_context->buffer_write_position)-(local_context->buffer_read_position));
    }

    local_context->buffer_read_position += overrun * NUM_PACKET_BUFFERS;
    local_context->number_of_overruns += overrun;

    unsigned char* packet_buffer = local_context->buffer + (local_context->buffer_read_position%NUM_PACKET_BUFFERS)*PACKET_BUFFER_SIZE;
    ++(local_context->buffer_read_position);
    return packet_buffer;
}


unsigned char* get_random_packet_buffer(local_context_t* local_context){
    int64_t random_bufpos = rand()%NUM_PACKET_BUFFERS;
    unsigned char* packet_buffer = local_context->buffer + random_bufpos*PACKET_BUFFER_SIZE;
    return packet_buffer;
}


void monitor(int monitor_fd, char* state, local_context_t* context){
    if (monitor_fd > 0) {
        // fill string
        snprintf(monitor_string, STRLEN, "%s %"PRId64" %"PRId64" %"PRId64" %"PRId64" %lf %"PRId64" %"PRId64" %"PRId64" %"PRId64" %d\n",
                state,
                context->packet_count, context->dropped_packets,
                context->block_count,context->packets_to_read, context->seconds_per_packet,
                context->buffer_lag, context->max_buffer_lag, context->recent_buffer_lag,
                context->number_of_overruns,NUM_PACKET_BUFFERS);
        monitor_string[STRLEN-1]='\0';
        // write string
        write(monitor_fd,monitor_string,strlen(monitor_string));
    }
}


int band_select_to_frames_per_heap(uint64_t band_select) {
    //https://drive.google.com/file/d/1Dcp3hzQ37FaQsrmJCuuU-ry1TO9biQ90/view?usp=sharing
    switch (band_select){
        case 0:
            return 64;
        case 2:
            return 74;
        case 4:
            return 86;
        case 6:
            return 103;
        case 8:
            return 128;
        case 10:
            return 171;
        case 12:
            return 256;
        case 14:
            return 512;
        default:
            return -1;
    }
}
int band_select_to_data_size(uint64_t band_select) {
    int words_per_frame = 8-band_select/2;
    return band_select_to_frames_per_heap(band_select) * words_per_frame*8; // 
}
