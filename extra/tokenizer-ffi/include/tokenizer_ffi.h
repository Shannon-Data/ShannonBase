
/*
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License, version 2.0,
as published by the Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License, version 2.0, for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

Copyright (c) 2023, Shannon Data AI and/or its affiliates.
It's auto-generated, DO NOT MODIFY.
*/

#ifndef TOKENIZER_FFI_H
#define TOKENIZER_FFI_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct TokenizerHandle TokenizerHandle;

// Encoding result structure
typedef struct {
    uint32_t* ids;
    size_t length;
    uint32_t* attention_mask;
    char** tokens;
} EncodingResult;

// Error handling functions
/**
 * Get the last error message for the current thread.
 * Returns NULL if no error occurred.
 * The returned pointer is valid until the next tokenizer function call
 * on the same thread, or until the thread exits.
 */
const char* tokenizer_get_last_error(void);

/**
 * Check if there's a pending error on the current thread.
 * Returns true if an error occurred, false otherwise.
 */
bool tokenizer_has_error(void);

/**
 * Clear the last error message for the current thread.
 */
void tokenizer_clear_error(void);

// Tokenizer creation functions
/**
 * Create a tokenizer from a file path.
 * Returns NULL on failure. Check tokenizer_get_last_error() for details.
 */
TokenizerHandle* tokenizer_from_file(const char* path);

/**
 * Create a tokenizer from a JSON string.
 * Returns NULL on failure. Check tokenizer_get_last_error() for details.
 */
TokenizerHandle* tokenizer_from_json(const char* json);

// Tokenizer operations
/**
 * Encode a single text string.
 * Returns NULL on failure. Check tokenizer_get_last_error() for details.
 */
EncodingResult* tokenizer_encode(TokenizerHandle* handle, const char* text, bool add_special_tokens);

/**
 * Encode multiple text strings in batch.
 * Returns NULL on failure. Check tokenizer_get_last_error() for details.
 */
EncodingResult** tokenizer_encode_batch(TokenizerHandle* handle, const char** texts, size_t count, bool add_special_tokens);

/**
 * Decode token IDs back to text.
 * Returns NULL on failure. Check tokenizer_get_last_error() for details.
 * The returned string must be freed with string_free().
 */
char* tokenizer_decode(TokenizerHandle* handle, const uint32_t* ids, size_t length, bool skip_special_tokens);

/**
 * Get the vocabulary size of the tokenizer.
 * Returns 0 on failure. Check tokenizer_get_last_error() for details.
 */
uint32_t tokenizer_get_vocab_size(TokenizerHandle* handle);

// Utility functions
/**
 * Check if a tokenizer handle is valid (non-null).
 */
bool tokenizer_is_valid(const TokenizerHandle* handle);

// Memory management functions
/**
 * Free a tokenizer handle.
 * Safe to call with NULL pointer.
 */
void tokenizer_free(TokenizerHandle* handle);

/**
 * Free an encoding result.
 * Safe to call with NULL pointer.
 */
void encoding_result_free(EncodingResult* result);

/**
 * Free an array of encoding results.
 * Safe to call with NULL pointer.
 */
void encoding_result_array_free(EncodingResult** results, size_t count);

/**
 * Free a C string returned by tokenizer functions.
 * Safe to call with NULL pointer.
 */
void string_free(char* s);

#ifdef __cplusplus
}
#endif

#endif // TOKENIZER_FFI_H
