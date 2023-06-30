#include "decode_spead.h"

// these definitely are:
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <sys/time.h>
#include <assert.h>

#include <multilog.h>

#define STRLEN 1024

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


    char ip_address[128];
    int portnum = 12000;
    char verbose = 0;
    char arg;
    struct timeval tv;

    double time_to_sample=10; // seconds

    unsigned char* packet_buffer = malloc(PACKET_BUFFER_SIZE);

    strncpy(ip_address,"10.0.3.1",128);


    multilog_t* log = multilog_open ("udp2db", 0); // dada logger

    multilog_add (log, stderr);

    multilog(log,LOG_DEBUG,"Debug verbosity\n");

    while ((arg = getopt(argc, argv, "p:vI:T:")) != -1) {
        switch (arg) {
            case 'v':
                verbose=1;
                break;
            case 'I':
                strncpy(ip_address,optarg,128);
                break;
            case 'p':
                sscanf(optarg,"%d",&portnum);
                break;
            case 'T':
                sscanf(optarg,"%lf",&time_to_sample);
                break;
        }
    }
    multilog(log,LOG_INFO,"Read approx %lf seconds of data\n",time_to_sample);


    double packets_per_second = 100e3; // approx 400MBps / 4kB packets..

    uint64_t packets_to_read = time_to_sample*packets_per_second;

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

    uint_fast8_t* byte_value_histogram = malloc(sizeof(uint_fast8_t)*256);
    memset(byte_value_histogram,0,sizeof(uint_fast8_t)*256);
    uint64_t frame_counter=0;
    uint64_t band_select=0;
    uint64_t data_size=0;
    char const* data_pointer=0;
    ssize_t size=0;


    multilog(log,LOG_INFO,"Collect %"PRIu64" packets\n",packets_to_read);
    for (uint64_t packet_counter = 0; packet_counter < packets_to_read; ++packet_counter) {

        size = recv(sock, (void*)packet_buffer,PACKET_BUFFER_SIZE,0);
        if (size == -1 ){
            if (errno==EAGAIN) {
                multilog(log,LOG_WARNING,"No packets recieved within 5 seconds... [ERRNO=%d '%s']\n",errno,strerror(errno));
                break;
            } else {
                multilog(log,LOG_ERR,"error getting packet ERRNO=%d %s\n",errno,strerror(errno));
                break;
            }
        }
        data_pointer = decode_roach2_spead_packet(packet_buffer, &data_size, &frame_counter, &band_select);
        for (uint64_t i =0; i < data_size; ++i){
            ++byte_value_histogram[(int)data_pointer[i]+128];
        }
    }

    printf("---\n");
    for (int i =-128; i < 127; ++i){
        printf("% 4d %"PRIdFAST8"\n",i,byte_value_histogram[i+128]);
    }

    FILE* dumpf = fopen("pkt.dmp","wb");
    fwrite(packet_buffer,1,size,dumpf);
    fclose(dumpf);
    dumpf = fopen("data.dmp","wb");

    fwrite(data_pointer,1,data_size,dumpf);
    fclose(dumpf);



    // free local memory
    free(packet_buffer);
    free(byte_value_histogram);

    return EXIT_SUCCESS;
}
