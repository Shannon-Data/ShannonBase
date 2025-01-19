For Machine Learing Libs. We will incorporate some ML libs for query optimization in MySQL.
(https://github.com/microsoft/LightGBM)

HOW TO USE LightGBM:
0) to down a copy of LightGBM to your workshop, then start to build your own static lib files.

1) to build a static libs, using this command to compile LightGBM (with the latest version if needed):
cmake .. -DBUILD_STATIC_LIB=ON  -DUSE_MPI=OFF -DUSE_OPENMP=OFF -DCMAKE_INSTALL_PREFIX=../install -DCMAKE_BUILD_TYPE=Release

2) replace the .a files with the new compiled static lib files.

3) re-compile ShannonBase with these static lib files.