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

#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

 #include <sys/time.h>

#include <dada_hdu.h>
#include <dada_def.h>
#include <multilog.h>
#include <futils.h>
#include <ascii_header.h>


#define STRLEN 1024
#define DADA_TIMESTR "%Y-%m-%d-%H:%M:%S"

#define PACKET_BUFFER_SIZE 8192


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
    char ip_address[128];
    int portnum = 12000;
    char verbose = 0;
    char arg;
    struct timeval tv;

    unsigned char* packet_buffer = malloc(PACKET_BUFFER_SIZE);

    strncpy(ip_address,"10.0.3.1",128);


    multilog_t* log = multilog_open ("udp2db", 0); // dada logger

    multilog_add (log, stderr);

    multilog(log,LOG_DEBUG,"Debug verbosity\n");

    while ((arg = getopt(argc, argv, "i:k:p:vH:I:")) != -1) {
        switch (arg) {
            case 'v':
                verbose=1;
                break;
            case 'H':
                strncpy(header_file,optarg,STRLEN);
                break;
            case 'I':
                strncpy(ip_address,optarg,128);
                break;
            case 'p':
                sscanf(optarg,"%d",&portnum);
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


    // Set up the listening socket address
    multilog(log,LOG_INFO, "Listen IP   : %s\n",ip_address);
    multilog(log,LOG_INFO, "Listen Port : %d\n",portnum);

    struct sockaddr_in socket_address;
    memset(&socket_address,0,sizeof(socket_address)); // default set to zero.
    socket_address.sin_family=AF_INET; // set IP
    socket_address.sin_addr.s_addr=inet_addr(ip_address); // set ip address to listen on
    socket_address.sin_port = htons(portnum); // set port to listen on

    // create and bind the socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0); // UDP/IP
    int ret = bind(sock, (struct sockaddr *) &socket_address, sizeof(socket_address));
    if (ret != 0) {
        multilog(log,LOG_ERR,"error binding socket ERRNO=%d %s\n",errno,strerror(errno));
        return EXIT_FAILURE;
    }

    multilog(log,LOG_INFO,"Socket bind ok\n");


    // set a socket timeout to detect no packets comming
    tv.tv_sec = 5; // 5 second
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);



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





    // Part 2. Wait for a frame counter reset to indicate synchronisation with 1PPS.
    char waiting=1;
    uint64_t wait_count=0;

    // variables to store the packet contents.
    uint64_t frame_counter=0;
    uint64_t band_select=0;
    uint64_t data_size=0;
    unsigned char const* data_pointer=0;

    multilog(log,LOG_INFO,"Waiting for frame counter reset...\n");
    while (waiting) {
        ssize_t size = recv(sock, (void*)packet_buffer,PACKET_BUFFER_SIZE,0);
        if (size == -1 ){
            if (errno==EAGAIN) {
                multilog(log,LOG_WARNING,"No packets recieved within 5 seconds... [ERRNO=%d '%s']\n",errno,strerror(errno));
                continue;
            } else {
                multilog(log,LOG_ERR,"error getting packet ERRNO=%d %s\n",errno,strerror(errno));
            }
        }
        data_pointer = decode_roach2_spead_packet(packet_buffer, &data_size, &frame_counter, &band_select);
        if (frame_counter==0) { 
            (stderr,"wait count = %012"PRIu64"  frame counter %012"PRIu64"\n",wait_count,frame_counter);
            break;
        }

        if ((wait_count %100000) == 0) {
            multilog(log,LOG_INFO,"Packets recieved: %07"PRIu64"  Last frame counter: %012"PRIu64"\n",wait_count,frame_counter);
        }

        ++wait_count;
    }

    // part 2.2 - set the start time and write the header to the dada buffer
    // We should have just started at the current UTC second.
    gettimeofday(&tv, NULL);

    time_t rounded_start_time = tv.tv_sec;
    double fractional_second = tv.tv_usec/1e6;
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

    multilog(log, LOG_INFO, "BandSel %"PRIu64", Packet data size = %"PRIu64", dada block size = %"PRIu64"\n",band_select,data_size, dada_block_size);

    if (dada_block_size % data_size ) {
        multilog(log,LOG_ERR,"Require integer number of packets per block, but %"PRIu64"%"PRIu64"!=0.\n",dada_block_size,data_size);
        return EXIT_FAILURE;
    }
    const uint64_t packets_per_block = dada_block_size/data_size;

    // Part 3. Capture some data!

    // Part 4. Some cleanup when we are finished.

    free(packet_buffer);
    free(utc_start);
    free(header_file);
    free(obsid);

}
