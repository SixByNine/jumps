FROM debian:10-slim

WORKDIR /workspace
COPY ./pulchan_r2_2019_Sep_25_1507.fpg /workspace/pulchan_r2_2019_Sep_25_1507.fpg
COPY ./LaunchPulsarTimer.py /workspace/LaunchPulsarTimer.py
COPY ./StopPulsarTimer.py /workspace/StopPulsarTimer.py
COPY ./CheckPulsarTimer.py /workspace/CheckPulsarTimer.py
COPY ./SyncOn1PPS.py /workspace/SyncOn1PPS.py

RUN apt-get update && apt-get install -y git \
    build-essential \
    cmake \
    libboost-system1.67-dev \
    libboost-thread1.67-dev \
    libboost-program-options1.67-dev \
    libboost-filesystem1.67-dev \
    libhdf5-dev \
    python2.7 \
    python-pip \
    python-numpy \
    vim

RUN pip install katversion==0.9
RUN pip install katcp==0.6.2

WORKDIR /workspace
RUN git clone https://github.com/ska-sa/casperfpga.git
WORKDIR /workspace/casperfpga
RUN git checkout v0.4.3
RUN python setup.py install

WORKDIR /workspace
RUN git clone --recursive https://github.com/ska-sa/katcp_devel.git
WORKDIR /workspace/katcp_devel
# Change to KATCP v5.0
RUN git checkout version-5.0
RUN make
RUN make -C katcp install

WORKDIR /workspace
RUN git clone --recursive https://github.com/ska-sa/RoachAcquisitionServer.git
WORKDIR /workspace/RoachAcquisitionServer

RUN cmake .
RUN make
