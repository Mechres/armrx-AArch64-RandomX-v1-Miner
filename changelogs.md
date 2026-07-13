# Changelog

## 2026-07-13

### Added
- Created interpreted RandomX Virtual Machine execution engine (`src/vm.cpp`, `include/armrx/vm.hpp`) including integer and floating-point registers, scratchpad reads/writes, compiler thresholds, and interpreted execution loop.
- Added software AES encryption/decryption round primitives and scratchpad filling logic (`src/aes_hash.cpp`, `include/armrx/aes_hash.hpp`).
- Added end-to-end VM hash parity tests in `tests/test_blake2b.cpp` to validate against reference inputs `"This is a test"` and `"Lorem ipsum dolor sit amet"`.
- Implemented multi-threaded `MiningEngine` (`src/mining_engine.cpp`, `include/armrx/mining_engine.hpp`) for orchestrating mining loops, thread-local VM instances, and memory mode transitions.
- Added `Target` structure and `meets_target` verification logic (`include/armrx/mining_common.hpp`) to compare computed hashes against target difficulties.
- Added target difficulty translation, signal handling, and runtime benchmark duration controls to `src/main.cpp`.
- Added unit tests `tests/test_mining.cpp` to validate target difficulty boundary checks and worker orchestration lifecycles.

### Fixed
- Fixed AES decryption round sequence (`aes_decrypt_round` in `src/aes.cpp`) to apply standard Inverse ShiftRows -> Inverse SubBytes -> Inverse MixColumns -> AddRoundKey order.
- Corrected `build_aes_block` word mapping to follow standard little-endian format.
- Modified `init_scratchpad` to update the seeding `tempHash` in-place, passing the state-modified seed to the first VM program execution.
- Corrected floating-point instruction compilation frequency thresholds (`ceil_` ceilings in `src/vm.cpp`) to align with standard configuration frequencies.
- Added zero-initialization of integer registers `reg_.r` on VM setup to prevent cross-run state leaks.
- Removed unused parameter warnings from `execute_bytecode`.

### Modified
- Updated `README.md` to document the completed interpreted VM implementation details, status, and verification test vectors.
- Updated `CMakeLists.txt` to compile `src/mining_engine.cpp` with pthread linkages, and register `test_mining` to the build and CTest validation pipeline.
