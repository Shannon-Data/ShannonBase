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
*/

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;
use tokenizers::{Tokenizer, Encoding};
use libc::size_t;
use once_cell::sync::Lazy;
use std::sync::Mutex;

// Global error storage (thread-safe)
static LAST_ERROR: Lazy<Mutex<Option<String>>> = Lazy::new(|| Mutex::new(None));

/// Set the last error message
fn set_last_error(msg: impl Into<String>) {
    if let Ok(mut error) = LAST_ERROR.lock() {
        *error = Some(msg.into());
    }
}

/// Clear the last error
fn clear_last_error() {
    if let Ok(mut error) = LAST_ERROR.lock() {
        *error = None;
    }
}

/// Get the last error message
#[no_mangle]
pub extern "C" fn tokenizer_get_last_error() -> *const c_char {
    static mut ERROR_CSTRING: Option<CString> = None;
    
    if let Ok(error) = LAST_ERROR.lock() {
        if let Some(ref err) = *error {
            unsafe {
                ERROR_CSTRING = CString::new(err.as_str()).ok();
                if let Some(ref cs) = ERROR_CSTRING {
                    return cs.as_ptr();
                }
            }
        }
    }
    ptr::null()
}

/// Check if there's a pending error
#[no_mangle]
pub extern "C" fn tokenizer_has_error() -> bool {
    if let Ok(error) = LAST_ERROR.lock() {
        error.is_some()
    } else {
        false
    }
}

/// Clear the last error message
#[no_mangle]
pub extern "C" fn tokenizer_clear_error() {
    clear_last_error();
}

// Tokenizer handle
#[repr(C)]
pub struct TokenizerHandle {
    tokenizer: Tokenizer,
}

// Encoding result - exactly matching HuggingFace behavior
#[repr(C)]
pub struct EncodingResult {
    pub input_ids: *mut u32,
    pub length: size_t,
    pub attention_mask: *mut u32,
    pub tokens: *mut *mut c_char,
    pub token_type_ids: *mut u32,
    pub special_tokens_mask: *mut u32,
    pub overflowing: *mut *mut EncodingResult,
    pub overflowing_length: size_t,
}

// Batch encoding result
#[repr(C)]
pub struct BatchEncodingResult {
    pub encodings: *mut *mut EncodingResult,
    pub length: size_t,
}

/// Clean and repair JSON content
fn clean_json_content(content: &str) -> String {
    let mut cleaned = content.to_string();
    
    // Remove BOM if present
    if cleaned.starts_with('\u{feff}') {
        cleaned = cleaned.trim_start_matches('\u{feff}').to_string();
    }
    
    // Remove null terminators
    cleaned = cleaned.trim_end_matches('\0').to_string();
    
    // Trim whitespace
    cleaned = cleaned.trim().to_string();
    
    cleaned
}

/// Create tokenizer from file path - HuggingFace compatible
#[no_mangle]
pub extern "C" fn tokenizer_from_file(path: *const c_char) -> *mut TokenizerHandle {
    clear_last_error();

    if path.is_null() {
        set_last_error("Path cannot be null");
        return ptr::null_mut();
    }

    let c_str = unsafe { CStr::from_ptr(path) };
    let path_str = match c_str.to_str() {
        Ok(s) => s,
        Err(e) => {
            set_last_error(format!("Invalid UTF-8 in path: {}", e));
            return ptr::null_mut();
        }
    };

    // Primary method: Use HuggingFace's official from_file
    match Tokenizer::from_file(path_str) {
        Ok(tokenizer) => {
            let handle = TokenizerHandle { tokenizer };
            Box::into_raw(Box::new(handle))
        }
        Err(e) => {
            // Fallback: Try manual loading with JSON repair
            match std::fs::read_to_string(path_str) {
                Ok(content) => {
                    let cleaned_content = clean_json_content(&content);
                    
                    match Tokenizer::from_bytes(cleaned_content.as_bytes()) {
                        Ok(tokenizer) => {
                            let handle = TokenizerHandle { tokenizer };
                            Box::into_raw(Box::new(handle))
                        }
                        Err(e2) => {
                            set_last_error(format!(
                                "Unable to load tokenizer from '{}'. Primary error: {}. Fallback error: {}. \
                                Consider using a compatible tokenizers version (0.13-0.15) or download a fresh tokenizer.json",
                                path_str, e, e2
                            ));
                            ptr::null_mut()
                        }
                    }
                }
                Err(io_err) => {
                    set_last_error(format!(
                        "Unable to load tokenizer from '{}'. Parse error: {}. File read error: {}",
                        path_str, e, io_err
                    ));
                    ptr::null_mut()
                }
            }
        }
    }
}

/// Create tokenizer from JSON bytes - HuggingFace compatible
#[no_mangle]
pub extern "C" fn tokenizer_from_bytes(data: *const u8, length: size_t) -> *mut TokenizerHandle {
    clear_last_error();
    
    if data.is_null() || length == 0 {
        set_last_error("Data cannot be null or empty");
        return ptr::null_mut();
    }

    let bytes = unsafe { std::slice::from_raw_parts(data, length as usize) };
    
    match Tokenizer::from_bytes(bytes) {
        Ok(tokenizer) => {
            let handle = TokenizerHandle { tokenizer };
            Box::into_raw(Box::new(handle))
        }
        Err(e) => {
            set_last_error(format!("Failed to parse tokenizer from bytes: {}", e));
            ptr::null_mut()
        }
    }
}

/// Create tokenizer from JSON string - HuggingFace compatible
#[no_mangle]
pub extern "C" fn tokenizer_from_json(json: *const c_char) -> *mut TokenizerHandle {
    clear_last_error();
    
    if json.is_null() {
        set_last_error("JSON cannot be null");
        return ptr::null_mut();
    }

    let c_str = unsafe { CStr::from_ptr(json) };
    let json_str = match c_str.to_str() {
        Ok(s) => s,
        Err(e) => {
            set_last_error(format!("Invalid UTF-8 in JSON: {}", e));
            return ptr::null_mut();
        }
    };

    let cleaned_json = clean_json_content(json_str);
    
    match Tokenizer::from_bytes(cleaned_json.as_bytes()) {
        Ok(tokenizer) => {
            let handle = TokenizerHandle { tokenizer };
            Box::into_raw(Box::new(handle))
        }
        Err(e) => {
            set_last_error(format!("Failed to parse tokenizer JSON: {}", e));
            ptr::null_mut()
        }
    }
}

/// Create encoding result from HuggingFace Encoding
fn create_encoding_result(encoding: Encoding) -> Result<*mut EncodingResult, String> {
    let length = encoding.len();
    
    // Get input_ids (this is the primary token ID array)
    let input_ids = encoding.get_ids().to_vec();
    let mut input_ids_vec = input_ids;
    let input_ids_ptr = input_ids_vec.as_mut_ptr();
    std::mem::forget(input_ids_vec);

    // Get attention_mask
    let attention_mask = encoding.get_attention_mask().to_vec();
    let mut attention_mask_vec = attention_mask;
    let attention_mask_ptr = attention_mask_vec.as_mut_ptr();
    std::mem::forget(attention_mask_vec);

    // Get token_type_ids (if available)
    let token_type_ids = encoding.get_type_ids().to_vec();
    let mut token_type_ids_vec = token_type_ids;
    let token_type_ids_ptr = token_type_ids_vec.as_mut_ptr();
    std::mem::forget(token_type_ids_vec);

    // Get special_tokens_mask
    let special_tokens_mask = encoding.get_special_tokens_mask().to_vec();
    let mut special_tokens_mask_vec = special_tokens_mask;
    let special_tokens_mask_ptr = special_tokens_mask_vec.as_mut_ptr();
    std::mem::forget(special_tokens_mask_vec);

    // Get tokens (string representations)
    let tokens = encoding.get_tokens();
    let mut tokens_ptrs: Vec<*mut c_char> = Vec::with_capacity(length);
    
    for token in tokens {
        match CString::new(token.as_str()) {
            Ok(c_str) => tokens_ptrs.push(c_str.into_raw()),
            Err(e) => {
                // Clean up on error
                unsafe {
                    let _ = Vec::from_raw_parts(input_ids_ptr, 0, length);
                    let _ = Vec::from_raw_parts(attention_mask_ptr, 0, length);
                    let _ = Vec::from_raw_parts(token_type_ids_ptr, 0, length);
                    let _ = Vec::from_raw_parts(special_tokens_mask_ptr, 0, length);
                    for ptr in tokens_ptrs {
                        let _ = CString::from_raw(ptr);
                    }
                }
                return Err(format!("Failed to convert token to C string: {}", e));
            }
        }
    }

    let tokens_ptr = tokens_ptrs.as_mut_ptr();
    std::mem::forget(tokens_ptrs);

    // Handle overflowing tokens (if any)
    let overflowing = encoding.get_overflowing();
    let (overflowing_ptr, overflowing_length) = if overflowing.is_empty() {
        (ptr::null_mut(), 0)
    } else {
        let mut overflowing_results = Vec::with_capacity(overflowing.len());
        for overflow in overflowing {
            match create_encoding_result(overflow.clone()) {
                Ok(result) => overflowing_results.push(result),
                Err(e) => {
                    // Clean up on error
                    unsafe {
                        let _ = Vec::from_raw_parts(input_ids_ptr, 0, length);
                        let _ = Vec::from_raw_parts(attention_mask_ptr, 0, length);
                        let _ = Vec::from_raw_parts(token_type_ids_ptr, 0, length);
                        let _ = Vec::from_raw_parts(special_tokens_mask_ptr, 0, length);
                        let _ = Vec::from_raw_parts(tokens_ptr, 0, length);
                        for result in overflowing_results {
                            encoding_result_free(result);
                        }
                    }
                    return Err(e);
                }
            }
        }
        let length = overflowing_results.len();
        let ptr = overflowing_results.as_mut_ptr();
        std::mem::forget(overflowing_results);
        (ptr, length)
    };

    Ok(Box::into_raw(Box::new(EncodingResult {
        input_ids: input_ids_ptr,
        length: length as size_t,
        attention_mask: attention_mask_ptr,
        tokens: tokens_ptr,
        token_type_ids: token_type_ids_ptr,
        special_tokens_mask: special_tokens_mask_ptr,
        overflowing: overflowing_ptr,
        overflowing_length: overflowing_length as size_t,
    })))
}

/// Encode text - exact HuggingFace behavior
#[no_mangle]
pub extern "C" fn tokenizer_encode(
    handle: *mut TokenizerHandle,
    text: *const c_char,
    add_special_tokens: bool,
) -> *mut EncodingResult {
    clear_last_error();
    
    if handle.is_null() {
        set_last_error("Tokenizer handle cannot be null");
        return ptr::null_mut();
    }
    
    if text.is_null() {
        set_last_error("Text cannot be null");
        return ptr::null_mut();
    }

    let tokenizer = unsafe { &(*handle).tokenizer };
    let c_str = unsafe { CStr::from_ptr(text) };
    let text_str = match c_str.to_str() {
        Ok(s) => s,
        Err(e) => {
            set_last_error(format!("Invalid UTF-8 in text: {}", e));
            return ptr::null_mut();
        }
    };

    match tokenizer.encode(text_str, add_special_tokens) {
        Ok(encoding) => {
            match create_encoding_result(encoding) {
                Ok(result) => result,
                Err(e) => {
                    set_last_error(e);
                    ptr::null_mut()
                }
            }
        }
        Err(e) => {
            set_last_error(format!("Failed to encode text: {}", e));
            ptr::null_mut()
        }
    }
}

/// Encode batch of texts - exact HuggingFace behavior
#[no_mangle]
pub extern "C" fn tokenizer_encode_batch(
    handle: *mut TokenizerHandle,
    texts: *const *const c_char,
    count: size_t,
    add_special_tokens: bool,
) -> *mut BatchEncodingResult {
    clear_last_error();
    
    if handle.is_null() {
        set_last_error("Tokenizer handle cannot be null");
        return ptr::null_mut();
    }
    
    if texts.is_null() || count == 0 {
        set_last_error("Texts array cannot be null or empty");
        return ptr::null_mut();
    }

    let tokenizer = unsafe { &(*handle).tokenizer };
    let text_slice = unsafe { std::slice::from_raw_parts(texts, count as usize) };

    // Convert C strings to Rust strings
    let mut text_strings = Vec::with_capacity(count as usize);
    for (i, &text_ptr) in text_slice.iter().enumerate() {
        if text_ptr.is_null() {
            set_last_error(format!("Text at index {} cannot be null", i));
            return ptr::null_mut();
        }
        let c_str = unsafe { CStr::from_ptr(text_ptr) };
        match c_str.to_str() {
            Ok(s) => text_strings.push(s),
            Err(e) => {
                set_last_error(format!("Invalid UTF-8 in text at index {}: {}", i, e));
                return ptr::null_mut();
            }
        }
    }

    // Encode batch
    match tokenizer.encode_batch(text_strings, add_special_tokens) {
        Ok(encodings) => {
            let mut result_ptrs = Vec::with_capacity(encodings.len());
            
            for (i, encoding) in encodings.into_iter().enumerate() {
                match create_encoding_result(encoding) {
                    Ok(result) => result_ptrs.push(result),
                    Err(e) => {
                        // Clean up on error
                        for result in result_ptrs {
                            encoding_result_free(result);
                        }
                        set_last_error(format!("Failed to create encoding result at index {}: {}", i, e));
                        return ptr::null_mut();
                    }
                }
            }

            let length = result_ptrs.len();
            let encodings_ptr = result_ptrs.as_mut_ptr();
            std::mem::forget(result_ptrs);

            Box::into_raw(Box::new(BatchEncodingResult {
                encodings: encodings_ptr,
                length: length as size_t,
            }))
        }
        Err(e) => {
            set_last_error(format!("Failed to encode batch: {}", e));
            ptr::null_mut()
        }
    }
}

/// Decode token IDs - exact HuggingFace behavior
#[no_mangle]
pub extern "C" fn tokenizer_decode(
    handle: *mut TokenizerHandle,
    ids: *const u32,
    length: size_t,
    skip_special_tokens: bool,
) -> *mut c_char {
    clear_last_error();
    
    if handle.is_null() {
        set_last_error("Tokenizer handle cannot be null");
        return ptr::null_mut();
    }
    
    if ids.is_null() || length == 0 {
        set_last_error("Token IDs array cannot be null or empty");
        return ptr::null_mut();
    }

    let tokenizer = unsafe { &(*handle).tokenizer };
    let ids_slice = unsafe { std::slice::from_raw_parts(ids, length as usize) };

    match tokenizer.decode(ids_slice, skip_special_tokens) {
        Ok(text) => {
            match CString::new(text) {
                Ok(c_str) => c_str.into_raw(),
                Err(e) => {
                    set_last_error(format!("Failed to convert decoded text to C string: {}", e));
                    ptr::null_mut()
                }
            }
        }
        Err(e) => {
            set_last_error(format!("Failed to decode token IDs: {}", e));
            ptr::null_mut()
        }
    }
}

/// Get vocabulary size - exact HuggingFace behavior
#[no_mangle]
pub extern "C" fn tokenizer_get_vocab_size(handle: *mut TokenizerHandle, with_added_tokens: bool) -> u32 {
    clear_last_error();
    
    if handle.is_null() {
        set_last_error("Tokenizer handle cannot be null");
        return 0;
    }

    let tokenizer = unsafe { &(*handle).tokenizer };
    tokenizer.get_vocab_size(with_added_tokens) as u32
}

/// Get vocabulary as a dictionary - HuggingFace compatible
#[no_mangle]
pub extern "C" fn tokenizer_get_vocab(
    handle: *mut TokenizerHandle,
    with_added_tokens: bool,
    keys: *mut *mut *mut c_char,
    values: *mut *mut u32,
    length: *mut size_t,
) -> bool {
    clear_last_error();
    
    if handle.is_null() || keys.is_null() || values.is_null() || length.is_null() {
        set_last_error("All parameters must be non-null");
        return false;
    }

    let tokenizer = unsafe { &(*handle).tokenizer };
    let vocab = tokenizer.get_vocab(with_added_tokens);

    let mut key_ptrs: Vec<*mut c_char> = Vec::with_capacity(vocab.len());
    let mut value_vec: Vec<u32> = Vec::with_capacity(vocab.len());

    // Convert HashMap to sorted vectors
    let mut vocab_items: Vec<_> = vocab.into_iter().collect();
    vocab_items.sort_by_key(|(_, id)| *id);

    for (token, id) in vocab_items {
        match CString::new(token) {
            Ok(c_str) => {
                key_ptrs.push(c_str.into_raw());
                value_vec.push(id);
            }
            Err(e) => {
                // Clean up on error
                for ptr in key_ptrs {
                    unsafe { let _ = CString::from_raw(ptr); }
                }
                set_last_error(format!("Failed to convert token to C string: {}", e));
                return false;
            }
        }
    }

    let vocab_length = key_ptrs.len();
    
    // Transfer ownership to C
    let keys_ptr = key_ptrs.as_mut_ptr();
    std::mem::forget(key_ptrs);
    
    let values_ptr = value_vec.as_mut_ptr();
    std::mem::forget(value_vec);

    unsafe {
        *keys = keys_ptr;
        *values = values_ptr;
        *length = vocab_length as size_t;
    }

    true
}

/// Free tokenizer
#[no_mangle]
pub extern "C" fn tokenizer_free(handle: *mut TokenizerHandle) {
    if !handle.is_null() {
        unsafe {
            let _ = Box::from_raw(handle);
        }
    }
}

/// Free encoding result
#[no_mangle]
pub extern "C" fn encoding_result_free(result: *mut EncodingResult) {
    if result.is_null() {
        return;
    }

    unsafe {
        let result = Box::from_raw(result);
        
        // Free input_ids
        if !result.input_ids.is_null() {
            let _ = Vec::from_raw_parts(result.input_ids, result.length as usize, result.length as usize);
        }
        
        // Free attention_mask
        if !result.attention_mask.is_null() {
            let _ = Vec::from_raw_parts(result.attention_mask, result.length as usize, result.length as usize);
        }

        // Free token_type_ids
        if !result.token_type_ids.is_null() {
            let _ = Vec::from_raw_parts(result.token_type_ids, result.length as usize, result.length as usize);
        }

        // Free special_tokens_mask
        if !result.special_tokens_mask.is_null() {
            let _ = Vec::from_raw_parts(result.special_tokens_mask, result.length as usize, result.length as usize);
        }
        
        // Free tokens
        if !result.tokens.is_null() {
            let tokens = Vec::from_raw_parts(result.tokens, result.length as usize, result.length as usize);
            for token_ptr in tokens {
                if !token_ptr.is_null() {
                    let _ = CString::from_raw(token_ptr);
                }
            }
        }

        // Free overflowing encodings
        if !result.overflowing.is_null() {
            let overflowing = Vec::from_raw_parts(
                result.overflowing,
                result.overflowing_length as usize,
                result.overflowing_length as usize
            );
            for overflow_ptr in overflowing {
                if !overflow_ptr.is_null() {
                    encoding_result_free(overflow_ptr);
                }
            }
        }
    }
}

/// Free batch encoding result
#[no_mangle]
pub extern "C" fn batch_encoding_result_free(result: *mut BatchEncodingResult) {
    if result.is_null() {
        return;
    }

    unsafe {
        let result = Box::from_raw(result);
        
        if !result.encodings.is_null() {
            let encodings = Vec::from_raw_parts(
                result.encodings,
                result.length as usize,
                result.length as usize
            );
            for encoding_ptr in encodings {
                if !encoding_ptr.is_null() {
                    encoding_result_free(encoding_ptr);
                }
            }
        }
    }
}

/// Free vocabulary arrays
#[no_mangle]
pub extern "C" fn vocab_free(keys: *mut *mut c_char, values: *mut u32, length: size_t) {
    if !keys.is_null() {
        unsafe {
            let key_ptrs = Vec::from_raw_parts(keys, length as usize, length as usize);
            for key_ptr in key_ptrs {
                if !key_ptr.is_null() {
                    let _ = CString::from_raw(key_ptr);
                }
            }
        }
    }
    
    if !values.is_null() {
        unsafe {
            let _ = Vec::from_raw_parts(values, length as usize, length as usize);
        }
    }
}

/// Free C string
#[no_mangle]
pub extern "C" fn string_free(s: *mut c_char) {
    if !s.is_null() {
        unsafe {
            let _ = CString::from_raw(s);
        }
    }
}

/// Check if tokenizer is valid
#[no_mangle]
pub extern "C" fn tokenizer_is_valid(handle: *const TokenizerHandle) -> bool {
    !handle.is_null()
}

/// Get tokenizer info (model type, vocab size, etc.) - HuggingFace compatible
#[no_mangle]
pub extern "C" fn tokenizer_get_info(
    handle: *mut TokenizerHandle,
    model_type: *mut *mut c_char,
    vocab_size: *mut u32,
    added_tokens_count: *mut u32,
) -> bool {
    clear_last_error();
    
    if handle.is_null() {
        set_last_error("Tokenizer handle cannot be null");
        return false;
    }

    let tokenizer = unsafe { &(*handle).tokenizer };

    // Get model type (simplified - you might need to inspect the actual model)
    if !model_type.is_null() {
        // This is a simplified approach - in practice you might need to check the actual model type
        let model_type_str = "BPE"; // Default assumption
        match CString::new(model_type_str) {
            Ok(c_str) => unsafe { *model_type = c_str.into_raw(); },
            Err(e) => {
                set_last_error(format!("Failed to create model type string: {}", e));
                return false;
            }
        }
    }

    // Get vocabulary sizes
    if !vocab_size.is_null() {
        unsafe { *vocab_size = tokenizer.get_vocab_size(false) as u32; }
    }
    
    if !added_tokens_count.is_null() {
        let total_size = tokenizer.get_vocab_size(true) as u32;
        let base_size = tokenizer.get_vocab_size(false) as u32;
        unsafe { *added_tokens_count = total_size - base_size; }
    }

    true
}