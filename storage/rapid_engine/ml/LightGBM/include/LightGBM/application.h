#ifndef LIGHTGBM_APPLICATION_H_
#define LIGHTGBM_APPLICATION_H_

#include <LightGBM/meta.h>
#include <LightGBM/config.h>
#include <map>
#include <vector>
#include <memory>

namespace LightGBM {

class DatasetLoader;
class Dataset;
class Boosting;
class ObjectiveFunction;
class Metric;

/*!
* \brief The main entrance of LightGBM. this application has two tasks:
*        Train and Predict.
*        Train task will train a new model
*        Predict task will predict the scores of test data using existing model,
*        and save the score to disk.
*/
class Application {
public:
  Application(char *model_filename,char *config_file);
  Application(int argc, char** argv);

  /*! \brief Destructor */
  ~Application();

  /*! \brief To call this funciton to run application*/
  inline void Run();

  void InitPredictOnline();
  void PredictOnline(std::vector<std::string>& vector_datas, int label_index,std::map<int,  std::vector<double> >& result);
private:

  /*! \brief Load parameters from command line and config file*/
  void LoadParameters(int argc, char** argv);

  /*! \brief Load data, including training data and validation data*/
  void LoadData();

  /*! \brief Initialization before training*/
  void InitTrain();

  /*! \brief Main Training logic */
  void Train();

  /*! \brief Initializations before prediction */
  void InitPredict();

  /*! \brief Main predicting logic */
  void Predict();

  /*! \brief Main Convert model logic */
  void ConvertModel();

  /*! \brief All configs */
  Config config_;
  /*! \brief Training data */
  std::unique_ptr<Dataset> train_data_;
  /*! \brief Validation data */
  std::vector<std::unique_ptr<Dataset>> valid_datas_;
  /*! \brief Metric for training data */
  std::vector<std::unique_ptr<Metric>> train_metric_;
  /*! \brief Metrics for validation data */
  std::vector<std::vector<std::unique_ptr<Metric>>> valid_metrics_;
  /*! \brief Boosting object */
  std::unique_ptr<Boosting> boosting_;
  /*! \brief Training objective function */
  std::unique_ptr<ObjectiveFunction> objective_fun_;
  
  std::string input_model ;
};


inline void Application::Run() {
  if (config_.task == TaskType::kPredict || config_.task == TaskType::KRefitTree) {
    InitPredict();
    Predict();
  } else if (config_.task == TaskType::kConvertModel) {
    ConvertModel();
  } else {
    InitTrain();
    Train();
  }
}
}  // namespace LightGBM

#endif   // LightGBM_APPLICATION_H_
