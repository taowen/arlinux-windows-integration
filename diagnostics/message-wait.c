/* Bounded hidden-window/audio queue diagnostic. Does not inject into applications. */
#define COBJMACROS
#include <windows.h>
#include <dsound.h>
#include <stdio.h>
#include <string.h>

struct worker
{
    HANDLE ready;
    HWND window;
    BOOL audio;
    HRESULT result;
};

static BOOL inside_create;

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_CREATE && inside_create)
    {
        HANDLE ready = CreateEventW(NULL, TRUE, TRUE, NULL);
        DWORD start = GetTickCount(), result;
        do
            result = MsgWaitForMultipleObjects(1, &ready, TRUE, 1, QS_ALLINPUT);
        while (result == WAIT_TIMEOUT && GetTickCount() - start < 2000);
        printf("inside_create result=%lu elapsed=%lu queue=%08lx\n", result,
               GetTickCount() - start, GetQueueStatus(QS_ALLINPUT));
        CloseHandle(ready);
    }
    return DefWindowProcW(window, message, wp, lp);
}

static DWORD WINAPI initialize(void *arg)
{
    struct worker *worker = arg;
    IDirectSound *sound = NULL;
    IDirectSoundBuffer *buffer = NULL;
    WAVEFORMATEX format = {WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0};
    DSBUFFERDESC desc = {0};

    if (worker->audio)
    {
        worker->result = DirectSoundCreate(NULL, &sound, NULL);
        if (SUCCEEDED(worker->result))
        {
            worker->result = IDirectSound_SetCooperativeLevel(sound, worker->window, DSSCL_NORMAL);
            desc.dwSize = sizeof(desc);
            desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_GLOBALFOCUS;
            desc.dwBufferBytes = 176400;
            desc.lpwfxFormat = &format;
            if (SUCCEEDED(worker->result))
                worker->result = IDirectSound_CreateSoundBuffer(sound, &desc, &buffer, NULL);
        }
    }
    Sleep(20);
    SetEvent(worker->ready);
    if (buffer) IDirectSoundBuffer_Release(buffer);
    if (sound) IDirectSound_Release(sound);
    return 0;
}

int main(int argc, char **argv)
{
    WNDCLASSW cls = {0};
    struct worker worker = {0};
    HANDLE thread;
    MSG message;
    DWORD start, result = WAIT_TIMEOUT, iterations = 0, status;
    BOOL drain = argc > 2 && !strcmp(argv[2], "drain");
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    HWND parent = NULL;

    if (argc > 3 && !strcmp(argv[3], "popup")) style = WS_POPUP;
    if (argc > 3 && !strcmp(argv[3], "popup-caption")) style = WS_POPUP | WS_CAPTION;
    if (argc > 3 && !strcmp(argv[3], "popup-border")) style = WS_POPUP | WS_BORDER;
    if (argc > 3 && !strcmp(argv[3], "plain")) style = 0;
    if (argc > 3 && !strcmp(argv[3], "message")) parent = HWND_MESSAGE;
    inside_create = argc > 4 && !strcmp(argv[4], "inside-create");

    setvbuf(stdout, NULL, _IONBF, 0);
    worker.audio = argc > 1 && !strcmp(argv[1], "audio");
    cls.lpfnWndProc = window_proc;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = L"ARLinuxMessageWaitDiagnostic";
    if (!RegisterClassW(&cls)) return 2;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    worker.window = CreateWindowExW(WS_EX_WINDOWEDGE, cls.lpszClassName, L"Hidden queue diagnostic",
                                  style, 100, 100, 1280, 720, parent, NULL, cls.hInstance, NULL);
    worker.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!worker.window || !worker.ready) return 2;
    if (drain)
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    thread = CreateThread(NULL, 0, initialize, &worker, 0, NULL);
    if (!thread) return 2;
    SetThreadPriority(thread, THREAD_PRIORITY_LOWEST);
    start = GetTickCount();
    do
    {
        result = MsgWaitForMultipleObjects(1, &worker.ready, TRUE, 1, QS_ALLINPUT);
        ++iterations;
    } while (result == WAIT_TIMEOUT && GetTickCount() - start < 2000);
    status = GetQueueStatus(QS_ALLINPUT);
    printf("audio=%u drain=%u result=%lu elapsed=%lu iterations=%lu queue=%08lx ready=%lu\n",
           worker.audio, drain, result, GetTickCount()-start, iterations, status,
           WaitForSingleObject(worker.ready, 0));
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
    {
        printf("message hwnd=%p id=%04x wp=%llx lp=%llx\n", (void *)message.hwnd, message.message,
               (unsigned long long)message.wParam, (unsigned long long)message.lParam);
        DispatchMessageW(&message);
    }
    if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) return 3;
    printf("audio_result=%08lx\n", (unsigned long)worker.result);
    CloseHandle(thread);
    CloseHandle(worker.ready);
    DestroyWindow(worker.window);
    UnregisterClassW(cls.lpszClassName, cls.hInstance);
    return 0; /* A completed observation is not application acceptance. */
}
