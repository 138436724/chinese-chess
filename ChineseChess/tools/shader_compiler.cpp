module;

#include <slang.h>
#include <slang-com-ptr.h>
#include <slang-com-helper.h>

module shader_compiler;

import file_watcher;

shader_compiler shader_compiler::compiler;

#ifndef NDEBUG
void diagnoseIfNeeded(Slang::ComPtr<slang::IBlob>& diagnosticBlob)
{
	if (diagnosticBlob != nullptr)
	{
		std::println("{}", reinterpret_cast<const char*>(diagnosticBlob->getBufferPointer()));
	}
}

void printEntrypointHashes(int entryPointCount, int targetCount, Slang::ComPtr<slang::IComponentType>& composedProgram)
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
			std::cout << strBuilder.str() << '\n';
		}
	}
}
#else
void diagnoseIfNeeded(Slang::ComPtr<slang::IBlob>& diagnosticBlob) {}
void printEntrypointHashes(int entryPointCount, int targetCount, Slang::ComPtr<slang::IComponentType>& composedProgram) {}
#endif

shader_compiler::shader_compiler()
{
	auto result = slang::createGlobalSession(globalSession.writeRef());
	assert(SLANG_SUCCEEDED(result));
}

shader_compiler::~shader_compiler()
{
}

const std::vector<char> shader_compiler::compile_shader_to_spv(const std::filesystem::path& _shader_path, const std::vector<std::string>& _entry_name)
{
	std::filesystem::path spirv_path = _shader_path;
	spirv_path.replace_extension(".spv");

	if (!file_watcher::get_file_watcher().is_file_modified(_shader_path.generic_string()) && std::filesystem::exists(spirv_path))
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
		bool result = compile_slang_to_spv(_shader_path, _entry_name, spirv_code.writeRef());

		if (result)
		{
			std::ofstream out_file(spirv_path.generic_string(), std::ios::out | std::ios::binary);
			out_file.write(reinterpret_cast<const char*>(spirv_code->getBufferPointer()), spirv_code->getBufferSize());
			out_file.close();

			std::vector<char> buffer;
			buffer.resize(spirv_code->getBufferSize());
			memcpy(buffer.data(), spirv_code->getBufferPointer(), spirv_code->getBufferSize());
			return buffer;
		}
	}

	return std::vector<char>();
}


bool shader_compiler::compile_slang_to_spv(const std::filesystem::path& _shader_path, const std::vector<std::string>& _entry_name, slang::IBlob** _spirv_code)
{
	slang::TargetDesc targetDesc = {};
	targetDesc.format = SLANG_SPIRV;
	targetDesc.profile = globalSession->findProfile("spirv_1_4");
	// targetDesc.flags = 0;

	slang::SessionDesc sessionDesc = {};
	sessionDesc.targets = &targetDesc;
	sessionDesc.targetCount = 1;

	const std::string parentPath = _shader_path.parent_path().generic_string();
	const char* searchPaths[] = { parentPath.c_str() };
	sessionDesc.searchPathCount = 1;
	sessionDesc.searchPaths = searchPaths;

	std::array<slang::CompilerOptionEntry, 5> options =
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
	sessionDesc.compilerOptionEntries = options.data();
	sessionDesc.compilerOptionEntryCount = static_cast<uint32_t>(options.size());

	Slang::ComPtr<slang::ISession> session;
	auto result = globalSession->createSession(sessionDesc, session.writeRef());
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

	std::vector<slang::IComponentType*> componentTypes;

	for (auto& entryPointName : _entry_name)
	{
		Slang::ComPtr<slang::IEntryPoint> entryPoint;
		slangModule->findEntryPointByName(entryPointName.c_str(), entryPoint.writeRef());
		if (!entryPoint)
		{
			return false;
		}
		componentTypes.emplace_back(entryPoint);
	}

	Slang::ComPtr<slang::IComponentType> composedProgram;
	result = session->createCompositeComponentType(componentTypes.data(), componentTypes.size(), composedProgram.writeRef(), diagnosticsBlob.writeRef());
	diagnoseIfNeeded(diagnosticsBlob);
	SLANG_RETURN_FALSE_ON_FAIL(result);

	Slang::ComPtr<slang::IComponentType> linkedProgram;
	result = composedProgram->link(linkedProgram.writeRef(), diagnosticsBlob.writeRef());
	diagnoseIfNeeded(diagnosticsBlob);
	SLANG_RETURN_FALSE_ON_FAIL(result);

	// result = linkedProgram->getEntryPointCode(0, 0, spirv_code, diagnosticsBlob.writeRef());
	result = linkedProgram->getTargetCode(0, _spirv_code, diagnosticsBlob.writeRef());
	diagnoseIfNeeded(diagnosticsBlob);
	SLANG_RETURN_FALSE_ON_FAIL(result);
	printEntrypointHashes(1, 1, composedProgram);

	return true;
}

shader_compiler& shader_compiler::get_shader_compiler() noexcept
{
	return compiler;
}