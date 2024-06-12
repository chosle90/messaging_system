/*****************************************************************************
BSD 3-Clause License

Copyright (c) 2021, 🍀☀🌕🌥 🌊
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*****************************************************************************/

#include "file_handler.h"

#include "converting.h"

#include <fcntl.h>
#include <filesystem>
#include <fstream>

#include <string>

namespace file_handler
{
	using namespace converting;

	auto file::remove(const std::string& path) -> bool
	{
		if (!std::filesystem::exists(path))
		{
			return false;
		}

		return std::filesystem::remove(path);
	}

	auto file::load(const std::string& path) -> std::vector<uint8_t>
	{
		if (!std::filesystem::exists(path))
		{
			return std::vector<uint8_t>();
		}

		std::ifstream stream(path, std::ios::binary);
		if (!stream.is_open())
		{
			return std::vector<uint8_t>();
		}

		std::vector<uint8_t> target((std::istreambuf_iterator<char>(stream)),
									std::istreambuf_iterator<char>());
		stream.close();

		return target;
	}

	auto file::save(const std::string& path,
					const std::vector<uint8_t>& data) -> bool
	{
		std::filesystem::path target_path(path);
		if (target_path.parent_path().empty() != true)
		{
			std::filesystem::create_directories(target_path.parent_path());
		}

		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream.is_open())
		{
			return false;
		}

		stream.write((char*)data.data(), (unsigned int)data.size());
		stream.close();

		return true;
	}

	auto file::append(const std::string& source,
					  const std::vector<uint8_t>& data) -> bool
	{
		std::fstream stream(source,
							std::ios::out | std::ios::binary | std::ios::app);
		if (!stream.is_open())
		{
			return false;
		}

		stream.write((char*)data.data(), (unsigned int)data.size());
		stream.close();

		return true;
	}
} // namespace file_handler
