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

#include "ml_forecasting.h"

namespace ShannonBase {
namespace ML {

ML_forecasting::ML_forecasting() {
}

ML_forecasting::~ML_forecasting() {
}

int ML_forecasting::train() {
  return 0;
}

int ML_forecasting::predict() {
  return 0;
}

int ML_forecasting::load(std::string model_handle_name, std::string user_name) {
  return 0;
}

int ML_forecasting::unload(std::string model_handle_name) {
  return 0;
}

int ML_forecasting::import() {
  return 0;
}

double ML_forecasting::score() {
  return 0;
}

int ML_forecasting::explain_row() {
  return 0;
}

int ML_forecasting::explain_table() {
  return 0;
}

int ML_forecasting::predict_row() {
  return 0;
}

int ML_forecasting::predict_table() {
  return 0;
}


} //ml
} //shannonbase