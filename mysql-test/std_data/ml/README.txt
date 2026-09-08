Datasets used by the mysql-test/suite/ml test suite
===================================================

Every directory here holds a headerless, comma separated <name>_train_load.csv
and <name>_test_load.csv pair, loaded by the tests with

  LOAD DATA INFILE '$MYSQLTEST_VARDIR/std_data/ml/<name>/<name>_train_load.csv'
    INTO TABLE <name>_train FIELDS TERMINATED BY ',' [OPTIONALLY ENCLOSED BY '"'] ;

Column order matches the CREATE TABLE in the corresponding test, which in turn
matches the upstream table definition.  Missing values are written as \N so
LOAD DATA stores a real SQL NULL; upstream's preprocess.py instead writes the
literal string "NULL", which cannot round trip through a numeric column.

census
------
Predates this file.  UCI adult / census income, target column `revenue`.
Used by ml_train, ml_predict, ml_score, ml_explain, ml_regression,
ml_anomaly_detection and friends.

The remaining directories back the ml_bench_* tests, which are ports of the
HeatWave AutoML examples at https://github.com/ShannonBase/heatwave-ml.  Each
was fetched from the URL that repository's sql/README.md lists and then run
through the same preparation its sql/preprocess.py applies: a 70/30
train_test_split with random_state=1, stratified on the target where upstream
stratifies.  Where a set is far larger than a test suite wants, it was first
downsampled with the same seeded, stratified splitter; the sizes below say
which.

  name                      source                                          rows kept
  ------------------------  ----------------------------------------------  ---------
  titanic                   openml.org/data/get_csv/16826755/phpMYEkMl.csv  all 1309
                            '?' -> NULL, stratified on survived.  Keeps the
                            missing ages/cabins/boats, so it doubles as the
                            suite's NULLable-feature coverage.
  bank_marketing            archive.ics.uci.edu/.../00222/bank.zip           10000 of 45211
                            bank-full.csv, ';' separated, stratified on y.
  diamonds                  openml.org/data/get_csv/21792853/dataset.csv     10000 of 53940
                            unstratified split, as upstream.
  cnae-9                    openml.org/data/get_csv/1586233/phpmcGu2X.csv   all 1080
                            cast to int, stratified on Class.  856 features,
                            nine classes - the suite's multi-class and widest
                            table case.
  creditcard                ulb.ac.be/di/map/adalpozz/data/creditcard.Rdata  8000 of 284807
                            Time and Class cast to int, stratified on Class.
                            All 492 frauds are kept and only the legitimate
                            transactions are downsampled, so the fraud rate is
                            6.2% here rather than the native 0.17% - at the
                            native rate an 8000 row sample holds ~14 positives,
                            too few to score roc_auc on.  V1..V28 and Amount
                            are rounded to 6 decimals to keep the file small.

Two sets are not downloads:

  electricity_consumption   The forecasting notebook has no dataset to fetch -
                            it builds the San Francisco series in pandas with
                            generate_sf_electricity_data().  That generator was
                            rerun verbatim under np.random.seed(1) over its own
                            2024-01-01..2026-12-31 range and split 80/20
                            chronologically, as the notebook does.
  movielens                 The recommendation notebook uses MovieLens 100K,
                            which could not be downloaded: files.grouplens.org
                            serves an expired TLS certificate and the mirrors
                            tried all 404.  The file here has the same
                            (user_id, movie_id, rating, timestamp) layout,
                            drawn from a seeded rank-3 latent factor model so
                            the recommender has real structure to recover.
                            Replace it with the real u.data when the download
                            becomes reachable again.

Benchmarks added later
----------------------
The rest of sql/'s 18 benchmarks, prepared the same way: the upstream
train_test_split(test_size=0.30, random_state=1[, stratify=<target>]) after a
seeded stratified downsample to roughly 10000 rows.  scikit-learn was not
available when these were built, so the split is an equivalent seeded stratified
splitter rather than sklearn's own; the class balances are preserved either way.
Column order in every file matches the CREATE TABLE of the corresponding
upstream sql/table_*.sql, verified programmatically.

  name              source                                          rows kept
  ----------------  ----------------------------------------------  ---------
  airlines          openml.org/data/get_csv/66526/phpvcoG8S.csv     10000 of 539383
                    Stratified on Delay.  18 airlines and ~290
                    airports make this the high-cardinality
                    categorical case.
  connect-4         archive.ics.uci.edu/.../connect-4.data.Z        10000 of 67557
                    Stratified on class.  Every feature is a three
                    valued board square, so it is the all
                    categorical case.
  nomao             archive.ics.uci.edu/.../00227/Nomao.zip         10000 of 34465
                    Stratified on Class.  Read with na_values='?'
                    as upstream does, so the '?' placeholders
                    become real NULLs across 119 features.
  numerai           openml.org/data/get_csv/2160285/phpg2t68G.csv   10000 of 96320
                    Stratified on attribute_21.  Encrypted equity
                    features: all dense floats.
  appetency         openml.org/data/get_csv/53994/                  10000 of 50000
                    KDDCup09_appetency.arff.  Stratified on
                    APPETENCY.  230 sparse features, strongly
                    imbalanced target.  Table keeps upstream's
                    KDDCup09_appetency name.
  higgs             archive.ics.uci.edu/.../00280/HIGGS.csv.gz      10000 of 1011501
                    Stratified on target.  The 2.6GB download
                    truncates part way; the file is not ordered by
                    target, so the readable 1011501 row prefix
                    keeps the native 53/47 balance.  Features
                    rounded to 6 decimals.
  fashion_mnist     github.com/zalandoresearch/fashion-mnist        3000 train /
                    The four idx-ubyte files, decoded to label      1000 test
                    plus pixel1..pixel784.  Upstream keeps the
                    dataset's own train/test split, so both sides
                    are subsampled separately, stratified on
                    label.  All 784 pixel columns exceed what
                    Rapid can load, so the test demonstrates that
                    and then trains on a 14x14 subsample.
  mercedes          openml.org/data/get_csv/21854646/dataset.csv    all 4209
                    ID dropped, unstratified split as upstream.
                    377 columns.
  news_popularity   openml.org/data/get_csv/22044756/dataset.csv    10000 of 39644
                    url column dropped, unstratified.
  black_friday      openml.org/data/get_csv/21230845/               10000 of 166821
                    file639340bd9ca9.arff.  Unstratified.
  nyc_taxi          openml.org/data/get_csv/22044763/dataset.csv    10000 of 581835
                    total_amount and the three lpep_dropoff_
                    datetime parts dropped, as upstream does.
  twitter           archive.ics.uci.edu/.../00248/regression.tar.gz 10000 of 583250
                    Twitter.data, unstratified.
