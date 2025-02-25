For Machine Learing Libs. We will incorporate some ML libs for query optimization in MySQL.
(https://github.com/microsoft/LightGBM)

1) to build a static libs, using this command to compile LightGBM (with the latest version if needed):
cmake .. -DBUILD_STATIC_LIB=ON  -DUSE_MPI=OFF -DUSE_OPENMP=OFF -DCMAKE_INSTALL_PREFIX=../install -DCMAKE_BUILD_TYPE=Release

Now, compiling with mysql cmake.
