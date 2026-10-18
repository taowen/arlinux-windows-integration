/* Synthetic MSVC exception metadata; validates observation, not Adobe itself. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
static int handled;
static struct { uintptr_t vtable, spare; char name[64]; } descriptor = {0, 0, ".?AVobserver_probe@@"};
static uint32_t catch_type[7], catch_array[2], throw_info[4];
static uintptr_t object[8];
static uintptr_t pointee[8];
static const char message[] = "observer probe: original UTF-8 message";
static const WCHAR wide_message[] = L"observer probe: original UTF-16 message";
static LONG CALLBACK handler(EXCEPTION_POINTERS *p)
{
    if (p->ExceptionRecord->ExceptionCode != 0xe06d7363 ||
        p->ExceptionRecord->NumberParameters != 4 ||
        p->ExceptionRecord->ExceptionInformation[1] != (uintptr_t)object)
        return EXCEPTION_CONTINUE_SEARCH;
    handled++;
    return EXCEPTION_CONTINUE_EXECUTION;
}
int main(void)
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    ULONG_PTR parameters[] = {0x19930520, (uintptr_t)object, (uintptr_t)throw_info, base};
    void *before = NtCurrentTeb();
    FILE *result;
    catch_type[1] = (uintptr_t)&descriptor-base;
    catch_array[0] = 1;
    catch_array[1] = (uintptr_t)catch_type-base;
    throw_info[3] = (uintptr_t)catch_array-base;
    object[1] = (uintptr_t)message;
    object[3] = (uintptr_t)wide_message;
    AddVectoredExceptionHandler(1, handler);
    RaiseException(0xe06d7363, 0, 4, parameters);
    RaiseException(0xe06d7363, 0, 4, parameters);
    strcpy(descriptor.name, ".PEAVobserver_probe@@");
    pointee[1] = 0x80070057;
    pointee[2] = (uintptr_t)message;
    object[0] = (uintptr_t)pointee;
    RaiseException(0xe06d7363, 0, 4, parameters);
    result = fopen("C:\\observer-probe-result.txt", "w");
    if (!result) return 2;
    fprintf(result, "handled=%d teb_unchanged=%d status=%d\n", handled, NtCurrentTeb()==before,
            handled==3 && NtCurrentTeb()==before ? 0 : 1);
    fclose(result);
    return handled==3 && NtCurrentTeb()==before ? 0 : 1;
}
