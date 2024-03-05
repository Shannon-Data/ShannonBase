/**
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   The fundmental code for ML.

   Copyright (c) 2023-, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_ML_REGRESSION_H__
#define __SHANNONBASE_ML_REGRESSION_H__

#include <memory>
#include <string>

#include "ml_algorithm.h"

namespace LightGBM{
   class Application;
}

namespace ShannonBase {
namespace ML {

enum class STAGE {
   UNKNOWN,
   TRAINED,
   PREDICT
};
class ML_regression : public ML_algorithm {
  public:
    ML_regression(std::string sch_name, std::string table_name,
                  std::string target_name);
    ~ML_regression();
    int train() override;
    int predict() override;
 private:
   std::string m_sch_name;
   std::string m_table_name;
   std::string m_target_name;

   STAGE m_stage {STAGE::UNKNOWN};
   std::unique_ptr<LightGBM::Application> m_app;
};

} //ML
} //shannonbase
#endif //__SHANNONBASE_ML_REGRESSION_H__