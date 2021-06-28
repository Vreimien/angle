//
// Copyright 2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// global_state.cpp : Implements functions for querying the thread-local GL and EGL state.

#include "libGLESv2/global_state.h"

#include "common/debug.h"
#include "common/platform.h"
#include "common/system_utils.h"
#include "libANGLE/ErrorStrings.h"
#include "libANGLE/Thread.h"
#include "libGLESv2/resource.h"

#include <atomic>

namespace egl
{
namespace
{
ANGLE_REQUIRE_CONSTANT_INIT std::atomic<angle::GlobalMutex *> g_Mutex(nullptr);
static_assert(std::is_trivially_destructible<decltype(g_Mutex)>::value,
              "global mutex is not trivially destructible");

ANGLE_REQUIRE_CONSTANT_INIT gl::Context *g_LastContext(nullptr);
static_assert(std::is_trivially_destructible<decltype(g_LastContext)>::value,
              "global last context is not trivially destructible");

void SetContextToAndroidOpenGLTLSSlot(gl::Context *value)
{
#if defined(ANGLE_PLATFORM_ANDROID)
    if (angle::gUseAndroidOpenGLTlsSlot)
    {
        ANGLE_ANDROID_GET_GL_TLS()[angle::kAndroidOpenGLTlsSlot] = static_cast<void *>(value);
    }
#endif
}

Thread *AllocateCurrentThread()
{
    {
        // Global thread intentionally leaked
        ANGLE_SCOPED_DISABLE_LSAN();
        gCurrentThread = new Thread();
    }

    // Initialize fast TLS slot
    SetContextToAndroidOpenGLTLSSlot(nullptr);
    gl::gCurrentValidContext = nullptr;

    return gCurrentThread;
}

void AllocateMutex()
{
    if (g_Mutex == nullptr)
    {
        std::unique_ptr<angle::GlobalMutex> newMutex(new angle::GlobalMutex());
        angle::GlobalMutex *expected = nullptr;
        if (g_Mutex.compare_exchange_strong(expected, newMutex.get()))
        {
            newMutex.release();
        }
    }
}

}  // anonymous namespace

thread_local Thread *gCurrentThread = nullptr;

angle::GlobalMutex &GetGlobalMutex()
{
    AllocateMutex();
    return *g_Mutex;
}

gl::Context *GetGlobalLastContext()
{
    return g_LastContext;
}

void SetGlobalLastContext(gl::Context *context)
{
    g_LastContext = context;
}

// This function causes an MSAN false positive, which is muted. See https://crbug.com/1211047
// It also causes a flaky false positive in TSAN. http://crbug.com/1223970
ANGLE_NO_SANITIZE_MEMORY ANGLE_NO_SANITIZE_THREAD Thread *GetCurrentThread()
{
    Thread *current = gCurrentThread;
    return (current ? current : AllocateCurrentThread());
}

void SetContextCurrent(Thread *thread, gl::Context *context)
{
    ASSERT(gCurrentThread);
    gCurrentThread->setCurrent(context);
    SetContextToAndroidOpenGLTLSSlot(context);
    gl::gCurrentValidContext = context;
#if defined(ANGLE_FORCE_CONTEXT_CHECK_EVERY_CALL)
    DirtyContextIfNeeded(context);
#endif
}

}  // namespace egl

namespace gl
{
void GenerateContextLostErrorOnContext(Context *context)
{
    if (context && context->isContextLost())
    {
        context->validationError(GL_CONTEXT_LOST, err::kContextLost);
    }
}

void GenerateContextLostErrorOnCurrentGlobalContext()
{
    GenerateContextLostErrorOnContext(GetGlobalContext());
}
}  // namespace gl

#if defined(ANGLE_PLATFORM_WINDOWS) && !defined(ANGLE_STATIC)
namespace egl
{

namespace
{
void DeallocateCurrentThread()
{
    SafeDelete(gCurrentThread);
}

void DeallocateMutex()
{
    angle::GlobalMutex *mutex = g_Mutex.exchange(nullptr);
    {
        // Wait for the mutex to become released by other threads before deleting.
        std::lock_guard<angle::GlobalMutex> lock(*mutex);
    }
    SafeDelete(mutex);
}

bool InitializeProcess()
{
    EnsureDebugAllocated();
    AllocateMutex();
    return AllocateCurrentThread() != nullptr;
}

void TerminateProcess()
{
    DeallocateDebug();
    DeallocateMutex();
    DeallocateCurrentThread();
}

}  // anonymous namespace

}  // namespace egl

namespace
{
// The following WaitForDebugger code is based on SwiftShader. See:
// https://cs.chromium.org/chromium/src/third_party/swiftshader/src/Vulkan/main.cpp
#    if defined(ANGLE_ENABLE_ASSERTS) && !defined(ANGLE_ENABLE_WINDOWS_UWP)
INT_PTR CALLBACK DebuggerWaitDialogProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    RECT rect;

    switch (uMsg)
    {
        case WM_INITDIALOG:
            ::GetWindowRect(GetDesktopWindow(), &rect);
            ::SetWindowPos(hwnd, HWND_TOP, rect.right / 2, rect.bottom / 2, 0, 0, SWP_NOSIZE);
            ::SetTimer(hwnd, 1, 100, NULL);
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL)
            {
                ::EndDialog(hwnd, 0);
            }
            break;
        case WM_TIMER:
            if (angle::IsDebuggerAttached())
            {
                ::EndDialog(hwnd, 0);
            }
    }

    return FALSE;
}

void WaitForDebugger(HINSTANCE instance)
{
    if (angle::IsDebuggerAttached())
        return;

    HRSRC dialog = ::FindResourceA(instance, MAKEINTRESOURCEA(IDD_DIALOG1), MAKEINTRESOURCEA(5));
    if (!dialog)
    {
        printf("Error finding wait for debugger dialog. Error %lu.\n", ::GetLastError());
        return;
    }

    DLGTEMPLATE *dialogTemplate = reinterpret_cast<DLGTEMPLATE *>(::LoadResource(instance, dialog));
    ::DialogBoxIndirectA(instance, dialogTemplate, NULL, DebuggerWaitDialogProc);
}
#    else
void WaitForDebugger(HINSTANCE instance) {}
#    endif  // defined(ANGLE_ENABLE_ASSERTS) && !defined(ANGLE_ENABLE_WINDOWS_UWP)
}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            if (angle::GetEnvironmentVar("ANGLE_WAIT_FOR_DEBUGGER") == "1")
            {
                WaitForDebugger(instance);
            }
            return static_cast<BOOL>(egl::InitializeProcess());

        case DLL_THREAD_ATTACH:
            return static_cast<BOOL>(egl::AllocateCurrentThread() != nullptr);

        case DLL_THREAD_DETACH:
            egl::DeallocateCurrentThread();
            break;

        case DLL_PROCESS_DETACH:
            egl::TerminateProcess();
            break;
    }

    return TRUE;
}
#endif  // defined(ANGLE_PLATFORM_WINDOWS) && !defined(ANGLE_STATIC)
