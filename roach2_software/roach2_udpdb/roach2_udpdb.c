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

#include <dada_hdu.h>
#include <dada_def.h>


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

    // DADA header plus data
    dada_hdu_t* hdu;

    key_t dada_key = DADA_DEFAULT_BLOCK_KEY;

    // header parameter stuff
    char header_file[STRLEN];
    char obsid[STRLEN];
    char ip_address[128];
    int portnum = 12000;
    char verbose = 0;
    char arg;

    unsigned char* packet_buffer = malloc(PACKET_BUFFER_SIZE);

    strncpy(ip_address,"10.0.3.1",128);


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
                    fprintf(stderr, "dada_dbdisk: could not parse key from %s\n", optarg);
                    return -1;
                }
                break;
        }
    }



    fprintf(stderr, "dada key    : %x\n",dada_key);
    fprintf(stderr, "Listen IP   : %s\n",ip_address);
    fprintf(stderr, "Listen Port : %d\n",portnum);





    // Part 1. Initialise everything ...


    // Set up the listening socket address
    struct sockaddr_in socket_address;
    memset(&socket_address,0,sizeof(socket_address)); // default set to zero.
    socket_address.sin_family=AF_INET; // set IP
    socket_address.sin_addr.s_addr=inet_addr(ip_address); // set ip address to listen on
    socket_address.sin_port = htons(portnum); // set port to listen on

    // create and bind the socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0); // UDP/IP
    int ret = bind(sock, (struct sockaddr *) &socket_address, sizeof(socket_address));
    if (ret != 0) {
        fprintf(stderr,"error binding socket ERRNO=%d %s\n",errno,strerror(errno));
        return -1;
    }

    fprintf(stderr,"Socket bind ok\n");



    // 
    //@TODO Still to be implemented



    // Part 2. Wait for a frame counter reset to indicate synchronisation with 1PPS.
    char waiting=1;
    uint64_t wait_count=0;
    fprintf(stderr,"Waiting for frame counter reset...\n");
    while (waiting) {
        ssize_t size = recv(sock, (void*)packet_buffer,PACKET_BUFFER_SIZE,0);
        if (size == -1 ){
            if (errno==EAGAIN) {
                continue;
            } else {
                fprintf(stderr,"error getting packet ERRNO=%d %s\n",errno,strerror(errno));
            }
        }
        uint64_t frame_counter=0;
        uint64_t band_select=0;
        uint64_t data_size=0;
        unsigned char const* data_pointer = decode_roach2_spead_packet(packet_buffer, &data_size, &frame_counter, &band_select);
        if (frame_counter==0) { 
            fprintf(stderr,"wait count = %012"PRIu64"  frame counter %012"PRIu64"\n",wait_count,frame_counter);
            break;
        }

        if ((wait_count %100000) == 0) {

            fprintf(stderr,"wait count = %012"PRIu64"  frame counter %012"PRIu64"\n",wait_count,frame_counter);
        }

        ++wait_count;
    }

    
    // Part 3. Capture some data!
    fprintf(stderr,"Start reading data to dada_db buffer\n");

    //@TODO Still to be implemented



    // Part 4. Some cleanup when we are finished.



    free(packet_buffer);

}
