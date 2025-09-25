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

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================

typedef struct TokenizerHandle TokenizerHandle;

// =============================================================================
// STRUCTURES - HuggingFace Compatible
// =============================================================================

/**
 * Encoding result structure - matches HuggingFace tokenizer output
 * This structure contains all the standard outputs from HuggingFace tokenizers
 */
typedef struct EncodingResult {
    uint32_t* input_ids;           // Primary token IDs (equivalent to HF's input_ids)
    size_t length;                 // Length of all arrays
    uint32_t* attention_mask;      // Attention mask (1 for real tokens, 0 for padding)
    char** tokens;                 // String representation of tokens
    uint32_t* token_type_ids;      // Token type IDs (for models like BERT)
    uint32_t* special_tokens_mask; // Special tokens mask (1 for special tokens)
    struct EncodingResult** overflowing; // Overflowing tokens (if truncation occurred)
    size_t overflowing_length;     // Number of overflowing encodings
} EncodingResult;

/**
 * Batch encoding result
 */
typedef struct BatchEncodingResult {
    EncodingResult** encodings;    // Array of encoding results
    size_t length;                 // Number of encodings
} BatchEncodingResult;

typedef struct ChatMessage{
    const char* role;
    const char* content;
} ChatMessage;

// =============================================================================
// ERROR HANDLING - HuggingFace Compatible
// =============================================================================

/**
 * Get the last error message for the current thread.
 * Returns NULL if no error occurred.
 * The returned pointer is valid until the next tokenizer function call.
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

// =============================================================================
// TOKENIZER CREATION - HuggingFace Compatible
// =============================================================================

/**
 * Create tokenizer from file path.
 * Equivalent to: tokenizer = Tokenizer.from_file("path/to/tokenizer.json")
 * 
 * @param path Path to tokenizer.json file
 * @return Tokenizer handle or NULL on error
 */
TokenizerHandle* tokenizer_from_file(const char* path);

/**
 * Create tokenizer from raw bytes.
 * Equivalent to: tokenizer = Tokenizer.from_bytes(data)
 * 
 * @param data Raw byte data
 * @param length Length of data
 * @return Tokenizer handle or NULL on error
 */
TokenizerHandle* tokenizer_from_bytes(const uint8_t* data, size_t length);

/**
 * Create tokenizer from JSON string.
 * Equivalent to: tokenizer = Tokenizer.from_json(json_str)
 * 
 * @param json JSON string
 * @return Tokenizer handle or NULL on error
 */
TokenizerHandle* tokenizer_from_json(const char* json);

// =============================================================================
// ENCODING FUNCTIONS - HuggingFace Compatible
// =============================================================================

/**
 * Encode single text string.
 * Equivalent to: tokenizer.encode(text, add_special_tokens=add_special_tokens)
 * 
 * @param handle Tokenizer handle
 * @param text Input text to encode
 * @param add_special_tokens Whether to add special tokens (CLS, SEP, etc.)
 * @return EncodingResult or NULL on error. Must be freed with encoding_result_free()
 */
EncodingResult* tokenizer_encode(TokenizerHandle* handle, const char* text, bool add_special_tokens);

/**
 * Encode batch of text strings.
 * Equivalent to: tokenizer.encode_batch(texts, add_special_tokens=add_special_tokens)
 * 
 * @param handle Tokenizer handle
 * @param texts Array of input texts
 * @param count Number of texts in array
 * @param add_special_tokens Whether to add special tokens
 * @return BatchEncodingResult or NULL on error. Must be freed with batch_encoding_result_free()
 */
BatchEncodingResult* tokenizer_encode_batch(TokenizerHandle* handle, const char** texts, size_t count, bool add_special_tokens);

/**
 * Decode token IDs back to text.
 * Equivalent to: tokenizer.decode(ids, skip_special_tokens=skip_special_tokens)
 * 
 * @param handle Tokenizer handle
 * @param ids Array of token IDs
 * @param length Number of token IDs
 * @param skip_special_tokens Whether to skip special tokens in output
 * @return Decoded text string or NULL on error. Must be freed with string_free()
 */
char* tokenizer_decode(TokenizerHandle* handle, const uint32_t* ids, size_t length, bool skip_special_tokens);

/**
 * Apply chat template to messages.
 * 
 * @param handle Tokenizer handle
 * @param messages Array of chat messages
 * @param messages_count Number of messages in the array
 * @param add_generation_prompt Whether to add generation prompt at the end
 * @param chat_template Optional custom template (NULL to use tokenizer's default)
 * @return Formatted text string, or NULL on failure
 *         The returned string must be freed with string_free()
 */
char* tokenizer_apply_chat_template(TokenizerHandle* handle, const ChatMessage* messages, size_t messages_count, bool add_generation_prompt, const char* chat_template);

/**
 * Apply chat template and encode the result in one step.
 * 
 * @param handle Tokenizer handle
 * @param messages Array of chat messages
 * @param messages_count Number of messages in the array
 * @param add_generation_prompt Whether to add generation prompt at the end
 * @param chat_template Optional custom template (NULL to use tokenizer's default)
 * @param add_special_tokens Whether to add special tokens during encoding
 * @return EncodingResult, or NULL on failure
 *         The returned result must be freed with encoding_result_free()
 */
EncodingResult* tokenizer_apply_chat_template_and_encode(TokenizerHandle* handle, const ChatMessage* messages, size_t messages_count, bool add_generation_prompt, const char* chat_template, bool add_special_tokens);

/**
 * Check if tokenizer has a chat template configured.
 * 
 * @param handle Tokenizer handle
 * @return true if chat template is available, false otherwise
 */
bool tokenizer_has_chat_template(TokenizerHandle* handle);

/**
 * Get the chat template string from the tokenizer.
 * 
 * @param handle Tokenizer handle
 * @return Chat template string, or NULL if not available
 *         The returned string must be freed with string_free()
 */
char* tokenizer_get_chat_template(TokenizerHandle* handle);

// =============================================================================
// VOCABULARY FUNCTIONS - HuggingFace Compatible
// =============================================================================

/**
 * Get vocabulary size.
 * Equivalent to: tokenizer.get_vocab_size(with_added_tokens=with_added_tokens)
 * 
 * @param handle Tokenizer handle
 * @param with_added_tokens Whether to include added tokens in count
 * @return Vocabulary size or 0 on error
 */
uint32_t tokenizer_get_vocab_size(TokenizerHandle* handle, bool with_added_tokens);

/**
 * Get full vocabulary as key-value pairs.
 * Equivalent to: tokenizer.get_vocab(with_added_tokens=with_added_tokens)
 * 
 * @param handle Tokenizer handle
 * @param with_added_tokens Whether to include added tokens
 * @param keys Output array of token strings. Must be freed with vocab_free()
 * @param values Output array of token IDs. Must be freed with vocab_free()
 * @param length Output length of arrays
 * @return true on success, false on error
 */
bool tokenizer_get_vocab(TokenizerHandle* handle, bool with_added_tokens, 
                        char*** keys, uint32_t** values, size_t* length);

/**
 * Get tokenizer information.
 * 
 * @param handle Tokenizer handle
 * @param model_type Output model type string. Must be freed with string_free()
 * @param vocab_size Output base vocabulary size
 * @param added_tokens_count Output number of added tokens
 * @return true on success, false on error
 */
bool tokenizer_get_info(TokenizerHandle* handle, char** model_type, 
                       uint32_t* vocab_size, uint32_t* added_tokens_count);

// =============================================================================
// UTILITY FUNCTIONS
// =============================================================================

/**
 * Check if a tokenizer handle is valid (non-null).
 */
bool tokenizer_is_valid(const TokenizerHandle* handle);

// =============================================================================
// MEMORY MANAGEMENT FUNCTIONS
// =============================================================================

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
 * Free a batch encoding result.
 * Safe to call with NULL pointer.
 */
void batch_encoding_result_free(BatchEncodingResult* result);

/**
 * Free vocabulary arrays returned by tokenizer_get_vocab().
 * Safe to call with NULL pointers.
 * 
 * @param keys Array of token strings
 * @param values Array of token IDs
 * @param length Length of arrays
 */
void vocab_free(char** keys, uint32_t* values, size_t length);

/**
 * Free a C string returned by tokenizer functions.
 * Safe to call with NULL pointer.
 */
void string_free(char* s);

#ifdef __cplusplus
}
#endif

#endif // TOKENIZER_FFI_H
