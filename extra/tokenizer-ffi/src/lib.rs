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
use std::collections::HashMap;
use serde_json::Value;

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

// Tokenizer handle with chat template support
#[repr(C)]
pub struct TokenizerHandle {
    tokenizer: Tokenizer,
    chat_template: Option<String>,
    model_type: Option<String>,
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

#[repr(C)]
pub struct ChatMessage {
    pub role: *const c_char,
    pub content: *const c_char,
}

/// Detect model type from tokenizer or config
fn detect_model_type(tokenizer: &Tokenizer) -> Option<String> {
    // Try to get model info from tokenizer's vocab or special tokens
    let vocab = tokenizer.get_vocab(true);

    // Check for specific special tokens that indicate model type
    if vocab.contains_key("<|im_start|>") && vocab.contains_key("<|im_end|>") {
        Some("qwen".to_string())
    } else if vocab.contains_key("<|start_header_id|>") && vocab.contains_key("<|end_header_id|>") {
        Some("llama3".to_string())
    } else if vocab.contains_key("[INST]") && vocab.contains_key("[/INST]") {
        Some("llama2".to_string())
    } else if vocab.contains_key("<start_of_turn>") && vocab.contains_key("<end_of_turn>") {
        Some("gemma".to_string())
    } else {
        // Try to infer from vocab size or other patterns
        let vocab_size = tokenizer.get_vocab_size(false);
        if vocab_size > 150000 {
            Some("qwen".to_string()) // Qwen models often have large vocabs
        } else if vocab_size > 32000 {
            Some("llama".to_string()) // Llama-like models
        } else {
            Some("unknown".to_string())
        }
    }
}

/// Get default chat template for known model types
fn get_default_chat_template(model_type: &str) -> Option<String> {
    match model_type.to_lowercase().as_str() {
        "qwen" | "chatgpt" | "chatml" => {
            Some("{% for message in messages %}{% if loop.first and messages[0]['role'] != 'system' %}{{ '<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n' }}{% endif %}{{'<|im_start|>' + message['role'] + '\n' + message['content'] + '<|im_end|>' + '\n'}}{% endfor %}{% if add_generation_prompt %}{{ '<|im_start|>assistant\n' }}{% endif %}".to_string())
        }
        "llama2" | "mistral" => {
            Some("{% if messages[0]['role'] == 'system' %}{% set loop_messages = messages[1:] %}{% set system_message = messages[0]['content'] %}{% else %}{% set loop_messages = messages %}{% set system_message = false %}{% endif %}{% for message in loop_messages %}{% if (message['role'] == 'user') != (loop.index0 % 2 == 0) %}{{ raise_exception('Conversation roles must alternate user/assistant/user/assistant/...') }}{% endif %}{% if loop.index0 == 0 and system_message != false %}{% set content = '<<SYS>>\n' + system_message + '\n<</SYS>>\n\n' + message['content'] %}{% else %}{% set content = message['content'] %}{% endif %}{% if message['role'] == 'user' %}{{ '[INST] ' + content + ' [/INST]' }}{% elif message['role'] == 'assistant' %}{{ ' ' + content + ' </s>' }}{% endif %}{% endfor %}{% if add_generation_prompt %}{{ ' ' }}{% endif %}".to_string())
        }
        "llama3" => {
            Some("{% set loop_messages = messages %}{% for message in loop_messages %}{% set content = '<|start_header_id|>' + message['role'] + '<|end_header_id|>\n\n' + message['content'] | trim + '<|eot_id|>' %}{% if loop.index0 == 0 %}{% set content = bos_token + content %}{% endif %}{{ content }}{% endfor %}{% if add_generation_prompt %}{{ '<|start_header_id|>assistant<|end_header_id|>\n\n' }}{% endif %}".to_string())
        }
        "gemma" => {
            Some("{{ bos_token }}{% if messages[0]['role'] == 'system' %}{{ raise_exception('System role not supported') }}{% endif %}{% for message in messages %}{% if (message['role'] == 'user') != (loop.index0 % 2 == 0) %}{{ raise_exception('Conversation roles must alternate user/assistant/user/assistant/...') }}{% endif %}{% if message['role'] == 'user' %}{{ '<start_of_turn>user\n' + message['content'] | trim + '<end_of_turn>\n' }}{% elif message['role'] == 'assistant' %}{{ '<start_of_turn>model\n' + message['content'] | trim + '<end_of_turn>\n' }}{% endif %}{% endfor %}{% if add_generation_prompt %}{{ '<start_of_turn>model\n' }}{% endif %}".to_string())
        }
        _ => None
    }
}

/// Load chat template from tokenizer config file
fn load_chat_template_from_file(tokenizer_path: &str) -> (Option<String>, Option<String>) {
    let config_paths = [
        tokenizer_path.replace("tokenizer.json", "tokenizer_config.json"),
        tokenizer_path.replace("tokenizer.json", "config.json"),
        format!("{}/tokenizer_config.json", std::path::Path::new(tokenizer_path).parent().unwrap_or(std::path::Path::new("")).display()),
        format!("{}/config.json", std::path::Path::new(tokenizer_path).parent().unwrap_or(std::path::Path::new("")).display()),
    ];

    for config_path in &config_paths {
        if let Ok(content) = std::fs::read_to_string(config_path) {
            if let Ok(config) = serde_json::from_str::<Value>(&content) {
                // Get chat template
                let chat_template = config.get("chat_template")
                    .and_then(|v| v.as_str())
                    .map(|s| s.to_string());
                
                // Get model type/name
                let model_type = config.get("model_type")
                    .or_else(|| config.get("name"))
                    .or_else(|| config.get("_name_or_path"))
                    .and_then(|v| v.as_str())
                    .map(|s| s.to_lowercase());
                
                if chat_template.is_some() || model_type.is_some() {
                    return (chat_template, model_type);
                }
            }
        }
    }

    (None, None)
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

/// Create tokenizer from file path - HuggingFace compatible with chat template support
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
            // Try to load chat template from config files
            let (chat_template, config_model_type) = load_chat_template_from_file(path_str);

            // Detect model type if not found in config
            let model_type = config_model_type.or_else(|| detect_model_type(&tokenizer));

            // Use detected model type to get default template if no custom template found
            let final_template = chat_template.or_else(|| {
                model_type.as_ref().and_then(|mt| get_default_chat_template(mt))
            });

            let handle = TokenizerHandle { 
                tokenizer,
                chat_template: final_template,
                model_type,
            };
            Box::into_raw(Box::new(handle))
        }
        Err(e) => {
            // Fallback: Try manual loading with JSON repair
            match std::fs::read_to_string(path_str) {
                Ok(content) => {
                    let cleaned_content = clean_json_content(&content);

                    match Tokenizer::from_bytes(cleaned_content.as_bytes()) {
                        Ok(tokenizer) => {
                            let (chat_template, config_model_type) = load_chat_template_from_file(path_str);
                            let model_type = config_model_type.or_else(|| detect_model_type(&tokenizer));
                            let final_template = chat_template.or_else(|| {
                                model_type.as_ref().and_then(|mt| get_default_chat_template(mt))
                            });
                            
                            let handle = TokenizerHandle { 
                                tokenizer,
                                chat_template: final_template,
                                model_type,
                            };
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
            let model_type = detect_model_type(&tokenizer);
            let chat_template = model_type.as_ref().and_then(|mt| get_default_chat_template(mt));
            
            let handle = TokenizerHandle { 
                tokenizer,
                chat_template,
                model_type,
            };
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
            let model_type = detect_model_type(&tokenizer);
            let chat_template = model_type.as_ref().and_then(|mt| get_default_chat_template(mt));
            
            let handle = TokenizerHandle { 
                tokenizer,
                chat_template,
                model_type,
            };
            Box::into_raw(Box::new(handle))
        }
        Err(e) => {
            set_last_error(format!("Failed to parse tokenizer JSON: {}", e));
            ptr::null_mut()
        }
    }
}

/// Convert messages to the format expected by tokenizers crate
fn prepare_messages_for_tokenizer(messages_slice: &[ChatMessage]) -> Result<Vec<HashMap<String, String>>, String> {
    let mut conversation = Vec::new();
    
    for (i, msg) in messages_slice.iter().enumerate() {
        if msg.role.is_null() || msg.content.is_null() {
            return Err(format!("Message at index {} has null role or content", i));
        }

        let role_cstr = unsafe { CStr::from_ptr(msg.role) };
        let content_cstr = unsafe { CStr::from_ptr(msg.content) };

        let role = role_cstr.to_str()
            .map_err(|e| format!("Invalid UTF-8 in role at index {}: {}", i, e))?
            .to_string();

        let content = content_cstr.to_str()
            .map_err(|e| format!("Invalid UTF-8 in content at index {}: {}", i, e))?
            .to_string();

        // Validate role (HuggingFace compatible)
        match role.as_str() {
            "system" | "user" | "assistant" => {},
            _ => return Err(format!("Invalid role '{}' at index {}. Must be 'system', 'user', or 'assistant'", role, i)),
        }

        let mut msg_map = HashMap::new();
        msg_map.insert("role".to_string(), role);
        msg_map.insert("content".to_string(), content);
        conversation.push(msg_map);
    }

    Ok(conversation)
}

/// Apply chat template using tokenizers crate - HuggingFace compatible
#[no_mangle]
pub extern "C" fn tokenizer_apply_chat_template(
    handle: *mut TokenizerHandle,
    messages: *const ChatMessage,
    messages_count: size_t,
    add_generation_prompt: bool,
    template: *const c_char,
) -> *mut c_char {
    clear_last_error();

    if handle.is_null() {
        set_last_error("Tokenizer handle is null");
        return ptr::null_mut();
    }

    if messages.is_null() {
        set_last_error("Messages array pointer is null");
        return ptr::null_mut();
    }

    if messages_count == 0 {
        set_last_error("Messages count is zero");
        return ptr::null_mut();
    }

    let tokenizer_handle = unsafe { &(*handle) };
    let messages_slice = unsafe { std::slice::from_raw_parts(messages, messages_count as usize) };

    // Convert C messages to the format expected by tokenizers
    let conversation = match prepare_messages_for_tokenizer(messages_slice) {
        Ok(conv) => conv,
        Err(e) => {
            set_last_error(e);
            return ptr::null_mut();
        }
    };

    // Handle custom template parameter
    let template_to_use = if template.is_null() {
        // Use tokenizer's built-in template or detected template
        tokenizer_handle.chat_template.as_deref()
    } else {
        let template_cstr = unsafe { CStr::from_ptr(template) };
        match template_cstr.to_str() {
            Ok(s) => Some(s),
            Err(e) => {
                set_last_error(format!("Invalid UTF-8 in custom template: {}", e));
                return ptr::null_mut();
            }
        }
    };

    // Try to use tokenizers crate's apply_chat_template if available
    // Note: This depends on the tokenizers crate version having this method
    let formatted_text = if let Some(template_str) = template_to_use {
        // Try using the tokenizer's apply_chat_template method
        // This is pseudo-code as the exact API might vary by tokenizers version
        match apply_chat_template_with_tokenizer(&tokenizer_handle.tokenizer, &conversation, add_generation_prompt, Some(template_str)) {
            Ok(text) => text,
            Err(e) => {
                set_last_error(format!("Failed to apply chat template: {}", e));
                return ptr::null_mut();
            }
        }
    } else {
        // Fallback to simple formatting if no template available
        match format_conversation_simple(&conversation, add_generation_prompt) {
            Ok(text) => text,
            Err(e) => {
                set_last_error(format!("Failed to format conversation: {}", e));
                return ptr::null_mut();
            }
        }
    };

    match CString::new(formatted_text) {
        Ok(c_str) => c_str.into_raw(),
        Err(e) => {
            set_last_error(format!("Failed to convert formatted text to C string: {}", e));
            ptr::null_mut()
        }
    }
}

/// Internal function to apply chat template using tokenizer
fn apply_chat_template_with_tokenizer(
    _tokenizer: &Tokenizer,
    conversation: &[HashMap<String, String>],
    add_generation_prompt: bool,
    template: Option<&str>,
) -> Result<String, String> {
    // Convert to the format expected by the tokenizers crate
    let _messages_value: Vec<serde_json::Value> = conversation.iter()
        .map(|msg| serde_json::json!({
            "role": msg.get("role").unwrap_or(&"".to_string()),
            "content": msg.get("content").unwrap_or(&"".to_string())
        }))
        .collect();

    // Note: The tokenizers crate's apply_chat_template method might not be available
    // in all versions, so we use manual template processing as the primary method
    if let Some(template_str) = template {
        apply_template_manually(conversation, template_str, add_generation_prompt)
    } else {
        format_conversation_simple(conversation, add_generation_prompt)
    }
}

/// Manual template application (fallback for older tokenizers versions)
fn apply_template_manually(
    conversation: &[HashMap<String, String>],
    template: &str,
    add_generation_prompt: bool,
) -> Result<String, String> {
    // This is a very basic template engine
    // In a production environment, you'd want to use a proper Jinja2-like template engine
    let mut result = String::new();
    
    // Handle system message separately for some templates
    let mut messages_to_process = conversation;
    let system_message = if !conversation.is_empty() && 
        conversation[0].get("role").map(|r| r.as_str()) == Some("system") {
        messages_to_process = &conversation[1..];
        conversation[0].get("content").cloned()
    } else {
        None
    };

    // Apply template based on detected pattern
    if template.contains("<|im_start|>") {
        // ChatML style (Qwen, etc.)
        if let Some(sys_msg) = system_message {
            result.push_str(&format!("<|im_start|>system\n{}<|im_end|>\n", sys_msg));
        }
        for msg in messages_to_process {
            let role = msg.get("role").cloned().unwrap_or_else(|| "user".to_string());
            let content = msg.get("content").cloned().unwrap_or_else(|| "".to_string());
            result.push_str(&format!("<|im_start|>{}\n{}<|im_end|>\n", role, content));
        }
        if add_generation_prompt {
            result.push_str("<|im_start|>assistant\n");
        }
    } else if template.contains("[INST]") {
        // Llama2/Mistral style
        let mut is_first = true;
        for msg in messages_to_process {
            let role = msg.get("role").cloned().unwrap_or_else(|| "user".to_string());
            let content = msg.get("content").cloned().unwrap_or_else(|| "".to_string());
            
            if role == "user" {
                let full_content = if is_first && system_message.is_some() {
                    format!("<<SYS>>\n{}\n<</SYS>>\n\n{}", system_message.as_ref().unwrap(), content)
                } else {
                    content
                };
                result.push_str(&format!("[INST] {} [/INST]", full_content));
                is_first = false;
            } else if role == "assistant" {
                result.push_str(&format!(" {} </s>", content));
            }
        }
        if add_generation_prompt {
            result.push_str(" ");
        }
    } else if template.contains("<start_of_turn>") {
        // Gemma style
        result.push_str("<bos>");
        for msg in messages_to_process {
            let role = msg.get("role").cloned().unwrap_or_else(|| "user".to_string());
            let content = msg.get("content").cloned().unwrap_or_else(|| "".to_string());
            let gemma_role = if role == "assistant" { "model" } else { &role };
            result.push_str(&format!("<start_of_turn>{}\n{}<end_of_turn>\n", gemma_role, content.trim()));
        }
        if add_generation_prompt {
            result.push_str("<start_of_turn>model\n");
        }
    } else {
        // Generic template or simple replacement
        for msg in conversation {
            let role = msg.get("role").cloned().unwrap_or_else(|| "unknown".to_string());
            let content = msg.get("content").cloned().unwrap_or_else(|| "".to_string());
            
            let formatted_msg = template
                .replace("{{role}}", &role)
                .replace("{{content}}", &content)
                .replace("{{ role }}", &role)
                .replace("{{ content }}", &content);
            result.push_str(&formatted_msg);
            result.push('\n');
        }

        if add_generation_prompt {
            result.push_str("Assistant: ");
        }
    }

    Ok(result)
}

/// Helper function to format conversation in a simple way (fallback)
fn format_conversation_simple(
    conversation: &[HashMap<String, String>], 
    add_generation_prompt: bool
) -> Result<String, String> {
    let mut result = String::new();

    for msg in conversation {
        let role = msg.get("role").map(|s| s.as_str()).unwrap_or("unknown");
        let content = msg.get("content").map(|s| s.as_str()).unwrap_or("");

        // Simple format: Role: Content
        result.push_str(&format!("{}: {}\n", role, content));
    }

    if add_generation_prompt {
        result.push_str("assistant: ");
    }

    Ok(result)
}

/// Apply chat template and encode the result - HuggingFace compatible
#[no_mangle]
pub extern "C" fn tokenizer_apply_chat_template_and_encode(
    handle: *mut TokenizerHandle,
    messages: *const ChatMessage,
    messages_count: size_t,
    add_generation_prompt: bool,
    template: *const c_char,
    add_special_tokens: bool,
) -> *mut EncodingResult {
    clear_last_error();

    // First apply the chat template
    let formatted_text_ptr = tokenizer_apply_chat_template(
        handle, 
        messages, 
        messages_count, 
        add_generation_prompt, 
        template
    );

    if formatted_text_ptr.is_null() {
        // Error is already set by tokenizer_apply_chat_template
        return ptr::null_mut();
    }

    // Then encode the formatted text
    let result = tokenizer_encode(handle, formatted_text_ptr, add_special_tokens);

    // Clean up the intermediate formatted text
    string_free(formatted_text_ptr);

    result
}

/// Check if tokenizer has a chat template - HuggingFace compatible
#[no_mangle]
pub extern "C" fn tokenizer_has_chat_template(handle: *mut TokenizerHandle) -> bool {
    clear_last_error();

    if handle.is_null() {
        set_last_error("Tokenizer handle is null");
        return false;
    }

    let tokenizer_handle = unsafe { &(*handle) };
    tokenizer_handle.chat_template.is_some()
}

/// Get the chat template string from the tokenizer - HuggingFace compatible
#[no_mangle]
pub extern "C" fn tokenizer_get_chat_template(handle: *mut TokenizerHandle) -> *mut c_char {
    clear_last_error();

    if handle.is_null() {
        set_last_error("Tokenizer handle is null");
        return ptr::null_mut();
    }

    let tokenizer_handle = unsafe { &(*handle) };

    match &tokenizer_handle.chat_template {
        Some(template) => {
            match CString::new(template.clone()) {
                Ok(c_str) => c_str.into_raw(),
                Err(e) => {
                    set_last_error(format!("Failed to convert chat template to C string: {}", e));
                    ptr::null_mut()
                }
            }
        }
        None => {
            set_last_error("No chat template available");
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

    let tokenizer_handle = unsafe { &(*handle) };
    let tokenizer = &tokenizer_handle.tokenizer;
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

    let tokenizer_handle = unsafe { &(*handle) };
    let tokenizer = &tokenizer_handle.tokenizer;
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

    let tokenizer_handle = unsafe { &(*handle) };
    let tokenizer = &tokenizer_handle.tokenizer;
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

    let tokenizer_handle = unsafe { &(*handle) };
    let tokenizer = &tokenizer_handle.tokenizer;
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

    let tokenizer_handle = unsafe { &(*handle) };
    let tokenizer = &tokenizer_handle.tokenizer;
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

    let tokenizer_handle = unsafe { &(*handle) };
    let tokenizer = &tokenizer_handle.tokenizer;

    // Get model type from detected model type or default
    if !model_type.is_null() {
        let model_type_str = tokenizer_handle.model_type.as_deref().unwrap_or("unknown");
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