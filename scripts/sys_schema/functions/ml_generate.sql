-- Copyright (c) 2014, 2023, Oracle and/or its affiliates.
--
-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation; version 2 of the License.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
-- Copyright (c) 2023, Shannon Data AI and/or its affiliates.

DROP FUNCTION IF EXISTS ml_generate;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' FUNCTION ml_generate (
        in_text TEXT,
        in_model_option JSON
    )
    RETURNS TEXT
    COMMENT '
Description
-----------

The ML_GENERATE routine uses the specified large language model (LLM) to generate text-based content as a response for the given natural-language query.

Parameters
-----------

in_text (TEXT):
  : specifies the natural-language query that is passed to the large language model (LLM) handle.
in_model_option JSON:
  specifies optional parameters as key-value pairs in JSON format. It can include the following parameters:
Example
-----------
mysql> SELECT sys.ML_GENERATE("What is AI?", JSON_OBJECT("task", "generation", "model_id", "mistral-7b-instruct-v3", "language", "en"));
'
    SQL SECURITY INVOKER
    DETERMINISTIC
    CONTAINS SQL
BEGIN

  DECLARE v_generate_res TEXT;

  SELECT ML_MODEL_GENERATE(in_text, in_model_option) INTO v_generate_res;
  RETURN v_generate_res;
END$$

DELIMITER ;
