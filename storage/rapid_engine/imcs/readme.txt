1:IMCS- in-memory column store. which is a secondary engine to MySQL. It's used to process analytical workloads.
2:CU- column compressed unit. it stores all data of rows in column format, and in compressed format.
3:Chunk- A CU divide into lots of chunks, chunk the the smallest storage unit.

All of these has a corresponding header.
