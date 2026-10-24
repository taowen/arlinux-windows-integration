/* Read-only first-chance exception observer. No context writes or injection. */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <dbghelp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int read_memory(HANDLE process, uintptr_t address, void *buffer, SIZE_T size)
{
    SIZE_T count = 0;
    return address && ReadProcessMemory(process, (void *)address, buffer, size, &count) && count == size;
}

static void memory_evidence(HANDLE process, uintptr_t address)
{
    MEMORY_BASIC_INFORMATION memory;
    unsigned char bytes[32];
    unsigned i;
    if (VirtualQueryEx(process, (void *)address, &memory, sizeof(memory)))
        printf("  memory base=%p allocation=%p size=%llx state=%lx protect=%lx type=%lx\n",
               memory.BaseAddress, memory.AllocationBase, (unsigned long long)memory.RegionSize,
               memory.State, memory.Protect, memory.Type);
    else printf("  memory_query_error=%lu\n", GetLastError());
    if (read_memory(process, address, bytes, sizeof(bytes)))
    {
        printf("  bytes@%llx=", (unsigned long long)address);
        for (i = 0; i < sizeof(bytes); ++i) printf("%02x", bytes[i]);
        printf("\n");
    }
}

static void print_string(HANDLE process, uintptr_t address)
{
    char text[384];
    unsigned i;
    for (i = 0; i < sizeof(text)-1; ++i)
    {
        if (!read_memory(process, address+i, &text[i], 1)) return;
        if (!text[i]) break;
        if ((unsigned char)text[i] < 32 && text[i] != '\n' && text[i] != '\t') return;
    }
    if (i == sizeof(text)-1) return;
    text[i] = 0;
    if (i > 1 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, i, NULL, 0))
        printf("  string@%llx: %s\n", (unsigned long long)address, text);
    else if (i == 1)
    {
        WCHAR wide[192];
        unsigned j;
        for (j = 0; j < 191; ++j)
        {
            if (!read_memory(process, address+j*2, &wide[j], 2)) return;
            if (!wide[j]) break;
            if (wide[j] < 32 && wide[j] != '\n' && wide[j] != '\t') return;
        }
        if (j < 2 || j == 191) return;
        wide[j] = 0;
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, text, sizeof(text), NULL, NULL))
            printf("  wide_string@%llx: %s\n", (unsigned long long)address, text);
    }
}

static void cpp_exception(HANDLE process, EXCEPTION_RECORD *exception)
{
    uint32_t array, count, catch_type, descriptor;
    uintptr_t object, base, info;
    unsigned i;
    int pointer_exception = 0;
    if (exception->NumberParameters != 4) return;
    object = exception->ExceptionInformation[1];
    info = exception->ExceptionInformation[2];
    base = exception->ExceptionInformation[3];
    printf("  CXX object=%llx info=%llx module=%llx\n", (unsigned long long)object,
           (unsigned long long)info, (unsigned long long)base);
    if (!read_memory(process, info+12, &array, 4) ||
        !read_memory(process, base+array, &count, 4)) return;
    for (i = 0; i < count && i < 8; ++i)
    {
        if (!read_memory(process, base+array+4+i*4, &catch_type, 4) ||
            !read_memory(process, base+catch_type+4, &descriptor, 4)) break;
        print_string(process, base+descriptor+2*sizeof(uintptr_t));
        if (!i)
        {
            char prefix[4];
            if (read_memory(process, base+descriptor+2*sizeof(uintptr_t), prefix, sizeof(prefix)))
                pointer_exception = prefix[0] == '.' && prefix[1] == 'P' && prefix[2] == 'E' && prefix[3] == 'A';
        }
    }
    /* Bounded raw object evidence. Never call what() inside the target. */
    for (i = 0; object && i < 8; ++i)
    {
        uintptr_t pointer;
        if (!read_memory(process, object+i*sizeof(pointer), &pointer, sizeof(pointer))) break;
        printf("  object[%u]=%llx\n", i, (unsigned long long)pointer);
        if (i)
        {
            MEMORY_BASIC_INFORMATION memory;
            unsigned j;
            print_string(process, pointer);
            /* Boxed exception payloads can contain a string descriptor rather
             * than inline text. Retain one bounded level of readable heap
             * words, without assuming a language ABI or invoking target code. */
            if (!VirtualQueryEx(process, (void *)pointer, &memory, sizeof(memory)) ||
                memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE ||
                (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
                !(memory.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY))) continue;
            for (j = 0; j < 4; ++j)
            {
                uintptr_t word;
                if (!read_memory(process, pointer+j*sizeof(word), &word, sizeof(word))) break;
                printf("  object[%u].word[%u]=%llx\n", i, j, (unsigned long long)word);
                print_string(process, word);
            }
        }
    }
    /* MSVC also throws pointers (e.g. CLR's HRException*). The exception
       storage then contains a pointer, not the object's fields. */
    if (pointer_exception && read_memory(process, object, &object, sizeof(object)))
    {
        printf("  pointee=%llx\n", (unsigned long long)object);
        for (i = 0; object && i < 8; ++i)
        {
            uintptr_t word;
            if (!read_memory(process, object+i*sizeof(word), &word, sizeof(word))) break;
            printf("  pointee[%u]=%llx\n", i, (unsigned long long)word);
            if (i) print_string(process, word);
        }
    }
}

static void backtrace(HANDLE process, HANDLE thread)
{
    CONTEXT context = {0};
    STACKFRAME64 frame = {0};
    unsigned i;
    context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &context)) { printf("  context_error=%lu\n", GetLastError()); return; }
#if defined(__aarch64__)
    frame.AddrPC.Offset = context.Pc;
    frame.AddrStack.Offset = context.Sp;
    frame.AddrFrame.Offset = context.Fp;
    DWORD machine = IMAGE_FILE_MACHINE_ARM64;
    printf("  registers pc=%llx sp=%llx lr=%llx fp=%llx\n", context.Pc, context.Sp, context.Lr, context.Fp);
    for (i = 0; i < 29; ++i)
        printf("  x%u=%llx\n", i, context.X[i]);
    memory_evidence(process, context.X0);
    {
        uintptr_t vtable;
        if (read_memory(process, context.X0, &vtable, sizeof(vtable)))
            memory_evidence(process, vtable);
    }
#elif defined(__x86_64__)
    frame.AddrPC.Offset = context.Rip;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrFrame.Offset = context.Rbp;
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
#error Build a 64-bit observer
#endif
    frame.AddrPC.Mode = frame.AddrStack.Mode = frame.AddrFrame.Mode = AddrModeFlat;
    /* Keep bounded raw evidence when Wine's architecture unwinder stops early.
     * Stack words are not asserted to be valid call frames. */
    for (i = 0; i < 128; ++i)
    {
        uintptr_t word;
        IMAGEHLP_MODULE64 module = {0};
        module.SizeOfStruct = sizeof(module);
        if (!read_memory(process, frame.AddrStack.Offset+i*sizeof(word), &word, sizeof(word))) break;
        if (SymGetModuleInfo64(process, word, &module))
            printf("  stack_word=%llx value=%llx %s+%llx\n",
                   (unsigned long long)(frame.AddrStack.Offset+i*sizeof(word)), (unsigned long long)word,
                   module.ModuleName, (unsigned long long)(word-module.BaseOfImage));
    }
    for (i = 0; i < 32 && frame.AddrPC.Offset; ++i)
    {
        IMAGEHLP_MODULE64 module = {0};
        module.SizeOfStruct = sizeof(module);
        if (SymGetModuleInfo64(process, frame.AddrPC.Offset, &module))
            printf("  frame=%u %s+%llx pc=%llx\n", i, module.ModuleName,
                   (unsigned long long)(frame.AddrPC.Offset-module.BaseOfImage),
                   (unsigned long long)frame.AddrPC.Offset);
        else printf("  frame=%u pc=%llx\n", i, (unsigned long long)frame.AddrPC.Offset);
        if (!StackWalk64(machine, process, thread, &frame, &context, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
    }
}

int wmain(int argc, WCHAR **argv)
{
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process;
    DEBUG_EVENT event;
    DWORD started = GetTickCount(), duration;
    int status = 0;
    BOOL loader_breakpoint_seen = FALSE;
    WCHAR report_path[32768];
    DWORD report_length = GetEnvironmentVariableW(L"ARLINUX_EXCEPTION_LOG", report_path, 32768);
    /* Proton's steam.exe launcher does not preserve console stdout. Keep the
     * observer evidence independent of the launcher's inherited handles. */
    if (report_length && (report_length >= 32768 || !_wfreopen(report_path, L"w", stdout))) return 5;
    startup.cb = sizeof(startup);
    if (argc != 3 && argc != 4) { fprintf(stderr, "Usage: exception-observer.exe SECONDS EXECUTABLE [COMMAND_LINE]\n"); return 2; }
    duration = _wtoi(argv[1])*1000;
    if (!duration || duration > 600000) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!CreateProcessW(argv[2], argc == 4 ? argv[3] : NULL, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS,
                        NULL, NULL, &startup, &process))
    { printf("create_error=%lu\n", GetLastError()); return 3; }
    if (!DebugSetProcessKillOnExit(FALSE))
    {
        printf("kill_on_exit_error=%lu\n", GetLastError());
        DebugActiveProcessStop(process.dwProcessId);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 4;
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    if (!SymInitialize(process.hProcess, NULL, TRUE))
        printf("symbols_error=%lu\n", GetLastError());
    printf("process=%lu main_thread=%lu duration_ms=%lu\n", process.dwProcessId, process.dwThreadId, duration);
    while (GetTickCount()-started < duration)
    {
        DWORD disposition = DBG_CONTINUE;
        LARGE_INTEGER output_size;
        if (GetFileSizeEx(GetStdHandle(STD_OUTPUT_HANDLE), &output_size) && output_size.QuadPart > 16*1024*1024)
        { printf("capture_limit=16MiB\n"); break; }
        if (!WaitForDebugEvent(&event, 250))
        {
            if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
            printf("wait_error=%lu\n", GetLastError()); status = 4; break;
        }
        if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT)
        {
            EXCEPTION_RECORD *exception = &event.u.Exception.ExceptionRecord;
            /* The loader breakpoint belongs to the debugger. Other exceptions
             * must reach the application's own exception handlers unchanged. */
            if (exception->ExceptionCode == EXCEPTION_BREAKPOINT && !loader_breakpoint_seen)
                loader_breakpoint_seen = TRUE;
            else
            {
                disposition = DBG_EXCEPTION_NOT_HANDLED;
                if (exception->ExceptionCode == 0xe06d7363 || !event.u.Exception.dwFirstChance ||
                    exception->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
                {
                    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, event.dwThreadId);
                    printf("exception=%08lx first_chance=%lu tid=%lu address=%p\n",
                           exception->ExceptionCode, event.u.Exception.dwFirstChance,
                           event.dwThreadId, exception->ExceptionAddress);
                    if (exception->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                        exception->NumberParameters >= 2)
                    {
                        printf("  access=%llu target=%llx\n",
                               (unsigned long long)exception->ExceptionInformation[0],
                               (unsigned long long)exception->ExceptionInformation[1]);
                        memory_evidence(process.hProcess, exception->ExceptionInformation[1]);
                    }
                    if (exception->ExceptionCode == 0xe06d7363) cpp_exception(process.hProcess, exception);
                    if (thread) { backtrace(process.hProcess, thread); CloseHandle(thread); }
                }
            }
        }
        else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT)
        {
            char path[1024];
            DWORD length = event.u.LoadDll.hFile ? GetFinalPathNameByHandleA(event.u.LoadDll.hFile, path, sizeof(path), 0) : 0;
            if (length && length < sizeof(path))
            {
                printf("module=%p path=%s\n", event.u.LoadDll.lpBaseOfDll, path);
                SymLoadModuleEx(process.hProcess, event.u.LoadDll.hFile, path, NULL,
                                (DWORD64)(uintptr_t)event.u.LoadDll.lpBaseOfDll, 0, NULL, 0);
            }
            if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
        }
        else if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT && event.u.CreateProcessInfo.hFile)
        {
            SymLoadModuleEx(process.hProcess, event.u.CreateProcessInfo.hFile, NULL, NULL,
                            (DWORD64)(uintptr_t)event.u.CreateProcessInfo.lpBaseOfImage, 0, NULL, 0);
            CloseHandle(event.u.CreateProcessInfo.hFile);
        }
        else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
        {
            printf("process_exit=%lu\n", event.u.ExitProcess.dwExitCode);
            if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition))
            { printf("continue_error=%lu\n", GetLastError()); status = 4; }
            break;
        }
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition))
        { printf("continue_error=%lu\n", GetLastError()); status = 4; break; }
    }
    SymCleanup(process.hProcess);
    if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
    {
        BOOL detached = DebugActiveProcessStop(process.dwProcessId);
        printf("detach=%d\n", detached);
        if (!detached) { printf("detach_error=%lu\n", GetLastError()); status = 4; }
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return status;
}
