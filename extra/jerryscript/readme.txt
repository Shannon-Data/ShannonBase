jerryscript is an open source, ligthweight javascript engine, which can be run on resource-cosntrain platform.
you can download from (https://github.com/jerryscript-project/jerryscript).

How to use jerryscript in ShannonBase:
1) download the jerryscript source code from URL above.

2) build the static libs, cp the built static libs files to here.
2.1 `mkdir build && cd build`
2.2 `cmake .. -DCMAKE_BUILD_TYPE=Release -DJERRY_CMDLINE=OFF -DENABLE_LTO=OFF -DBUILD_SHARED_LIBS=OFF -DJERRY_ERROR_MESSAGES=ON -DCMAKE_INSTALL_PREFIX=/path/to/install`
2.3 `make -j5`
2.4 `make install`
3) update the libs in this folder, to get the latest version of jerryscript.
copy install dir files to proper position.

if you're on not linux, pls re-compile the code mannually, and use these libs.