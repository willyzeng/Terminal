/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "srvinit.h"

#include "dbcs.h"
#include "handle.h"
#include "registry.hpp"
#include "renderFontDefaults.hpp"

#include "ApiRoutines.h"

#include "..\server\Entrypoints.h"
#include "..\server\IoSorter.h"

#include "..\interactivity\inc\ServiceLocator.hpp"

#pragma hdrstop

const UINT CONSOLE_EVENT_FAILURE_ID = 21790;
const UINT CONSOLE_LPC_PORT_FAILURE_ID = 21791;

void UseVtPipe(const wchar_t* const pwchInVtPipeName, const wchar_t* const pwchOutVtPipeName)
{
    DWORD le;
    auto g = ServiceLocator::LocateGlobals();

    if (pwchInVtPipeName != nullptr)
    {
        // g->hVtPipe.reset(
        g->hVtInPipe = (
            CreateFileW(pwchInVtPipeName,
                        GENERIC_READ, 
                        0, 
                        nullptr, 
                        OPEN_EXISTING, 
                        FILE_ATTRIBUTE_NORMAL, 
                        nullptr)
        );
        le = GetLastError();
        THROW_IF_HANDLE_INVALID(g->hVtInPipe);
    }

    DWORD outputFlags = FILE_ATTRIBUTE_NORMAL;
    // DWORD outputFlags = FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL;

    if (pwchOutVtPipeName != nullptr)
    {
        // g->hVtPipe.reset(
        g->hVtOutPipe = (
            CreateFileW(pwchOutVtPipeName,
                        GENERIC_WRITE, 
                        0, 
                        nullptr, 
                        OPEN_EXISTING, 
                        outputFlags, 
                        nullptr)
        );
        le = GetLastError();
        THROW_IF_HANDLE_INVALID(g->hVtOutPipe);
    }
    
    le;
}

HRESULT ConsoleServerInitialization(_In_ HANDLE Server)
{
    try
    {
        ServiceLocator::LocateGlobals()->pDeviceComm = new DeviceComm(Server);
    }
    CATCH_RETURN();

    ServiceLocator::LocateGlobals()->uiOEMCP = GetOEMCP();
    ServiceLocator::LocateGlobals()->uiWindowsCP = GetACP();

    ServiceLocator::LocateGlobals()->pFontDefaultList = new RenderFontDefaults();
    RETURN_IF_NULL_ALLOC(ServiceLocator::LocateGlobals()->pFontDefaultList);

    FontInfo::s_SetFontDefaultList(ServiceLocator::LocateGlobals()->pFontDefaultList);

    // Removed allocation of scroll buffer here.
    return S_OK;
}

NTSTATUS SetUpConsole(_Inout_ Settings* pStartupSettings,
                      _In_ DWORD TitleLength,
                      _In_reads_bytes_(TitleLength) LPWSTR Title,
                      _In_ LPCWSTR CurDir,
                      _In_ LPCWSTR AppName)
{
    // We will find and locate all relevant preference settings and then create the console here.
    // The precedence order for settings is:
    // 1. STARTUPINFO settings
    // 2a. Shortcut/Link settings
    // 2b. Registry specific settings
    // 3. Registry default settings
    // 4. Hardcoded default settings
    // To establish this hierarchy, we will need to load the settings and apply them in reverse order.

    // 4. Initializing Settings will establish hardcoded defaults.
    // Set to reference of global console information since that's the only place we need to hold the settings.
    CONSOLE_INFORMATION* const settings = ServiceLocator::LocateGlobals()->getConsoleInformation();

    // 3. Read the default registry values.
    Registry reg(settings);
    reg.LoadGlobalsFromRegistry();
    reg.LoadDefaultFromRegistry();

    // 2. Read specific settings

    // Link is expecting the flags from the process to be in already, so apply that first
    settings->SetStartupFlags(pStartupSettings->GetStartupFlags());

    // We need to see if we were spawned from a link. If we were, we need to
    // call back into the shell to try to get all the console information from the link.
    ServiceLocator::LocateSystemConfigurationProvider()->GetSettingsFromLink(settings, Title, &TitleLength, CurDir, AppName);

    // If we weren't started from a link, this will already be set.
    // If LoadLinkInfo couldn't find anything, it will remove the flag so we can dig in the registry.
    if (!(settings->IsStartupTitleIsLinkNameSet()))
    {
        reg.LoadFromRegistry(Title);
    }

    // 1. The settings we were passed contains STARTUPINFO structure settings to be applied last.
    settings->ApplyStartupInfo(pStartupSettings);

    // Validate all applied settings for correctness against final rules.
    settings->Validate();

    // As of the graphics refactoring to library based, all fonts are now DPI aware. Scaling is performed at the Blt time for raster fonts.
    // Note that we can only declare our DPI awareness once per process launch.
    // Set the process's default dpi awareness context to PMv2 so that new top level windows
    // inherit their WM_DPICHANGED* broadcast mode (and more, like dialog scaling) from the thread.

    IHighDpiApi *pHighDpiApi = ServiceLocator::LocateHighDpiApi();
    if (pHighDpiApi)
    {
        // N.B.: There is no high DPI support on OneCore (non-UAP) systems.
        //       Instead of implementing a no-op interface, just skip all high
        //       DPI configuration if it is not supported. All callers into the
        //       high DPI API are in the Win32-specific interactivity DLL.
        if (!pHighDpiApi->SetProcessDpiAwarenessContext())
        {
            // Fallback to per-monitor aware V1 if the API isn't available.
            pHighDpiApi->SetProcessPerMonitorDpiAwareness();

            // Allow child dialogs (i.e. Properties and Find) to scale automatically based on DPI if we're currently DPI aware.
            // Note that we don't need to do this if we're PMv2.
            pHighDpiApi->EnablePerMonitorDialogScaling();
        }
    }

    //Save initial font name for comparison on exit. We want telemetry when the font has changed
    if (settings->IsFaceNameSet())
    {
        settings->SetLaunchFaceName(settings->GetFaceName(), LF_FACESIZE);
    }

    // Now we need to actually create the console using the settings given.
#pragma prefast(suppress:26018, "PREfast can't detect null termination status of Title.")

// Allocate console will read the global ServiceLocator::LocateGlobals()->getConsoleInformation for the settings we just set.
    NTSTATUS Status = AllocateConsole(Title, TitleLength);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS RemoveConsole(_In_ ConsoleProcessHandle* ProcessData)
{
    CONSOLE_INFORMATION* const gci = ServiceLocator::LocateGlobals()->getConsoleInformation();
    CONSOLE_INFORMATION *Console;
    NTSTATUS Status = RevalidateConsole(&Console);
    ASSERT(NT_SUCCESS(Status));

    FreeCommandHistory((HANDLE)ProcessData);

    bool const fRecomputeOwner = ProcessData->fRootProcess;
    gci->ProcessHandleList.FreeProcessData(ProcessData);

    if (fRecomputeOwner)
    {
        IConsoleWindow* pWindow = ServiceLocator::LocateConsoleWindow();
        if (pWindow != nullptr)
        {
            pWindow->SetOwner();
        }
    }

    UnlockConsole();

    return Status;
}

DWORD ConsoleIoThread();

void ConsoleCheckDebug()
{
#ifdef DBG
    HKEY hCurrentUser;
    HKEY hConsole;
    NTSTATUS status = RegistrySerialization::s_OpenConsoleKey(&hCurrentUser, &hConsole);

    if (NT_SUCCESS(status))
    {
        DWORD dwData = 0;
        status = RegistrySerialization::s_QueryValue(hConsole, L"DebugLaunch", sizeof(dwData), (BYTE*)&dwData, nullptr);

        if (NT_SUCCESS(status))
        {
            if (dwData != 0)
            {
                DebugBreak();
            }
        }

        RegCloseKey(hConsole);
        RegCloseKey(hCurrentUser);
    }
#endif
}

HRESULT ConsoleCreateIoThreadLegacy(_In_ HANDLE Server)
{
    ConsoleCheckDebug();

    RETURN_IF_FAILED(ConsoleServerInitialization(Server));
    RETURN_IF_FAILED(ServiceLocator::LocateGlobals()->hConsoleInputInitEvent.create(wil::EventOptions::None));

    // Set up and tell the driver about the input available event.
    RETURN_IF_FAILED(ServiceLocator::LocateGlobals()->hInputEvent.create(wil::EventOptions::ManualReset));

    CD_IO_SERVER_INFORMATION ServerInformation;
    ServerInformation.InputAvailableEvent = ServiceLocator::LocateGlobals()->hInputEvent.get();
    RETURN_IF_FAILED(ServiceLocator::LocateGlobals()->pDeviceComm->SetServerInformation(&ServerInformation));

    HANDLE const hThread = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)ConsoleIoThread, 0, 0, nullptr);
    RETURN_IF_HANDLE_NULL(hThread);
    LOG_IF_WIN32_BOOL_FALSE(CloseHandle(hThread)); // The thread will run on its own and close itself. Free the associated handle.

    return S_OK;
}

HRESULT ConsoleCreateIoThread(_In_ HANDLE Server)
{
    return Entrypoints::StartConsoleForServerHandle(Server);
}

#define SYSTEM_ROOT         (L"%SystemRoot%")
#define SYSTEM_ROOT_LENGTH  (sizeof(SYSTEM_ROOT) - sizeof(WCHAR))

// Routine Description:
// - This routine translates path characters into '_' characters because the NT registry apis do not allow the creation of keys with
//   names that contain path characters. It also converts absolute paths into %SystemRoot% relative ones. As an example, if both behaviors were
//   specified it would convert a title like C:\WINNT\System32\cmd.exe to %SystemRoot%_System32_cmd.exe.
// Arguments:
// - ConsoleTitle - Pointer to string to translate.
// - Unexpand - Convert absolute path to %SystemRoot% relative one.
// - Substitute - Whether string-substitution ('_' for '\') should occur.
// Return Value:
// - Pointer to translated title or nullptr.
// Note:
// - This routine allocates a buffer that must be freed.
PWSTR TranslateConsoleTitle(_In_ PCWSTR pwszConsoleTitle, _In_ const BOOL fUnexpand, _In_ const BOOL fSubstitute)
{
    LPWSTR Tmp = nullptr;

    size_t cbConsoleTitle;
    size_t cbSystemRoot;

    LPWSTR pwszSysRoot = new wchar_t[MAX_PATH];
    if (nullptr != pwszSysRoot)
    {
        if (0 != GetWindowsDirectoryW(pwszSysRoot, MAX_PATH))
        {
            if (SUCCEEDED(StringCbLengthW(pwszConsoleTitle, STRSAFE_MAX_CCH, &cbConsoleTitle)) &&
                SUCCEEDED(StringCbLengthW(pwszSysRoot, MAX_PATH, &cbSystemRoot)))
            {
                int const cchSystemRoot = (int)(cbSystemRoot / sizeof(WCHAR));
                int const cchConsoleTitle = (int)(cbConsoleTitle / sizeof(WCHAR));
                cbConsoleTitle += sizeof(WCHAR); // account for nullptr terminator

                if (fUnexpand &&
                    cchConsoleTitle >= cchSystemRoot &&
#pragma prefast(suppress:26018, "We've guaranteed that cchSystemRoot is equal to or smaller than cchConsoleTitle in size.")
                    (CSTR_EQUAL == CompareStringOrdinal(pwszConsoleTitle, cchSystemRoot, pwszSysRoot, cchSystemRoot, TRUE)))
                {
                    cbConsoleTitle -= cbSystemRoot;
                    pwszConsoleTitle += cchSystemRoot;
                    cbSystemRoot = SYSTEM_ROOT_LENGTH;
                }
                else
                {
                    cbSystemRoot = 0;
                }

                LPWSTR pszTranslatedConsoleTitle;
                const size_t cbTranslatedConsoleTitle = cbSystemRoot + cbConsoleTitle;
                Tmp = pszTranslatedConsoleTitle = (PWSTR)new BYTE[cbTranslatedConsoleTitle];
                if (pszTranslatedConsoleTitle == nullptr)
                {
                    return nullptr;
                }

                // No need to check return here -- pszTranslatedConsoleTitle is guaranteed large enough for SYSTEM_ROOT
                (void)StringCbCopy(pszTranslatedConsoleTitle, cbTranslatedConsoleTitle, SYSTEM_ROOT);
                pszTranslatedConsoleTitle += (cbSystemRoot / sizeof(WCHAR));   // skip by characters -- not bytes

                for (UINT i = 0; i < cbConsoleTitle; i += sizeof(WCHAR))
                {
#pragma prefast(suppress:26018, "We are reading the null portion of the buffer on purpose and will escape on reaching it below.")
                    if (fSubstitute && *pwszConsoleTitle == '\\')
                    {
#pragma prefast(suppress:26019, "Console title must contain system root if this path was followed.")
                        *pszTranslatedConsoleTitle++ = (WCHAR)'_';
                    }
                    else
                    {
                        *pszTranslatedConsoleTitle++ = *pwszConsoleTitle;
                        if (*pwszConsoleTitle == L'\0')
                        {
                            break;
                        }
                    }

                    pwszConsoleTitle++;
                }
            }
        }
        delete[] pwszSysRoot;
    }

    return Tmp;
}

NTSTATUS GetConsoleLangId(_In_ const UINT uiOutputCP, _Out_ LANGID * const pLangId)
{
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    // -- WARNING -- LOAD BEARING CODE --
    // Only attempt to return the Lang ID if the Windows ACP on console launch was an East Asian Code Page.
    // -
    // As of right now, this is a load bearing check and causes a domino effect of errors during OEM preinstallation if removed
    // resulting in a crash on launch of CMD.exe
    // (and consequently any scripts OEMs use to customize an image during the auditUser preinstall step inside their unattend.xml files.)
    // I have no reason to believe that removing this check causes any problems on any other SKU or scenario types.
    // -
    // Returning STATUS_NOT_SUPPORTED will skip a call to SetThreadLocale inside the Windows loader. This has the effect of not
    // setting the appropriate locale on the client end of the pipe, but also avoids the error.
    // Returning STATUS_SUCCESS will trigger the call to SetThreadLocale inside the loader.
    // This method is called on process launch by the loader and on every SetConsoleOutputCP call made from the client application to
    // maintain the synchrony of the client's Thread Locale state.
    // -
    // It is important to note that a comment exists inside the loader stating that DBCS code pages (CJK languages)
    // must have the SetThreadLocale synchronized with the console in order for FormatMessage to output correctly.
    // I'm not sure of the full validity of that comment at this point in time (Nov 2016), but the least risky thing is to trust it and revert
    // the behavior to this function until it can be otherwise proven.
    // -
    // See MSFT: 9808579 for the complete story on what happened here and why this must stay until the other dominos are resolved.
    // -
    // I would also highly advise against expanding the LANGIDs returned here or modifying them in any way until the cascading impacts
    // discovered in MSFT: 9808579 are vetted against any changes.
    // -- END WARNING --
    if (IsAvailableEastAsianCodePage(ServiceLocator::LocateGlobals()->uiWindowsCP))
    {
        if (pLangId != nullptr)
        {
            switch (uiOutputCP)
            {
            case CP_JAPANESE:
                *pLangId = MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT);
                break;
            case CP_KOREAN:
                *pLangId = MAKELANGID(LANG_KOREAN, SUBLANG_KOREAN);
                break;
            case CP_CHINESE_SIMPLIFIED:
                *pLangId = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
                break;
            case CP_CHINESE_TRADITIONAL:
                *pLangId = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);
                break;
            default:
                *pLangId = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
                break;
            }
        }
        Status = STATUS_SUCCESS;
    }

    return Status;
}

HRESULT ApiRoutines::GetConsoleLangIdImpl(_Out_ LANGID* const pLangId)
{
    const CONSOLE_INFORMATION* const gci = ServiceLocator::LocateGlobals()->getConsoleInformation();
    LockConsole();
    auto Unlock = wil::ScopeExit([&] { UnlockConsole(); });

    RETURN_NTSTATUS(GetConsoleLangId(gci->OutputCP, pLangId));
}

// Routine Description:
// - This routine reads the connection information from a 'connect' IO, validates it and stores them in an internal format.
// - N.B. The internal informat contains information not sent by clients in their connect IOs and intialized by other routines.
// Arguments:
// - Server - Supplies a handle to the console server.
// - Message - Supplies the message representing the connect IO.
// - Cac - Receives the connection information.
// Return Value:
// - NTSTATUS indicating if the connection information was successfully initialized.
NTSTATUS ConsoleInitializeConnectInfo(_In_ PCONSOLE_API_MSG Message, _Out_ PCONSOLE_API_CONNECTINFO Cac)
{
    CONSOLE_SERVER_MSG Data = { 0 };

    // Try to receive the data sent by the client.
    NTSTATUS Status = NTSTATUS_FROM_HRESULT(Message->ReadMessageInput(0, &Data, sizeof(Data)));
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    // Validate that strings are within the buffers and null-terminated.
    if ((Data.ApplicationNameLength > (sizeof(Data.ApplicationName) - sizeof(WCHAR))) ||
        (Data.TitleLength > (sizeof(Data.Title) - sizeof(WCHAR))) ||
        (Data.CurrentDirectoryLength > (sizeof(Data.CurrentDirectory) - sizeof(WCHAR))) ||
        (Data.ApplicationName[Data.ApplicationNameLength / sizeof(WCHAR)] != UNICODE_NULL) ||
        (Data.Title[Data.TitleLength / sizeof(WCHAR)] != UNICODE_NULL) || (Data.CurrentDirectory[Data.CurrentDirectoryLength / sizeof(WCHAR)] != UNICODE_NULL))
    {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    // Initialize (partially) the connect info with the received data.
    ASSERT(sizeof(Cac->AppName) == sizeof(Data.ApplicationName));
    ASSERT(sizeof(Cac->Title) == sizeof(Data.Title));
    ASSERT(sizeof(Cac->CurDir) == sizeof(Data.CurrentDirectory));

    // unused(Data.IconId)
    Cac->ConsoleInfo.SetHotKey(Data.HotKey);
    Cac->ConsoleInfo.SetStartupFlags(Data.StartupFlags);
    Cac->ConsoleInfo.SetFillAttribute(Data.FillAttribute);
    Cac->ConsoleInfo.SetShowWindow(Data.ShowWindow);
    Cac->ConsoleInfo.SetScreenBufferSize(Data.ScreenBufferSize);
    Cac->ConsoleInfo.SetWindowSize(Data.WindowSize);
    Cac->ConsoleInfo.SetWindowOrigin(Data.WindowOrigin);
    Cac->ProcessGroupId = Data.ProcessGroupId;
    Cac->ConsoleApp = Data.ConsoleApp;
    Cac->WindowVisible = Data.WindowVisible;
    Cac->TitleLength = Data.TitleLength;
    Cac->AppNameLength = Data.ApplicationNameLength;
    Cac->CurDirLength = Data.CurrentDirectoryLength;

    memmove(Cac->AppName, Data.ApplicationName, sizeof(Cac->AppName));
    memmove(Cac->Title, Data.Title, sizeof(Cac->Title));
    memmove(Cac->CurDir, Data.CurrentDirectory, sizeof(Cac->CurDir));

    return STATUS_SUCCESS;
}

NTSTATUS ConsoleAllocateConsole(PCONSOLE_API_CONNECTINFO p)
{
    // AllocConsole is outside our codebase, but we should be able to mostly track the call here.
    Telemetry::Instance().LogApiCall(Telemetry::ApiCall::AllocConsole);
    CONSOLE_INFORMATION* const gci = ServiceLocator::LocateGlobals()->getConsoleInformation();

    NTSTATUS Status = SetUpConsole(&p->ConsoleInfo, p->TitleLength, p->Title, p->CurDir, p->AppName);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    if (NT_SUCCESS(Status) && p->WindowVisible)
    {
        HANDLE Thread = nullptr;

        IConsoleInputThread *pNewThread = nullptr;
        ServiceLocator::CreateConsoleInputThread(&pNewThread);

        ASSERT(pNewThread);

        Thread = pNewThread->Start();
        if (Thread == nullptr)
        {
            Status = STATUS_NO_MEMORY;
        }
        else
        {
            ServiceLocator::LocateGlobals()->dwInputThreadId = pNewThread->GetThreadId();

            // The ConsoleInputThread needs to lock the console so we must first unlock it ourselves.
            UnlockConsole();
            ServiceLocator::LocateGlobals()->hConsoleInputInitEvent.wait();
            LockConsole();

            CloseHandle(Thread);
            ServiceLocator::LocateGlobals()->hConsoleInputInitEvent.release();

            if (!NT_SUCCESS(ServiceLocator::LocateGlobals()->ntstatusConsoleInputInitStatus))
            {
                Status = ServiceLocator::LocateGlobals()->ntstatusConsoleInputInitStatus;
            }
            else
            {
                Status = STATUS_SUCCESS;
            }

            /*
             * Tell driver to allow clients with UIAccess to connect
             * to this server even if the security descriptor doesn't
             * allow it.
             *
             * N.B. This allows applications like narrator.exe to have
             *      access to the console. This is ok because they already
             *      have access to the console window anyway - this function
             *      is only called when a window is created.
             */

            LOG_IF_FAILED(ServiceLocator::LocateGlobals()->pDeviceComm->AllowUIAccess());
        }
    }
    else
    {
        gci->Flags |= CONSOLE_NO_WINDOW;
    }

    return Status;
}

// Routine Description:
// - This routine is the main one in the console server IO thread.
// - It reads IO requests submitted by clients through the driver, services and completes them in a loop.
// Arguments:
// - <none>
// Return Value:
// - This routine never returns. The process exits when no more references or clients exist.
DWORD ConsoleIoThread()
{
    ApiRoutines Routines;
    CONSOLE_API_MSG ReceiveMsg;
    ReceiveMsg._pApiRoutines = &Routines;
    ReceiveMsg._pDeviceComm = ServiceLocator::LocateGlobals()->pDeviceComm;
    PCONSOLE_API_MSG ReplyMsg = nullptr;

    bool fShouldExit = false;
    while (!fShouldExit)
    {
        if (ReplyMsg != nullptr)
        {
            ReplyMsg->ReleaseMessageBuffers();
        }

        // TODO: 9115192 correct mixed NTSTATUS/HRESULT
        HRESULT hr = ServiceLocator::LocateGlobals()->pDeviceComm->ReadIo(&ReplyMsg->Complete, &ReceiveMsg);
        if (FAILED(hr))
        {
            if (hr == HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED))
            {
                fShouldExit = true;

                // This will not return. Terminate immediately when disconnected.
                TerminateProcess(GetCurrentProcess(), STATUS_SUCCESS);
            }
            RIPMSG1(RIP_WARNING, "DeviceIoControl failed with Result 0x%x", hr);
            ReplyMsg = nullptr;
            continue;
        }

        IoSorter::ServiceIoOperation(&ReceiveMsg, &ReplyMsg);
    }

    return 0;
}
