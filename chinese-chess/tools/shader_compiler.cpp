#include "file_watcher.h"
#include "shader_compiler.h"
#include <fstream>
#include <print>
#include <slang-com-helper.h>
#include <slang-com-ptr.h>
#include <ranges>

shader_compiler shader_compiler::compiler;

#ifndef NDEBUG
void shader_compiler::diagnose_if_needed(const Slang::ComPtr<slang::IBlob>& _diagnostic_blob) noexcept
{
	if (_diagnostic_blob != nullptr)
	{
		std::println("{}", reinterpret_cast<const char*>(_diagnostic_blob->getBufferPointer()));
	}
}

void shader_compiler::print_entrypoint_hashes(int _entrypoint_count, int _target_count, const Slang::ComPtr<slang::IComponentType>& _composed_program) noexcept
{
	std::ranges::for_each(std::views::iota(0, _target_count), [&](int target_index)
		{
			std::ranges::for_each(std::views::iota(0, _entrypoint_count), [&](int entrypoint_index)
				{
					Slang::ComPtr<slang::IBlob> entrypoint_hash_blob;
					_composed_program->getEntryPointHash(entrypoint_index, target_index, entrypoint_hash_blob.writeRef());

					std::stringstream str_builder{};
					std::ranges::for_each(std::span<const uint8_t>(static_cast<const uint8_t*>(entrypoint_hash_blob->getBufferPointer()), entrypoint_hash_blob->getBufferSize()),
						[&str_builder](const auto& num)
						{
							str_builder << std::format("{:02X}", num);
						});

					std::println("callIdx: {}, entrypoint: {}, target: {}, hash: {}", global_counter, entrypoint_index, target_index, str_builder.str());
					global_counter++;
				});
		});
}
#else
void shader_compiler::diagnose_if_needed(const Slang::ComPtr<slang::IBlob>& _diagnostic_blob) noexcept {}
void shader_compiler::print_entrypoint_hashes(int _entrypoint_count, int _target_count, const Slang::ComPtr<slang::IComponentType>& _composed_program) noexcept {}
#endif

shader_compiler::shader_compiler()
{
	options = {
		slang::CompilerOptionEntry{
			slang::CompilerOptionName::EmitSpirvDirectly,
			{slang::CompilerOptionValueKind::Int, true, 0, nullptr, nullptr},
		},
		slang::CompilerOptionEntry{
			slang::CompilerOptionName::VulkanUseEntryPointName,
			{slang::CompilerOptionValueKind::Int, true, 0, nullptr, nullptr},
		},
		slang::CompilerOptionEntry{
			slang::CompilerOptionName::Optimization,
			{slang::CompilerOptionValueKind::Int, SlangOptimizationLevel::SLANG_OPTIMIZATION_LEVEL_MAXIMAL, 0, nullptr, nullptr},
		},
		//slang::CompilerOptionEntry{
		//	slang::CompilerOptionName::DownstreamArgs,
		//	{slang::CompilerOptionValueKind::String, 0, 0, nullptr, nullptr},
		//},
		slang::CompilerOptionEntry{
			slang::CompilerOptionName::MatrixLayoutColumn,
			{slang::CompilerOptionValueKind::Int, true, 0, nullptr, nullptr},
		},
	};

	auto result = slang::createGlobalSession(global_session.writeRef());
	assert(SLANG_SUCCEEDED(result));

	target_desc.format = SLANG_SPIRV;
	target_desc.profile = global_session->findProfile("spirv_1_4");
	// targetDesc.flags = 0;

	session_desc.targets = &target_desc;
	session_desc.targetCount = 1;
	session_desc.compilerOptionEntries = options.data();
	session_desc.compilerOptionEntryCount = static_cast<uint32_t>(options.size());
}

std::vector<char> shader_compiler::compile_shader_to_spv(const std::filesystem::path& _shader_path, const std::vector<std::string>& _entry_name) noexcept
{
	std::filesystem::path spirv_path = _shader_path;
	spirv_path.replace_extension(".spv");

	if ((!std::filesystem::exists(_shader_path) || !FILE_WATCHER.is_file_modified(_shader_path)) && std::filesystem::exists(spirv_path))
	{
		std::ifstream in_file(spirv_path.generic_string(), std::ios::ate | std::ios::binary);

		if (in_file.is_open())
		{
			std::vector<char> buffer(in_file.tellg());
			in_file.seekg(0, std::ios::beg);
			in_file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			in_file.close();

			return buffer;
		}
	}
	else
	{
		auto desc = session_desc;
		const std::string parent_path = _shader_path.parent_path().generic_string();
		const std::array search_paths = { parent_path.c_str() };
		desc.searchPathCount = 1;
		desc.searchPaths = search_paths.data();

		Slang::ComPtr<slang::IBlob> spirv_code;

		bool result = slang_to_slang_module(desc, _shader_path.stem().generic_string(), true, _entry_name, spirv_code.writeRef());

		if (result)
		{
			std::ofstream out_file(spirv_path.generic_string(), std::ios::out | std::ios::binary);
			out_file.write(reinterpret_cast<const char*>(spirv_code->getBufferPointer()), spirv_code->getBufferSize());
			out_file.close();

			std::vector<char> buffer(spirv_code->getBufferSize());
			memcpy(buffer.data(), spirv_code->getBufferPointer(), spirv_code->getBufferSize());
			return buffer;
		}
	}

	return std::vector<char>();
}

std::vector<char> shader_compiler::compile_shader_to_spv(const std::string& _shader_string, const std::vector<std::string>& _entry_name) noexcept
{
	Slang::ComPtr<slang::IBlob> spirv_code;
	bool result = slang_to_slang_module(session_desc, _shader_string, false, _entry_name, spirv_code.writeRef());

	if (result)
	{
		std::vector<char> buffer(spirv_code->getBufferSize());
		memcpy(buffer.data(), spirv_code->getBufferPointer(), spirv_code->getBufferSize());
		return buffer;
	}

	return std::vector<char>();
}

bool shader_compiler::slang_to_slang_module(const slang::SessionDesc& _session_desc, const std::string& _shader_string, bool _as_shader_name, const std::vector<std::string>& _entry_name, slang::IBlob** _spirv_code) noexcept
{
	Slang::ComPtr<slang::ISession> session;
	auto result = global_session->createSession(_session_desc, session.writeRef());
	SLANG_RETURN_FALSE_ON_FAIL(result);

	Slang::ComPtr<slang::IBlob> diagnostics_blob;
	Slang::ComPtr<slang::IModule> slang_module;

	if (_as_shader_name)
	{
		slang_module = session->loadModule(_shader_string.c_str(), diagnostics_blob.writeRef());
	}
	else
	{
		slang_module = session->loadModuleFromSourceString("shaders", "memory:shader", _shader_string.c_str(), diagnostics_blob.writeRef());
	}

	diagnose_if_needed(diagnostics_blob);

	if (!slang_module)
	{
		return false;
	}

	return slang_module_to_spv(session, diagnostics_blob, slang_module, _entry_name, _spirv_code);
}

bool shader_compiler::slang_module_to_spv(Slang::ComPtr<slang::ISession>& _session, Slang::ComPtr<slang::IBlob>& _diagnostics_blob, Slang::ComPtr<slang::IModule>& _slang_module, const std::vector<std::string>& _entry_name, slang::IBlob** _spirv_code) noexcept
{
	std::vector<slang::IComponentType*> component_types;
	for (const auto& entry_name : _entry_name)
	{
		Slang::ComPtr<slang::IEntryPoint> entry_point;
		_slang_module->findEntryPointByName(entry_name.c_str(), entry_point.writeRef());
		if (!entry_point)
		{
			return false;
		}
		component_types.emplace_back(entry_point);
	}

	Slang::ComPtr<slang::IComponentType> composed_program;
	auto result = _session->createCompositeComponentType(component_types.data(), component_types.size(), composed_program.writeRef(), _diagnostics_blob.writeRef());
	diagnose_if_needed(_diagnostics_blob);
	SLANG_RETURN_FALSE_ON_FAIL(result);

	Slang::ComPtr<slang::IComponentType> linked_program;
	result = composed_program->link(linked_program.writeRef(), _diagnostics_blob.writeRef());
	diagnose_if_needed(_diagnostics_blob);
	SLANG_RETURN_FALSE_ON_FAIL(result);

	// result = linkedProgram->getEntryPointCode(0, 0, spirv_code, _diagnostics_blob.writeRef());
	result = linked_program->getTargetCode(0, _spirv_code, _diagnostics_blob.writeRef());
	diagnose_if_needed(_diagnostics_blob);
	SLANG_RETURN_FALSE_ON_FAIL(result);
	print_entrypoint_hashes(static_cast<int>(_entry_name.size()), 1, composed_program);

	return true;
}

shader_compiler& shader_compiler::get_shader_compiler() noexcept
{
	return compiler;
}
