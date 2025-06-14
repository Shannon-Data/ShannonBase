![image](./Docs/shannon-logo.png)

![nightly](https://github.com/Shannon-Data/ShannonBase/actions/workflows/nightly.yaml/badge.svg)
![weekly](https://github.com/Shannon-Data/ShannonBase/actions/workflows/weekly.yaml/badge.svg)

ShannonBase is a HTAP database provided by Shannon Data AI, which is an infra for big data & AI. 

ShannonBase: The Next-Gen Database for AI—an infrastructure designed for big data and AI. As the MySQL of the AI era, ShannonBase extends MySQL with native embedding support, machine learning capabilities, a JavaScript engine, and a columnar storage engine. These enhancements empower ShannonBase to serve as a powerful data processing and Generative AI infrastructure.

Firstly, ShannonBase incorporates a columnar store, IMCS (In-Memory Column Store), named Rapid, to transform it into a MySQL HTAP (Hybrid Transactional/Analytical Processing) database. Transactional and analytical workloads are intelligently offloaded to either InnoDB or Rapid using a combination of cost-based and ML-based algorithms. Additionally, version linking is introduced in IMCS to support MVCC (Multi-Version Concurrency Control). Changes in InnoDB are automatically and synchronously propagated to Rapid by applying Redo logs.

Secondly, ShannonBase supports multimodal data types, including structured, semi-structured, and unstructured data, such as GIS, JSON, and Vector.

Thirdly, ShannonBase natively supports LightGBM or XGBoost (TBD), allowing users to perform training and prediction directly via stored procedures, such as ml_train, ml_predict_row, ml_model_import, etc.—eliminating the need for ETL (exporting data and importing trained ML models). Alternatively, pre-trained models can be imported into ShannonBase to save training time. Classification, Regression, Recommendation, Abnormal detection, etc. supported.

Fourthly, By leveraging embedding algorithms and vector data type, ShannonBase becomes a powerful ML/RAG tool for ML/AI data scientists. With Zero Data Movement, Native Performance Optimization, and Seamless SQL Integration, ShannonBase is easy to use, making it an essential hands-on tool for data scientists and ML/AI developers.

At last, ShannonBase Multilingual Engine Component. ShannonBase includes a lightweight JavaScript engine, JerryScript, allowing users to write stored procedures in either SQL or JavaScript.


## Getting Started with ShannonBase:
### Compilation, Installation and Start ShannonBase
#### 1: Folk or clone the repo.
```
git clone --recursive git@github.com:Shannon-Data/ShannonBase.git
```
PS: You should ensure that your prerequisite development environment is properly set up.

#### 2: Make a directory where we build the source code from.
```
cd ShannonBase && mkdir cmake_build && cd cmake_build
```

#### 3: Run cmake and start compilation and installation.
```
 cmake ../ \
  -DWITH_BOOST=/path-to-boost-include-files/ \
  -DCMAKE_BUILD_TYPE=[Release|Debug]  \
  -DCMAKE_INSTALL_PREFIX=/path-to-shannon-bin \
  -DMYSQL_DATADIR=/home/path-to-shannon-bin/data \
  -DSYSCONFDIR=. \
  -DMYSQL_UNIX_ADDR=/home/path-to-shannon-bin/tmp/mysql.sock \
  -DWITH_EMBEDDED_SERVER=OFF \
  -DWITH_MYISAM_STORAGE_ENGINE=1 \
  -DWITH_INNOBASE_STORAGE_ENGINE=1 \
  -DWITH_PARTITION_STORAGE_ENGINE=1 \
  -DMYSQL_TCP_PORT=3306 \
  -DENABLED_LOCAL_INFILE=1 \
  -DEXTRA_CHARSETS=all \
  -DWITH_PROTOBUF=bundled \
  -DWITH_SSL_PATH=/path-to-open-ssl/ \
  -DDEFAULT_SET=community \
  -DWITH_UNIT_TESTS=OFF \
  [-DENABLE_GCOV=1 \ |
  -DWITH_ASAN=1 \    | 
  ]
  -DCOMPILATION_COMMENT="MySQL Community Server, and Shannon Data AI Alpha V.- (GPL)" 

make -j5 && make install

```
PS: in `[]`, it's an optional compilation params, which is to enable coverage collection and ASAN check.

#### 4: Initialize the database and run ShannonBase
```
 /path-to-shannbase-bin/bin/mysqld --defaults-file=/path-to-shannonbase-bin/my.cnf --initialize  --user=xxx

  /path-to-shannbase-bin/bin//mysqld --defaults-file=/path-to-shannonbase-bin/my.cnf   --user=xxx & 
```
PS: you should use your own `my.cnf`.

### Basic Usage
#### 1: Rapid Engine Usage.
To create a test table with secondary_engine set to Rapid and load it into Rapid, use the following SQL commands:
```
CREATE TABLE test1 (
    col1 INT PRIMARY KEY,
    col2 INT
) SECONDARY_ENGINE = Rapid;

ALTER TABLE test1 SECONDARY_LOAD;
```

If you want to forcefully use Rapid, use:
```
set use_secondary_engine=forced;
```

#### 2: Using GIS, JSON, Vector.
ShannonBase supports GIS data types for storing and querying spatial data.
```
CREATE TABLE locations (
    id INT PRIMARY KEY,
    name VARCHAR(100),
    coordinates POINT NOT NULL
);

INSERT INTO locations (id, name, coordinates) VALUES 
    (1, 'Beijing', ST_GeomFromText('POINT(116.4074 39.9042)')), 
    (2, 'Shanghai', ST_GeomFromText('POINT(121.4737 31.2304)')), 
    (3, 'Guangzhou', ST_GeomFromText('POINT(113.2644 23.1291)')), 
    (4, 'Shenzhen', ST_GeomFromText('POINT(114.0579 22.5431)')), 
    (5, 'Chengdu', ST_GeomFromText('POINT(104.0665 30.5728)'));

SELECT name FROM locations WHERE ST_X(coordinates) BETWEEN 110 AND 120 AND ST_Y(coordinates) BETWEEN 20 AND 40;
```

ShannonBase allows efficient JSON storage and querying.
```
CREATE TABLE users (
    id INT PRIMARY KEY,
    name VARCHAR(100),
    details JSON
);

INSERT INTO users (id, name, details) 
VALUES (1, 'Alice', '{"age": 30, "email": "alice@example.com", "preferences": {"theme": "dark"}}');

SELECT details->>'$.email' AS email FROM users WHERE details->>'$.preferences.theme' = 'dark';
```

ShannonBase natively supports Vector data types for AI and ML applications.
```
CREATE TABLE embeddings (
    id INT PRIMARY KEY,
    description TEXT,
    embedding VECTOR(10)) secondary_engine=rapid;

INSERT INTO embeddings (id, description, embedding)
VALUES (1, 'Example text', TO_VECTOR("[0.12, -0.34, 0.56, 0.78, -0.91, 0.23, -0.45, 0.67, -0.89, 1.23]"));

SELECT LENGTH(embedding), FROM_VECTOR(embedding) FROM embeddings WHERE id = 1;
```

#### 3: Using ML functions.
Use native ML functions in ShannonBase to perform machine learning tasks seamlessly.
```
CREATE TABLE census_train ( age INT, workclass VARCHAR(255), fnlwgt INT, education VARCHAR(255), `education-num` INT, `marital-status` VARCHAR(255), occupation VARCHAR(255), relationship VARCHAR(255), race VARCHAR(255), sex VARCHAR(255), `capital-gain` INT, `capital-loss` INT, `hours-per-week` INT, `native-country` VARCHAR(255), revenue VARCHAR(255)) secondary_engine=rapid;

CREATE TABLE census_test LIKE census_train;

LOAD DATA INFILE '/path_to_data_source/ML/census/census_train_load.csv' INTO TABLE census_train FIELDS TERMINATED BY ',' ;

LOAD DATA INFILE '/path_to_data_source//ML/census/census_test_load.csv' INTO TABLE census_test FIELDS TERMINATED BY ',' ;

ALTER TABLE census_train secondary_load;
SET @census_model = 'census_test';

CALL sys.ML_TRAIN('heatwaveml_bench.census_train', 'revenue', JSON_OBJECT('task', 'classification'), @census_model);

CALL sys.ML_MODEL_LOAD(@census_model, NULL);

SELECT sys.ML_PREDICT_ROW(@row_input, @census_model, NULL);
```

#### 3: Creating javascript language stored procedure.
To specify the language as `JavaScript`, you can create a stored procedure in JavaScript
```
DELIMITER |;
CREATE FUNCTION IS_EVEN (VAL INT) RETURNS INT
LANGUAGE JAVASCRIPT AS $$
function isEven(num) {
    return num % 2 == 0;
}
return isEven(VAL);
$$|
DELIMITER ;|

SELECT is_even(3);
```

For more information, please refer to https://github.com/Shannon-Data/ShannonBase/wiki
for details.
