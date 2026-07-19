/*
Copyright (c) 2018-2019, tevador <tevador@gmail.com>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
	* Redistributions of source code must retain the above copyright
	  notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.
	* Neither the name of the copyright holder nor the
	  names of its contributors may be used to endorse or promote products
	  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <sstream>
#include <cmath>
#include <cfloat>
#include "vm_interpreted.hpp"
#include "dataset.hpp"
#include "intrin_portable.h"
#include "reciprocal.h"
#include "soft_aes.h"

namespace randomx {

	template<class Allocator, bool softAes>
	void InterpretedVm<Allocator, softAes>::setDataset(randomx_dataset* dataset) {
		datasetPtr = dataset;
		mem.memory = dataset->memory;
	}

	template<class Allocator, bool softAes>
	void InterpretedVm<Allocator, softAes>::run(void* seed) {
		VmBase<Allocator, softAes>::generateProgram(seed);
		randomx_vm::initialize();
		execute();
	}

	template<class Allocator, bool softAes>
	void InterpretedVm<Allocator, softAes>::execute() {
		static int trace_count = 0;
		bool is_target = (trace_count < 50);
		static bool trace_printed = false;
		if (is_target) {
			std::cout << "[RefTrace] count=" << trace_count << " mx_=" << std::hex << mem.mx << " ma_=" << mem.ma
			          << " read_reg0_=" << config.readReg0 << " read_reg1_=" << config.readReg1
			          << " read_reg2_=" << config.readReg2 << " read_reg3_=" << config.readReg3
			          << " dataset_offset_=" << datasetOffset << std::dec << std::endl;
			std::cout << "[RefTrace] entropy:";
			for (int i = 0; i < 16; ++i) {
				std::cout << " " << std::hex << program.getEntropy(i);
			}
			std::cout << std::dec << std::endl;
			trace_count++;
			trace_printed = false;
		}

		NativeRegisterFile nreg;

		for(unsigned i = 0; i < RegisterCountFlt; ++i)
			nreg.a[i] = rx_load_vec_f128(&reg.a[i].lo);

		if (is_target) {
			std::cout << "[RefTrace] program opcodes:";
			for (int i = 0; i < 256; ++i) {
				std::cout << " " << std::hex << (int)program(i).opcode;
			}
			std::cout << std::dec << std::endl;
		}
		compileProgram(program, bytecode, nreg, randomx_vm::vmFlags);

		uint32_t spAddr0 = mem.mx;
		uint32_t spAddr1 = mem.ma;

		for(unsigned ic = 0; ic < RANDOMX_PROGRAM_ITERATIONS; ++ic) {
			uint64_t spMix = nreg.r[config.readReg0] ^ nreg.r[config.readReg1];
			spAddr0 ^= spMix;
			spAddr0 &= ScratchpadL3Mask64;
			spAddr1 ^= spMix >> 32;
			spAddr1 &= ScratchpadL3Mask64;
			
			for (unsigned i = 0; i < RegistersCount; ++i)
				nreg.r[i] ^= load64(scratchpad + spAddr0 + 8 * i);

			for (unsigned i = 0; i < RegisterCountFlt; ++i)
				nreg.f[i] = rx_cvt_packed_int_vec_f128(scratchpad + spAddr1 + 8 * i);

			for (unsigned i = 0; i < RegisterCountFlt; ++i)
				nreg.e[i] = maskRegisterExponentMantissa(config, rx_cvt_packed_int_vec_f128(scratchpad + spAddr1 + 8 * (RegisterCountFlt + i)));

			if (is_target && !trace_printed && ic == 0) {
				std::cout << "[RefTrace] Before executeBytecode ic=0 reg_r=" << std::hex
				          << nreg.r[0] << " " << nreg.r[1] << " " << nreg.r[2] << " " << nreg.r[3] << " "
				          << nreg.r[4] << " " << nreg.r[5] << " " << nreg.r[6] << " " << nreg.r[7] << std::dec << std::endl;
			}

			executeBytecode(bytecode, scratchpad, config, randomx_vm::getFlags());

			if (is_target && !trace_printed && ic == 0) {
				std::cout << "[RefTrace] After executeBytecode ic=0 reg_r=" << std::hex
				          << nreg.r[0] << " " << nreg.r[1] << " " << nreg.r[2] << " " << nreg.r[3] << " "
				          << nreg.r[4] << " " << nreg.r[5] << " " << nreg.r[6] << " " << nreg.r[7] << std::dec << std::endl;
				trace_printed = true;
			}

			const uint64_t readPtr = datasetOffset + (mem.ma & CacheLineAlignMask);

			auto& mp = (randomx_vm::getFlags() & RANDOMX_FLAG_V2) ? mem.ma : mem.mx;
			mp ^= nreg.r[config.readReg2] ^ nreg.r[config.readReg3];

			datasetPrefetch(datasetOffset + (mp & CacheLineAlignMask));
			datasetRead(readPtr, nreg.r);
			std::swap(mem.mx, mem.ma);

			for (unsigned i = 0; i < RegistersCount; ++i)
				store64(scratchpad + spAddr1 + 8 * i, nreg.r[i]);

			if (randomx_vm::getFlags() & RANDOMX_FLAG_V2) {
				rx_vec_i128 ekey[RegisterCountFlt];
				rx_vec_i128 freg[RegisterCountFlt];

				for (unsigned i = 0; i < RegisterCountFlt; ++i) {
					ekey[i] = rx_cast_vec_f2i(nreg.e[i]);
					freg[i] = rx_cast_vec_f2i(nreg.f[i]);
				}

				for (unsigned i = 0; i < RegisterCountFlt; ++i) {
					freg[0] = aesenc<softAes>(freg[0], ekey[i]);
					freg[1] = aesdec<softAes>(freg[1], ekey[i]);
					freg[2] = aesenc<softAes>(freg[2], ekey[i]);
					freg[3] = aesdec<softAes>(freg[3], ekey[i]);
				}

				for (unsigned i = 0; i < RegisterCountFlt; ++i)
					nreg.f[i] = rx_cast_vec_i2f(freg[i]);
			}
			else {
				for (unsigned i = 0; i < RegisterCountFlt; ++i)
					nreg.f[i] = rx_xor_vec_f128(nreg.f[i], nreg.e[i]);
			}

			for (unsigned i = 0; i < RegisterCountFlt; ++i)
				rx_store_vec_f128((double*)(scratchpad + spAddr0 + 16 * i), nreg.f[i]);

			spAddr0 = 0;
			spAddr1 = 0;
		}

		for (unsigned i = 0; i < RegistersCount; ++i)
			store64(&reg.r[i], nreg.r[i]);

		for (unsigned i = 0; i < RegisterCountFlt; ++i)
			rx_store_vec_f128(&reg.f[i].lo, nreg.f[i]);

		for (unsigned i = 0; i < RegisterCountFlt; ++i)
			rx_store_vec_f128(&reg.e[i].lo, nreg.e[i]);
	}

	template<class Allocator, bool softAes>
	void InterpretedVm<Allocator, softAes>::datasetRead(uint64_t address, int_reg_t(&r)[RegistersCount]) {
		uint64_t* datasetLine = (uint64_t*)(mem.memory + address);
		for (int i = 0; i < RegistersCount; ++i)
			r[i] ^= datasetLine[i];
	}

	template<class Allocator, bool softAes>
	void InterpretedVm<Allocator, softAes>::datasetPrefetch(uint64_t address) {
		rx_prefetch_nta(mem.memory + address);
	}

	template class InterpretedVm<AlignedAllocator<CacheLineSize>, false>;
	template class InterpretedVm<AlignedAllocator<CacheLineSize>, true>;
	template class InterpretedVm<LargePageAllocator, false>;
	template class InterpretedVm<LargePageAllocator, true>;
}