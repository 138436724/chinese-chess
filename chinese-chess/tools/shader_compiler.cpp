#include "file_watcher.h"
#include "shader_compiler.h"
#include <fstream>
#include <print>
#include <slang-com-helper.h>
#include <slang-com-ptr.h>

shader_compiler shader_compiler::compiler;

#ifndef NDEBUG
void shader_compiler::diagnoseIfNeeded(Slang::ComPtr<slang::IBlob>& diagnosticBlob)
{
	if (diagnosticBlob != nullptr)
	{
		std::println("{}", reinterpret_cast<const char*>(diagnosticBlob->getBufferPointer()));
	}
}

void shader_compiler::printEntrypointHashes(int entryPointCount, int targetCount, Slang::ComPtr<slang::IComponentType>& composedProgram)
{
	int m_globalCounter = 0;

	for (int targetIndex = 0; targetIndex < targetCount; targetIndex++)
	{
		for (int entryPointIndex = 0; entryPointIndex < entryPointCount; entryPointIndex++)
		{
			Slang::ComPtr<slang::IBlob> entryPointHashBlob;
			composedProgram->getEntryPointHash(
				entryPointIndex,
				targetIndex,
				entryPointHashBlob.writeRef());

			std::stringstream strBuilder{};
			strBuilder << "callIdx: " << m_globalCounter << ", entrypoint: " << entryPointIndex
				<< ", target: " << targetIndex << ", hash: ";
			m_globalCounter++;

			uint8_t* buffer = (uint8_t*)entryPointHashBlob->getBufferPointer();
			for (size_t i = 0; i < entryPointHashBlob->getBufferSize(); i++)
			{
				strBuilder << std::format("%.2X", buffer[i]);
			}
			std::println("{}", strBuilder.str());
		}
	}
}
#else
void shader_compiler::diagnoseIfNeeded(Slang::ComPtr<slang::IBlob>& diagnosticBlob) {}
void shader_compiler::printEntrypointHashes(int entryPointCount, int targetCount, Slang::ComPtr<slang::IComponentType>& composedProgram) {}
#endif

shader_compiler::shader_compiler()
{
	options =
	{
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
		slang::CompilerOptionEntry{
			slang::CompilerOptionName::DownstreamArgs,
			{slang::CompilerOptionValueKind::String, 0, 0, nullptr, nullptr},
		},
		slang::CompilerOptionEntry{
			slang::CompilerOptionName::MatrixLayoutColumn,
			{slang::CompilerOptionValueKind::Int, true, 0, nullptr, nullptr},
		},
	};

	auto result = slang::createGlobalSession(global_session.writeRef());
	assert(SLANG_SUCCEEDED(result));

	target_desc = slang::TargetDesc();
	target_desc.format = SLANG_SPIRV;
	target_desc.profile = global_session->findProfile("spirv_1_4");
	// targetDesc.flags = 0;

	session_desc = slang::SessionDesc();
	session_desc.targets = &target_desc;
	session_desc.targetCount = 1;
	session_desc.compilerOptionEntries = options.data();
	session_desc.compilerOptionEntryCount = static_cast<uint32_t>(options.size());
}

shader_compiler::~shader_compiler()
{
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
		Slang::ComPtr<slang::IBlob> spirv_code;
		bool result = slang_to_slang_module(_shader_path, _entry_name, spirv_code.writeRef());

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
	bool result = slang_to_slang_module(_shader_string, _entry_name, spirv_code.writeRef());

	if (result)
	{
		std::vector<char> buffer(spirv_code->getBufferSize());
		memcpy(buffer.data(), spirv_code->getBufferPointer(), spirv_code->getBufferSize());
		return buffer;
	}

	return std::vector<char>();
}

bool shader_compiler::slang_to_slang_module(const std::filesystem::path& _shader_path, const std::vector<std::string>& _entry_name, slang::IBlob** _spirv_code) noexcept
{
	const std::string parentPath = _shader_path.parent_path().generic_string();
	const char* searchPaths[] = { parentPath.c_str() };
	session_desc.searchPathCount = 1;
	session_desc.searchPaths = searchPaths;

	Slang::ComPtr<slang::ISession> session;
	auto result = global_session->createSession(session_desc, session.writeRef());
	SLANG_RETURN_FALSE_ON_FAIL(result);

	Slang::ComPtr<slang::IBlob> diagnosticsBlob;

	Slang::ComPtr<slang::IModule> slangModule;
	const std::string shaderName = _shader_path.stem().generic_string();
	slangModule = session->loadModule(shaderName.c_str(), diagnosticsBlob.writeRef());
	diagnoseIfNeeded(diagnosticsBlob);

	if (!slangModule)
	{
		return false;
	}

	return slang_module_to_spv(session, diagnosticsBlob, slangModule, _entry_name, _spirv_code);
}

bool shader_compiler::slang_to_slang_module(const std::string& _shader_string, const std::vector<std::string>& _entry_name, slang::IBlob** _spirv_code) noexcept
{
	//const std::string parentPath = _shader_path.parent_path().generic_string();
	//const char* searchPaths[] = { parentPath.c_str() };
	//session_desc.searchPathCount = 1;
	//session_desc.searchPaths = searchPaths;

	Slang::ComPtr<slang::ISession> session;
	auto result = global_session->createSession(session_desc, session.writeRef());
	SLANG_RETURN_FALSE_ON_FAIL(result);

	Slang::ComPtr<slang::IBlob> diagnosticsBlob;

	Slang::ComPtr<slang::IModule> slangModule;
	//const std::string shaderName = _shader_path.stem().generic_string();
	slangModule = session->loadModuleFromSourceString("shaders", "memory:shader", _shader_string.c_str(), diagnosticsBlob.writeRef());
	diagnoseIfNeeded(diagnosticsBlob);

	if (!slangModule)
	{
		return false;
	}

	return slang_module_to_spv(session, diagnosticsBlob, slangModule, _entry_name, _spirv_code);
}

bool shader_compiler::slang_module_to_spv(Slang::ComPtr<slang::ISession>& _session, Slang::ComPtr<slang::IBlob>& _diagnostics_blob, Slang::ComPtr<slang::IModule>& _slang_module, const std::vector<std::string>& _entry_name, slang::IBlob** _spirv_code) noexcept
{
	std::vector<slang::IComponentType*> componentTypes;
	for (auto& entryPointName : _entry_name)
	{
		Slang::ComPtr<slang::IEntryPoint> entryPoint;
		_slang_module->findEntryPointByName(entryPointName.c_str(), entryPoint.writeRef());
		if (!entryPoint)
		{
			return false;
		}
		componentTypes.emplace_back(entryPoint);
	}

	Slang::ComPtr<slang::IComponentType> composedProgram;
	auto result = _session->createCompositeComponentType(componentTypes.data(), componentTypes.size(), composedProgram.writeRef(), _diagnostics_blob.writeRef());
	diagnoseIfNeeded(_diagnostics_blob);
	SLANG_RETURN_FALSE_ON_FAIL(result);

	Slang::ComPtr<slang::IComponentType> linkedProgram;
	result = composedProgram->link(linkedProgram.writeRef(), _diagnostics_blob.writeRef());
	diagnoseIfNeeded(_diagnostics_blob);
	SLANG_RETURN_FALSE_ON_FAIL(result);

	// result = linkedProgram->getEntryPointCode(0, 0, spirv_code, _diagnostics_blob.writeRef());
	result = linkedProgram->getTargetCode(0, _spirv_code, _diagnostics_blob.writeRef());
	diagnoseIfNeeded(_diagnostics_blob);
	SLANG_RETURN_FALSE_ON_FAIL(result);
	printEntrypointHashes(1, 1, composedProgram);

	return true;
}

shader_compiler& shader_compiler::get_shader_compiler() noexcept
{
	return compiler;
}