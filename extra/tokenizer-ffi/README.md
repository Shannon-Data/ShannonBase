
This is used to wrapper HuggingFace Tokenizer Rust to C++ SDK. It wrapper the Rust interface to C++ compatible. Therefore, ShannonBase
can load the `token.json` directly in HuggingFace format. And run the LLM model with ONNXRuntime framework.

If you rebuild the libs and header files. then use `cargo build --release -v` to generate the lib files and header files. 
then copy the libs fiels: libxxx.a , libxxx.so from `./target/realeas/`
to `./lib/xxx`.
