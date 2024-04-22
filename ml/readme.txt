For Machine Learing Libs. We will incorporate some ML libs for query optimization in MySQL.
(https://github.com/microsoft/LightGBM)

using this to compile LightGBM:
cmake .. -DBUILD_STATIC_LIB=ON -DCMAKE_INSTALL_PREFIX=../install -DUSE_MPI=ON -DCMAKE_BUILD_TYPE=Release