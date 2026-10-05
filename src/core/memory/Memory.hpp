#ifndef _PPROCESS_HPP_
#define _PPROCESS_HPP_

#include <vector>
#include <math.h>
#include <Windows.h>
#include <TlHelp32.h>
#include <string>
#include <iostream>
#include <Psapi.h> 

typedef NTSTATUS(WINAPI* pNtReadVirtualMemory)(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer, ULONG NumberOfBytesToRead, PULONG NumberOfBytesRead);
typedef NTSTATUS(WINAPI* pNtWriteVirtualMemory)(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer, ULONG NumberOfBytesToWrite, PULONG NumberOfBytesWritten);

class pMemory {

public:
	pMemory() {
		pfnNtReadVirtualMemory = (pNtReadVirtualMemory)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtReadVirtualMemory");
		pfnNtWriteVirtualMemory = (pNtWriteVirtualMemory)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtWriteVirtualMemory");
	}

	pNtReadVirtualMemory pfnNtReadVirtualMemory;
	pNtWriteVirtualMemory pfnNtWriteVirtualMemory;
};

struct ProcessModule
{
	uintptr_t base, size;
};

class pProcess
{
public:
	DWORD		  pid_; // process id
	HANDLE		  handle_; // handle to process
	HWND		  hwnd_; // window handle
	ProcessModule base_client_;

public:
	bool AttachProcess(const char* process_name);
	bool AttachWindow(const char* window_name);
	bool UpdateHWND();
	void Close();

public:
	ProcessModule GetModule(const char* module_name);
	std::wstring  ReadCommandLine();

	// Address of a function the module exports, 0 when not there
	uintptr_t     FindExport(const char* module_name, const char* export_name);

	// Instance of an interface the module registered, one whose create function is "lea rax, [rip + instance]; ret"
	uintptr_t     FindInterface(const char* module_name, const char* interface_name);
	LPVOID		  Allocate(size_t size_in_bytes);
	uintptr_t	  FindCodeCave(uint32_t length_in_bytes);
	uintptr_t     FindSignature(std::vector<uint8_t> signature);
	uintptr_t     FindSignature(ProcessModule target_module, std::vector<uint8_t> signature);

	template<class T>
	uintptr_t ReadOffsetFromSignature(std::vector<uint8_t> signature, uint8_t offset) // offset example: "FF 05 ->22628B01<-" offset is 2
	{
		uintptr_t pattern_address = this->FindSignature(signature);
		if (!pattern_address)
			return 0x0;

		T offset_value = this->read<T>(pattern_address + offset);
		return pattern_address + offset_value + offset + sizeof(T);
	}

	bool read_raw(uintptr_t address, void* buffer, size_t size)
	{
		SIZE_T bytesRead;
		pMemory cMemory;

		NTSTATUS status = cMemory.pfnNtReadVirtualMemory(this->handle_, (PVOID)(address), buffer, static_cast<ULONG>(size), (PULONG)&bytesRead);
	
		return status == 0x00000000/*STATUS_SUCCESS*/ || bytesRead == size;
	}

	template<class T>
	void write(uintptr_t address, T value)
	{
		pMemory cMemory;
		cMemory.pfnNtWriteVirtualMemory(handle_, (void*)address, &value, sizeof(T), 0);
	}

	template<class T>
	T read(uintptr_t address)
	{
		T buffer{};
		pMemory cMemory;

		cMemory.pfnNtReadVirtualMemory(handle_, (void*)address, &buffer, sizeof(T), 0);
		return buffer;
	}

	// Writes over code, its pages are read only so the protection is lifted for the write
	bool patch_code(uintptr_t address, const void* bytes, size_t size)
	{
		DWORD protection = 0;
		if (!VirtualProtectEx(handle_, reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &protection))
			return false;

		SIZE_T written = 0;
		bool success = WriteProcessMemory(handle_, reinterpret_cast<void*>(address), bytes, size, &written) && written == size;

		VirtualProtectEx(handle_, reinterpret_cast<void*>(address), size, protection, &protection);
		FlushInstructionCache(handle_, reinterpret_cast<void*>(address), size);

		return success;
	}

	// Memory inside the game, released with free_remote()
	uintptr_t allocate_remote(size_t size, DWORD protection = PAGE_READWRITE)
	{
		return reinterpret_cast<uintptr_t>(VirtualAllocEx(handle_, nullptr, size, MEM_COMMIT | MEM_RESERVE, protection));
	}

	void free_remote(uintptr_t address)
	{
		if (address)
			VirtualFreeEx(handle_, reinterpret_cast<void*>(address), 0, MEM_RELEASE);
	}

	// Starts a new thread of the game at function, without waiting for it
	bool start_remote(uintptr_t function, uintptr_t argument = 0)
	{
		HANDLE thread = CreateRemoteThread(handle_, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(function), reinterpret_cast<void*>(argument), 0, nullptr);
		if (!thread)
			return false;

		CloseHandle(thread);
		return true;
	}

	// Runs a game function taking up to one argument on a new thread of the game and waits for it
	bool call_remote(uintptr_t function, uintptr_t argument = 0, DWORD timeout_ms = 2000)
	{
		HANDLE thread = CreateRemoteThread(handle_, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(function), reinterpret_cast<void*>(argument), 0, nullptr);
		if (!thread)
			return false;

		bool finished = WaitForSingleObject(thread, timeout_ms) == WAIT_OBJECT_0;
		CloseHandle(thread);

		return finished;
	}

	void write_bytes(uintptr_t addr, std::vector<uint8_t> patch)
	{
		pMemory cMemory;
		cMemory.pfnNtWriteVirtualMemory(handle_, (void*)addr, &patch[0], patch.size(), 0);
	}

	uintptr_t read_multi_address(uintptr_t ptr, std::vector<uintptr_t> offsets)
	{
		uintptr_t buffer = ptr;
		for (int i = 0; i < offsets.size(); i++)
			buffer = this->read<uintptr_t>(buffer + offsets[i]);

		return buffer;
	}

	template <typename T>
	T read_multi(uintptr_t base, std::vector<uintptr_t> offsets) {
		uintptr_t buffer = base;
		for (int i = 0; i < offsets.size() - 1; i++)
		{
			buffer = this->read<uintptr_t>(buffer + offsets[i]);
		}
		return this->read<T>(buffer + offsets.back());
	}

private:
	uint32_t FindProcessIdByProcessName(const char* process_name);
	uint32_t FindProcessIdByWindowName(const char* window_name);
	HWND GetWindowHandleFromProcessId(DWORD ProcessId);
};
#endif