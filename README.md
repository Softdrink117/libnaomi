Naomi Toolchain & Libraries
===========================

A minimal Naomi homebrew environment, very loosely based off of KallistiOS toolchain work but bare metal. This assumes that you have a Linux computer with standard prerequisites for compiling gcc/newlib/binutils already installed. The Naomi system library is minimal, but will continue to get fleshed out. There is currently no support for pthreads or accelerated video though I hope to fix that as I continue to improve libnaomi.

To get started, create a directory named "/opt/toolchains/naomi" and copy the contents of the `setup/` directory to it. This directory and the copied contents should be user-owned and user-writeable. Then, cd to "/opt/toolchains/naomi" and in order run `./download.sh` (downloads toolchain sources), `./unpack.sh` (unpacks the toolchain to be built), `make` (builds the toolchain and installs it in the correct directories), `make gdb` (builds gdb for SH-4 only) and finally `./cleanup.sh`. If everything is successful, you should have a working compiler and C standard library.

After this, you will need to set up a python virtualenv for the various build utility dependencies. To do that, run `make pyenv` from the `homebrew/` directory. This should create a virtualenv in a place where the various tools can find it and install the correct dependencies so that everything will work. Note that this does assume you have a working python3 3.6+ installation with pip and venv packages available. Once this is done, you are ready to start compiling libnaomi as well as the tests and examples!

The next thing you will need to do is build libnaomi, the system support library that includes the C/C++ runtime setup, newlib system hooks and various low-level drivers. To do that, run `make` from inside the `libnaomi/` directory. If a `libnaomi.a` file is created this means that the toolchain is set up properly and the system library was successfully built! If you receive error messages about "Command not found", you have not activated your Naomi enviornment by running `source /opt/toolchains/naomi/env.sh`. Then, you will want to run `make` from inside the `libnaomi/message/` directory to build the message library which is required for one of the examples to build. Similarly, if a `libnaomimessage.a` file is created that means you built it successfully! Finally, to build any examples that are included, first activate the Naomi enviornment by running `source /opt/toolchains/naomi/env.sh`, and then run `make` in the directory of the example you want to run. The resulting binary file can be loaded in Demul or netbooted to a Naomi with a netdimm.

For convenience the python virtualenv, libnaomi and the examples will all be built if you run `make` in the `homebrew/` directory. That means that you can skip all of the steps in the above two paragraphs assuming that your toolchain setup is working. Note that by default there are no 3rd party libraries installed and thus libnaomi support for things like font rendering is disabled. To enable 3rd party libraries, first run `make -C 3rdparty install` in the `homebrew/` directory which will fetch, configure, make and install all of the 3rd party libraries. Then, back in the `homebrew/` directory run `make clean` and then re-run `make` to rebuild libnaomi with 3rd party support.

Once you are satisfied that the toolchain is working and all of the examples are building, you can install libnaomi into the toolchain path so that you can reference it from your own compiled code. To do that, run `make install` in the `homebrew/` directory. This will blow away and remake the python virtualenv for you and install all the proper dependencies needed, copy the libraries and header files, copy the ldscripts and build utilities such as makerom, and finally copy a `Makefile.base` that you can source to get rid of a lot of the boilerplate of makefiles. If all goes well then you should be able to cd into the `homebrew/minimal/` directory and run `make` there. You should base your own build setup off of the makefile found in this directory. Remember to source `/opt/toolchains/naomi/env.sh` before building so that your makefile can find `Makefile.base` and everything else that comes along with it.

For ease of tracking down program bugs, an exception handler is present which prints out the system registers, stack address and PC at the point of exception. For further convenience, debugging information is left in an elf file that resides in the build/ directory of an example you might be building. To locate the offending line of code when an exception is displayed, you can run `/opt/toolchains/naomi/sh-elf/bin/sh-elf-addr2line --exe=build/naomi.elf <displayed PC address>` and the function and line of code where the exception occurred will be displayed for you. Additionally, there is GDB remote debugging support allowing you to attach to a program running on the Naomi and step through as well as debug the program. To debug your program, first activate the GDB server by running `/opt/toolchains/naomi/tools/gdbserver` and then run GDB with `/opt/toolchains/naomi/sh-elf/bin/sh-elf-gdb build/naomi.elf`. To attach to the target once GDB is running and has read symbols from your compiled program, type `target remote :2345`. If all is successful, your program will halt and you can examine your program in real-time on the target.

Homebrew is also welcome to make use of additional facilities that allow for redirecting stdout/stderr to the host console for debugging. Notably, the test executable that is generated out of the `tests/` directory will do this. To intercept such messages, you can run `/opt/toolchains/naomi/tools/stdioredirect` and it will handle displaying anything that the target is sending to stdout or stderr using printf(...) or fprintf(stderr, ...) calls. Note that the homebrew program must link against libnaomimessage.a and initialize the console redirect hook for this to work properly. If GDB debugging is too advanced for you this might be adequate for debugging your program when it is running on target.

For the minimal hello world example which can be compiled from its own repository outside of this one, see the `minimal/` directory. It contains a makefile that works with this toolchain if properly installed (using `make install` at the root) and sourced (running `source /opt/toolchains/naomi/env.sh`) in the shell you are compiling in. The included c file is extremely minimal and only illustrates the most basic text output. However, you can use this as the baseline for any new project you are starting by copying the files to a new directory and changing things as needed.

If you are looking for a great resource for programming, the first thing I would recommend is https://github.com/Kochise/dreamcast-docs which is mostly relevant to the Naomi. For memory maps and general low-level stuff, Mame's https://github.com/mamedev/mame/blob/master/src/mame/drivers/naomi.cpp is extremely valuable. Also the headers for various libnaomi modules contain descriptions for how to use the functions found within. And of course, you can look at the source code to the various examples to see some actual code.
