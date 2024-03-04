#ifndef LIGHTGBM_META_H_
#define LIGHTGBM_META_H_

#include <cstdint>

#include <limits>
#include <vector>
#include <functional>
#include <memory>

namespace LightGBM {

/*! \brief Type of data size, it is better to use signed type*/
typedef int32_t data_size_t;

// Enable following marco to use double for score_t
// #define SCORE_T_USE_DOUBLE

// Enable following marco to use double for label_t
// #define LABEL_T_USE_DOUBLE

/*! \brief Type of score, and gradients */
#ifdef SCORE_T_USE_DOUBLE
typedef double score_t;
#else
typedef float score_t;
#endif

/*! \brief Type of metadata, include weight and label */
#ifdef LABEL_T_USE_DOUBLE
typedef double label_t;
#else
typedef float label_t;
#endif

const score_t kMinScore = -std::numeric_limits<score_t>::infinity();

const score_t kEpsilon = 1e-15f;

const double kZeroThreshold = 1e-35f;


typedef int32_t comm_size_t;

using PredictFunction =
std::function<void(const std::vector<std::pair<int, double>>&, double* output)>;

typedef void(*ReduceFunction)(const char* input, char* output, int type_size, comm_size_t array_size);


typedef void(*ReduceScatterFunction)(char* input, comm_size_t input_size, int type_size,
                                     const comm_size_t* block_start, const comm_size_t* block_len, int num_block, char* output, comm_size_t output_size,
                                     const ReduceFunction& reducer);

typedef void(*AllgatherFunction)(char* input, comm_size_t input_size, const comm_size_t* block_start,
                                 const comm_size_t* block_len, int num_block, char* output, comm_size_t output_size);


#define NO_SPECIFIC (-1)

#if (_MSC_VER <= 1800)
#define __func__ __FUNCTION__
#endif

}  // namespace LightGBM

#endif   // LightGBM_META_H_
