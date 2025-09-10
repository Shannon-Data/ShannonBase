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
*/

use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;
use tokenizers::Tokenizer;
use libc::size_t;

// Thread-local error storage
thread_local! {
    static LAST_ERROR: RefCell<Option<CString>> = RefCell::new(None);
}

/// Set the last error message
fn set_last_error(msg: impl Into<String>) {
    let error_msg = msg.into();
    LAST_ERROR.with(|e| {
        *e.borrow_mut() = CString::new(error_msg).ok();
    });
}

/// Clear the last error
fn clear_last_error() {
    LAST_ERROR.with(|e| {
        *e.borrow_mut() = None;
    });
}

/// Get the last error message
/// Returns a pointer to the error string, or null if no error
/// The returned pointer is valid until the next call to any tokenizer function
/// on the same thread, or until the thread exits
#[no_mangle]
pub extern "C" fn tokenizer_get_last_error() -> *const c_char {
    LAST_ERROR.with(|e| {
        if let Some(ref err) = *e.borrow() {
            err.as_ptr()
        } else {
            ptr::null()
        }
    })
}

/// Check if there's a pending error
#[no_mangle]
pub extern "C" fn tokenizer_has_error() -> bool {
    LAST_ERROR.with(|e| e.borrow().is_some())
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

// Encoding result
#[repr(C)]
pub struct EncodingResult {
    pub ids: *mut u32,
    pub length: size_t,
    pub attention_mask: *mut u32,
    pub tokens: *mut *mut c_char,
}

/// Create tokenizer from file
#[no_mangle]
pub extern "C" fn tokenizer_from_file(path: *const c_char) -> *mut TokenizerHandle {
    clear_last_error();
    
    if path.is_null() {
        set_last_error("Path pointer is null");
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

    match Tokenizer::from_file(path_str) {
        Ok(tokenizer) => {
            let handle = TokenizerHandle { tokenizer };
            Box::into_raw(Box::new(handle))
        }
        Err(e) => {
            set_last_error(format!("Failed to load tokenizer from file '{}': {}", path_str, e));
            ptr::null_mut()
        }
    }
}

/// Create tokenizer from JSON string
#[no_mangle]
pub extern "C" fn tokenizer_from_json(json: *const c_char) -> *mut TokenizerHandle {
    clear_last_error();
    
    if json.is_null() {
        set_last_error("JSON pointer is null");
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

    match Tokenizer::from_bytes(json_str.as_bytes()) {
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

/// Encode text
#[no_mangle]
pub extern "C" fn tokenizer_encode(
    handle: *mut TokenizerHandle,
    text: *const c_char,
    add_special_tokens: bool,
) -> *mut EncodingResult {
    clear_last_error();
    
    if handle.is_null() {
        set_last_error("Tokenizer handle is null");
        return ptr::null_mut();
    }
    
    if text.is_null() {
        set_last_error("Text pointer is null");
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
            let length = encoding.len();
            let ids = encoding.get_ids().to_vec();
            let attention_mask = encoding.get_attention_mask().to_vec();
            let tokens = encoding.get_tokens();

            let mut ids_vec = ids;
            let ids_ptr = ids_vec.as_mut_ptr();
            std::mem::forget(ids_vec);

            let mut attention_mask_vec = attention_mask;
            let attention_mask_ptr = attention_mask_vec.as_mut_ptr();
            std::mem::forget(attention_mask_vec);

            let mut tokens_ptrs: Vec<*mut c_char> = Vec::with_capacity(length);
            for token in tokens {
                match CString::new(token.as_str()) {
                    Ok(c_str) => tokens_ptrs.push(c_str.into_raw()),
                    Err(e) => {
                        set_last_error(format!("Failed to convert token to C string: {}", e));
                        // Clean up allocated memory
                        unsafe {
                            let _ = Vec::from_raw_parts(ids_ptr, 0, length);
                            let _ = Vec::from_raw_parts(attention_mask_ptr, 0, length);
                            for ptr in tokens_ptrs {
                                let _ = CString::from_raw(ptr);
                            }
                        }
                        return ptr::null_mut();
                    }
                }
            }

            let tokens_ptr = tokens_ptrs.as_mut_ptr();
            std::mem::forget(tokens_ptrs);

            Box::into_raw(Box::new(EncodingResult {
                ids: ids_ptr,
                length: length as size_t,
                attention_mask: attention_mask_ptr,
                tokens: tokens_ptr,
            }))
        }
        Err(e) => {
            set_last_error(format!("Failed to encode text: {}", e));
            ptr::null_mut()
        }
    }
}

/// Batch encode texts
#[no_mangle]
pub extern "C" fn tokenizer_encode_batch(
    handle: *mut TokenizerHandle,
    texts: *const *const c_char,
    count: size_t,
    add_special_tokens: bool,
) -> *mut *mut EncodingResult {
    clear_last_error();
    
    if handle.is_null() {
        set_last_error("Tokenizer handle is null");
        return ptr::null_mut();
    }
    
    if texts.is_null() {
        set_last_error("Texts array pointer is null");
        return ptr::null_mut();
    }
    
    if count == 0 {
        set_last_error("Text count is zero");
        return ptr::null_mut();
    }

    let tokenizer = unsafe { &(*handle).tokenizer };
    let text_slice = unsafe { std::slice::from_raw_parts(texts, count as usize) };

    let mut text_strings = Vec::with_capacity(count as usize);
    for (i, &text_ptr) in text_slice.iter().enumerate() {
        if text_ptr.is_null() {
            set_last_error(format!("Text pointer at index {} is null", i));
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

    match tokenizer.encode_batch(text_strings, add_special_tokens) {
        Ok(encodings) => {
            let results: Result<Vec<*mut EncodingResult>, String> = encodings
                .into_iter()
                .enumerate()
                .map(|(i, encoding)| {
                    let length = encoding.len();
                    let ids = encoding.get_ids().to_vec();
                    let attention_mask = encoding.get_attention_mask().to_vec();
                    let tokens = encoding.get_tokens();

                    let mut ids_vec = ids;
                    let ids_ptr = ids_vec.as_mut_ptr();
                    std::mem::forget(ids_vec);

                    let mut attention_mask_vec = attention_mask;
                    let attention_mask_ptr = attention_mask_vec.as_mut_ptr();
                    std::mem::forget(attention_mask_vec);

                    let mut tokens_ptrs: Vec<*mut c_char> = Vec::with_capacity(length);
                    for token in tokens {
                        match CString::new(token.as_str()) {
                            Ok(c_str) => tokens_ptrs.push(c_str.into_raw()),
                            Err(e) => {
                                // Clean up allocated memory
                                unsafe {
                                    let _ = Vec::from_raw_parts(ids_ptr, 0, length);
                                    let _ = Vec::from_raw_parts(attention_mask_ptr, 0, length);
                                    for ptr in &tokens_ptrs {
                                        let _ = CString::from_raw(*ptr);
                                    }
                                }
                                return Err(format!("Failed to convert token to C string at batch index {}: {}", i, e));
                            }
                        }
                    }

                    let tokens_ptr = tokens_ptrs.as_mut_ptr();
                    std::mem::forget(tokens_ptrs);

                    Ok(Box::into_raw(Box::new(EncodingResult {
                        ids: ids_ptr,
                        length: length as size_t,
                        attention_mask: attention_mask_ptr,
                        tokens: tokens_ptr,
                    })))
                })
                .collect();

            match results {
                Ok(results_vec) => {
                    let mut results_vec = results_vec;
                    let ptr = results_vec.as_mut_ptr();
                    std::mem::forget(results_vec);
                    ptr
                }
                Err(e) => {
                    set_last_error(e);
                    ptr::null_mut()
                }
            }
        }
        Err(e) => {
            set_last_error(format!("Failed to encode batch: {}", e));
            ptr::null_mut()
        }
    }
}

/// Decode token IDs
#[no_mangle]
pub extern "C" fn tokenizer_decode(
    handle: *mut TokenizerHandle,
    ids: *const u32,
    length: size_t,
    skip_special_tokens: bool,
) -> *mut c_char {
    clear_last_error();
    
    if handle.is_null() {
        set_last_error("Tokenizer handle is null");
        return ptr::null_mut();
    }
    
    if ids.is_null() {
        set_last_error("IDs array pointer is null");
        return ptr::null_mut();
    }
    
    if length == 0 {
        set_last_error("IDs array length is zero");
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

/// Get vocabulary size
#[no_mangle]
pub extern "C" fn tokenizer_get_vocab_size(handle: *mut TokenizerHandle) -> u32 {
    clear_last_error();
    
    if handle.is_null() {
        set_last_error("Tokenizer handle is null");
        return 0;
    }

    let tokenizer = unsafe { &(*handle).tokenizer };
    tokenizer.get_vocab_size(false) as u32
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
        
        // Free IDs
        if !result.ids.is_null() {
            let _ = Vec::from_raw_parts(result.ids, result.length as usize, result.length as usize);
        }
        
        // Free attention mask
        if !result.attention_mask.is_null() {
            let _ = Vec::from_raw_parts(result.attention_mask, result.length as usize, result.length as usize);
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
    }
}

/// Free encoding result array
#[no_mangle]
pub extern "C" fn encoding_result_array_free(results: *mut *mut EncodingResult, count: size_t) {
    if results.is_null() || count == 0 {
        return;
    }

    unsafe {
        let results_slice = std::slice::from_raw_parts_mut(results, count as usize);
        for i in 0..count as usize {
            if !results_slice[i].is_null() {
                encoding_result_free(results_slice[i]);
            }
        }
        let _ = Vec::from_raw_parts(results, 0, count as usize);
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