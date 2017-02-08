FROM debian:sid-slim
MAINTAINER Pete Markowsky <pete@markowsky.us>
RUN apt-get update
RUN apt-get install -y bash \
cmake \
curl \
g++ \
gcc \
git \
libboost-dev \
libcapstone-dev \
libz3-dev \
make \
python-dev
RUN curl https://bootstrap.pypa.io/get-pip.py | python
RUN cd /opt && curl -o pin.tgz -L http://software.intel.com/sites/landingpage/pintool/downloads/pin-2.14-71313-gcc.4.4.7-linux.tar.gz && tar zxf pin.tgz
RUN git clone https://github.com/JonathanSalwan/Triton.git && \ 
cd Triton \ 
&& mkdir build \ 
&& cd build \
&& cmake .. \
&& make -j2 install 

ENTRYPOINT ["/bin/bash"]
