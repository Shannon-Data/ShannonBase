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

DROP FUNCTION IF EXISTS ml_embed_row;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' FUNCTION ml_embed_row (
        in_text TEXT,
        in_model_option JSON
    )
    RETURNS BLOB
    COMMENT '
Description
-----------

The ML_EMBED_ROW routine uses the specified embedding model to encode the specified text or query into a vector embedding. 

Parameters
-----------

in_text (TEXT):
  user-defined session variable storing the ML model handle for the duration of the connection.
in_model_option JSON:
  specifies optional parameters as key-value pairs in JSON format
Example
-----------
mysql> SELECT sys.ML_EMBED_ROW("What is artificial intelligence?", JSON_OBJECT("model_id", "all_minilm_l12_v2")) into @text_embedding;
'
    SQL SECURITY INVOKER
    DETERMINISTIC
    CONTAINS SQL
BEGIN

  DECLARE v_embed_row_res BLOB;

  SELECT ML_MODEL_EMBED_ROW(in_text, in_model_option) INTO v_embed_row_res;
  RETURN v_embed_row_res;
END$$

DELIMITER ;
