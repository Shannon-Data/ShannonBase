/* Copyright (c) 2026, Shannon Data AI and/or its affiliates.
   SPDX-License-Identifier: GPL-2.0-only */
#include <gtest/gtest.h>
#include <string>

#include "sql/sql_lex.h"
#include "storage/rapid_engine/ml/query_arbitrator.h"
#include "unittest/gunit/test_utils.h"

namespace ShannonBase::ML {
class RapidMlTest : public ::testing::Test {
 protected:
  void SetUp() override { m_server.SetUp(); }
  void TearDown() override { m_server.TearDown(); }

  void ExpectPrediction(const char *model, Query_arbitrator::WHERE2GO expected) {
    Query_arbitrator classifier;
    ASSERT_TRUE(classifier.load_model(std::string(RAPID_ML_TEST_MODEL_DIR) + "/" + model + ".onnx"));
    EXPECT_EQ(expected, classifier.predict(m_server.thd(), m_server.thd()->lex->query_block));
    EXPECT_FALSE(m_server.thd()->is_error());
  }

  my_testing::Server_initializer m_server;
};

TEST_F(RapidMlTest, ScalarScoreCanChooseSecondary) {
  ExpectPrediction("scalar", Query_arbitrator::WHERE2GO::TO_SECONDARY);
}

TEST_F(RapidMlTest, ClassScoresCanChooseSecondary) {
  ExpectPrediction("classes", Query_arbitrator::WHERE2GO::TO_SECONDARY);
}

TEST_F(RapidMlTest, WrongOutputTypeFallsBack) {
  ExpectPrediction("wrong_type", Query_arbitrator::WHERE2GO::TO_PRIMARY);
}

TEST_F(RapidMlTest, WrongOutputShapeFallsBack) {
  ExpectPrediction("wrong_shape", Query_arbitrator::WHERE2GO::TO_PRIMARY);
}

TEST_F(RapidMlTest, NaNScoreFallsBack) {
  ExpectPrediction("nan_score", Query_arbitrator::WHERE2GO::TO_PRIMARY);
}

TEST_F(RapidMlTest, InfiniteScoreFallsBack) {
  ExpectPrediction("inf_score", Query_arbitrator::WHERE2GO::TO_PRIMARY);
}

TEST_F(RapidMlTest, RuntimeExceptionFallsBackAndNextPredictionWorks) {
  ExpectPrediction("wrong_input_type", Query_arbitrator::WHERE2GO::TO_PRIMARY);
  ExpectPrediction("scalar", Query_arbitrator::WHERE2GO::TO_SECONDARY);
}

TEST_F(RapidMlTest, UnloadedModelFallsBack) {
  Query_arbitrator classifier;
  EXPECT_EQ(Query_arbitrator::WHERE2GO::TO_PRIMARY,
            classifier.predict(m_server.thd(), m_server.thd()->lex->query_block));
  EXPECT_FALSE(m_server.thd()->is_error());
}
}  // namespace ShannonBase::ML
