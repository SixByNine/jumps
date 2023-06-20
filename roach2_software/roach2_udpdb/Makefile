PSRDADA = /opt/psr_dev

# Compiling flags   
CFLAGS = -g -O2 -I$(PSRDADA)/include/ # The dada_hdu.h file is in this directory

# Linking flags                                                               
LFLAGS = $(PSRDADA)/lib/libpsrdada.so -lm -lpthread -L/usr/local/cuda/lib64 -lcudart -L/usr/lib64 -lstdc++

# Compiler                                                                       
CC = gcc

all: roach2_udpdb roach2_udpstats

roach2_udpdb: roach2_udpdb.o decode_spead.o
	$(CC) -o roach2_udpdb roach2_udpdb.o decode_spead.o $(LFLAGS)

roach2_udpstats: roach2_udpstats.o decode_spead.o
	$(CC) -o roach2_udpstats roach2_udpstats.o decode_spead.o $(LFLAGS)


clean:
	rm *.o
