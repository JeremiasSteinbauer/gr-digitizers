
# Building

In a nutshell we have three main dependencies:
 - GNU Radio (boost, fftw, and some other libraries are required via dependency tree)
 - ROOT
 - PicoScope

GNU Radio and ROOT could be compiled/linked statically and required libraries could be in principle copied
and deployed together with the test deploy unit. The only problem is PicoScope library (well libraries)
which needs to be installed into the /opt/picoscope directory.


## Building gnuradio from source (static build, dynamic boost)

See [https://wiki.gnuradio.org/index.php/BuildGuide](https://wiki.gnuradio.org/index.php/BuildGuide).

Note, install location should be '/opt/gnuradio', and it should be statically build. This assumption
is made by FESA level code.

Steps:

git clone --recursive https://github.com/gnuradio/gnuradio.git


Comment out the following lines in `<top>/volk/apps/CMakeLists.txt`:

```shell
  set_target_properties(volk_profile PROPERTIES LINK_FLAGS "-static")
  set_target_properties(volk-config-info PROPERTIES LINK_FLAGS "-static")
```

```shell
cmake .. -DCMAKE_INSTALL_PREFIX=/opt/gnuradio \
	-DENABLE_GR_ANALOG=ON \
	-DENABLE_STATIC_LIBS=ON
```

Dependencies:

sudo yum install fftw-devel


# Building gr-digitizers module

Download and install PicoScope drivers from here: [https://www.picotech.com/downloads/linux](https://www.picotech.com/downloads/linux).

In order to build the gr-digitizers module please follow the following steps:

```shell
$ cd gr-digitizers
$ mkdir build
$ cd build
$ cmake .. -DCMAKE_PREFIX_PATH=/opt/gnuradio \
           -DCMAKE_INSTALL_PREFIX=/opt/gnuradio \
           -DENABLE_GR_LOG=1 \
           -DENABLE_STATIC_LIBS=ON \
           -DCMAKE_BUILD_TYPE=Debug
```
Note the cmake variables used above can be omitted if the standard GNU Radio installation is used. In the
example above it is assumed that the GNU Radio is istalled in the `/opt/gnuradio` directory.

To install the module execute the following command:

```shell
$ sudo make install
```
Note, depending on the system configuration the following two configuration files might be required:

```shell
$ cat ~/.gnuradio/config.conf 
[grc]
local_blocks_path=/opt/gnuradio/share/gnuradio/grc/blocks

$ cat /etc/ld.so.conf.d/gnuradio.conf 
/opt/gnuradio/lib64

$ cat ~/.bashrc 

<some output ommited>

PYTHONPATH=$PYTHONPATH:/usr/local/lib64/python2.7/site-packages/
export PYTHONPATH

```
When changing the loader configuration, don't forget to run `sudo ldconfig` command afterwards.

# Run unit tests

From the build directory execute the following command to run the unit tests:

```shell
$ ctest
```

By default only unit tests wihtout any HW dependencies are executed. In order to execute PicoScope 3000a
related tests execute the below command (from you build directory):

```shell
$ lib/test-digitizers --enable-ps3000a-tests
```

Note, HW related tests cannot be run in parallel.

# Examples

See gr-digitizers/examples/grc directory.


# Dependencies

## ROOT Framework

```shell
$ sudo yum install root-*
```
On CentOS 7 (cmake version 2.8.12), the CMake find_package command does not set needed ROOT components correctly,
namely MathCore and Graf, but rather pulls in all the libraries. In order to overcome this, uncomment the following 
two lines in the `gr-digitizers/lib/CMakeLists.txt` file:

```
    #/usr/lib64/root/libMathCore.so
    #/usr/lib64/root/libGraf.so
```
And comment out the following line:

```
    ${ROOT_LIBRARIES}
```

## Picoscope Drivers

Available from here: [https://www.picotech.com/downloads/linux](https://www.picotech.com/downloads/linux).

# Known Bugs

1) In rapid block mode acquisition is automatically started if no trigger is received for more than ~1 hour. This seems
   to be HW (or driver) related bug (relavant for 3000 series devices only).

