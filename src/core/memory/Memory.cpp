#include "Memory.hpp"
#include <tlhelp32.h>

uint32_t pProcess::FindProcessIdByProcessName(const char* ProcessName)
{
	std::wstring wideProcessName;
	int wideCharLength = MultiByteToWideChar(CP_UTF8, 0, ProcessName, -1, nullptr, 0);
	if (wideCharLength > 0)
	{
		wideProcessName.resize(wideCharLength);
		MultiByteToWideChar(CP_UTF8, 0, ProcessName, -1, &wideProcessName[0], wideCharLength);
	}

	HANDLE hPID = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
	PROCESSENTRY32W process_entry_{ };
	process_entry_.dwSize = sizeof(PROCESSENTRY32W);

	DWORD pid = 0;
	if (Process32FirstW(hPID, &process_entry_))
	{
		do
		{
			if (!wcscmp(process_entry_.szExeFile, wideProcessName.c_str()))
			{
				pid = process_entry_.th32ProcessID;
				break;
			}
		} while (Process32NextW(hPID, &process_entry_));
	}
	CloseHandle(hPID);
	return pid;
}

uint32_t pProcess::FindProcessIdByWindowName(const char* WindowName)
{
	DWORD process_id = 0;
	HWND windowHandle = FindWindowA(nullptr, WindowName);
	if (windowHandle)
		GetWindowThreadProcessId(windowHandle, &process_id);
	return process_id;
}

HWND pProcess::GetWindowHandleFromProcessId(DWORD ProcessId) {
	HWND hwnd = NULL;
	do {
		hwnd = FindWindowEx(NULL, hwnd, NULL, NULL);
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid == ProcessId) {
			TCHAR windowTitle[MAX_PATH];
			GetWindowText(hwnd, windowTitle, MAX_PATH);
			if (IsWindowVisible(hwnd) && windowTitle[0] != '\0') {
				return hwnd;
			}
		}
	} while (hwnd != NULL);
	return NULL; // No main window found for the given process ID
}

  bool pProcess::AttachProcess(const char* ProcessName)
{
	this->pid_ = this->FindProcessIdByProcessName(ProcessName);

	if (pid_)
	{
		HMODULE modules[0xFF];
		MODULEINFO module_info;
		DWORD _;

		handle_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | PROCESS_DUP_HANDLE |
                                PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid_);

		if (!handle_)
			return false;

		EnumProcessModulesEx(this->handle_, modules, sizeof(modules), &_, LIST_MODULES_64BIT);
		base_client_.base = (uintptr_t)modules[0];

		GetModuleInformation(this->handle_, modules[0], &module_info, sizeof(module_info));
		base_client_.size = module_info.SizeOfImage;

		hwnd_ = this->GetWindowHandleFromProcessId(pid_);

		return true;
	}

	return false;
}

bool pProcess::AttachWindow(const char* WindowName)
{
	this->pid_ = this->FindProcessIdByWindowName(WindowName);

	if (pid_)
	{
		HMODULE modules[0xFF];
		MODULEINFO module_info;
		DWORD _;

		handle_ = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid_);

		EnumProcessModulesEx(this->handle_, modules, sizeof(modules), &_, LIST_MODULES_64BIT);
		base_client_.base = (uintptr_t)modules[0];

		GetModuleInformation(this->handle_, modules[0], &module_info, sizeof(module_info));
		base_client_.size = module_info.SizeOfImage;

		hwnd_ = this->GetWindowHandleFromProcessId(pid_);

		return true;
	}
	return false;
}

bool pProcess::UpdateHWND()
{
	hwnd_ = this->GetWindowHandleFromProcessId(pid_);
	return hwnd_ != nullptr;
}

ProcessModule pProcess::GetModule(const char* lModule)
{
	std::wstring wideModule;
	int wideCharLength = MultiByteToWideChar(CP_UTF8, 0, lModule, -1, nullptr, 0);
	if (wideCharLength > 0)
	{
		wideModule.resize(wideCharLength);
		MultiByteToWideChar(CP_UTF8, 0, lModule, -1, &wideModule[0], wideCharLength);
	}

	HANDLE handle_module = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid_);
	MODULEENTRY32W module_entry_{};
	module_entry_.dwSize = sizeof(MODULEENTRY32W);

	do
	{
		if (!wcscmp(module_entry_.szModule, wideModule.c_str()))
		{
			CloseHandle(handle_module);
			return { (DWORD_PTR)module_entry_.modBaseAddr, module_entry_.modBaseSize }; // dwSize is the size of the entry
		}
	} while (Module32NextW(handle_module, &module_entry_));

	CloseHandle(handle_module);
	return { 0, 0 };
}

uintptr_t pProcess::FindExport(const char* module_name, const char* export_name)
{
	auto module = GetModule(module_name);
	if (!module.base)
		return 0;

	auto pe = read<uint32_t>(module.base + 0x3C);
	auto exports = read<uint32_t>(module.base + pe + 0x88);
	if (!pe || !exports)
		return 0;

	auto name_count = read<uint32_t>(module.base + exports + 0x18);
	auto functions = read<uint32_t>(module.base + exports + 0x1C);
	auto names = read<uint32_t>(module.base + exports + 0x20);
	auto ordinals = read<uint32_t>(module.base + exports + 0x24);

	for (uint32_t i = 0; i < name_count; i++) {
		char name[256]{};	// C++ names are long: ?LoadKV3@@... is 70
		read_raw(module.base + read<uint32_t>(module.base + names + 4 * i), name, sizeof(name) - 1);

		if (std::string_view(name) == export_name) {
			auto ordinal = read<uint16_t>(module.base + ordinals + 2 * i);
			return module.base + read<uint32_t>(module.base + functions + 4 * ordinal);
		}
	}

	return 0;
}

uintptr_t pProcess::FindInterface(const char* module_name, const char* interface_name)
{
	auto create_interface = FindExport(module_name, "CreateInterface");
	if (!create_interface)
		return 0;

	// It walks the registered interfaces: "mov r9/rax, [rip + list]" near the start
	uint8_t code[64]{};
	read_raw(create_interface, code, sizeof(code));

	uintptr_t registration = 0;
	for (size_t i = 0; i + 7 <= sizeof(code); i++) {
		if ((code[i] == 0x48 || code[i] == 0x4C) && code[i + 1] == 0x8B && (code[i + 2] & 0xC7) == 0x05) {
			registration = read<uintptr_t>(create_interface + i + 7 + *reinterpret_cast<int32_t*>(&code[i + 3]));
			break;
		}
	}

	// { create function, name, next }
	for (int guard = 0; registration && guard < 256; guard++) {
		auto create = read<uintptr_t>(registration);
		char name[64]{};
		read_raw(read<uintptr_t>(registration + 8), name, sizeof(name) - 1);

		if (std::string_view(name) == interface_name) {
			uint8_t lea[8]{};
			read_raw(create, lea, sizeof(lea));
			if (lea[0] == 0x48 && lea[1] == 0x8D && lea[2] == 0x05 && lea[7] == 0xC3)
				return create + 7 + *reinterpret_cast<int32_t*>(&lea[3]);
			return 0;
		}

		registration = read<uintptr_t>(registration + 16);
	}

	return 0;
}

LPVOID pProcess::Allocate(size_t size_in_bytes)
{
	return VirtualAllocEx(this->handle_, NULL, size_in_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

uintptr_t pProcess::FindSignature(std::vector<uint8_t> signature)
{
	std::unique_ptr<uint8_t[]> data;
	data = std::make_unique<uint8_t[]>(this->base_client_.size);

	if (!ReadProcessMemory(this->handle_, (void*)(this->base_client_.base), data.get(), this->base_client_.size, NULL)) {
		return 0x0;
	}

	for (uintptr_t i = 0; i < this->base_client_.size; i++)
	{
		for (uintptr_t j = 0; j < signature.size(); j++)
		{
			if (signature.at(j) == 0x00)
				continue;

			if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(&data[i + j])) == signature.at(j))
			{
				if (j == signature.size() - 1)
					return this->base_client_.base + i;
				continue;
			}
			break;
		}
	}
	return 0x0;
}

uintptr_t pProcess::FindSignature(ProcessModule target_module, std::vector<uint8_t> signature)
{
	std::unique_ptr<uint8_t[]> data;
	data = std::make_unique<uint8_t[]>(0xFFFFFFF);

	if (!ReadProcessMemory(this->handle_, (void*)(target_module.base), data.get(), 0xFFFFFFF, NULL)) {
		return NULL;
	}

	for (uintptr_t i = 0; i < 0xFFFFFFF; i++)
	{
		for (uintptr_t j = 0; j < signature.size(); j++)
		{
			if (signature.at(j) == 0x00)
				continue;

			if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(&data[i + j])) == signature.at(j))
			{
				if (j == signature.size() - 1)
					return this->base_client_.base + i;
				continue;
			}
			break;
		}
	}
	return 0x0;
}

uintptr_t pProcess::FindCodeCave(uint32_t length_in_bytes)
{
	std::vector<uint8_t> cave_pattern = {};

	for (uint32_t i = 0; i < length_in_bytes; i++) {
		cave_pattern.push_back(0x00);
	}

	return FindSignature(cave_pattern);
}

std::wstring pProcess::ReadCommandLine()
{
	using pNtQueryInformationProcess = NTSTATUS(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
	constexpr ULONG ProcessCommandLineInformation = 60;

	struct UnicodeString {
		USHORT Length;
		USHORT MaximumLength;
		PWSTR  Buffer;
	};

	auto query = (pNtQueryInformationProcess)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
	if (!query)
		return {};

	ULONG length = 0;
	query(handle_, ProcessCommandLineInformation, nullptr, 0, &length);
	if (length < sizeof(UnicodeString))
		return {};

	std::vector<uint8_t> buffer(length);
	if (query(handle_, ProcessCommandLineInformation, buffer.data(), length, &length) != 0)
		return {};

	// Buffer points inside our own buffer, right after the header
	auto str = reinterpret_cast<UnicodeString*>(buffer.data());
	return std::wstring(str->Buffer, str->Length / sizeof(wchar_t));
}

void pProcess::Close()
{
	CloseHandle(handle_);
}
