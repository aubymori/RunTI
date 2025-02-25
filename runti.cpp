#include "runti.h"
#include <stdio.h>

WCHAR *g_pszCommandLine = nullptr;

INT_PTR CALLBACK RunDlgProc(
	HWND   hWnd,
	UINT   uMsg,
	WPARAM wParam,
	LPARAM lParam
)
{
	switch (uMsg)
	{
		case WM_CLOSE:
			EndDialog(hWnd, IDCANCEL);
			return TRUE;
		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
				case IDOK:
				{
					HWND hwndCommand = GetDlgItem(hWnd, IDD_COMMAND);
					int length = GetWindowTextLengthW(hwndCommand) + 1;
					g_pszCommandLine = new WCHAR[length];
					GetWindowTextW(hwndCommand, g_pszCommandLine, length);
					EndDialog(hWnd, IDOK);
					break;
				}
				case IDCANCEL:
					EndDialog(hWnd, IDCANCEL);
					break;
				case IDD_BROWSE:
				{
					WCHAR szFile[MAX_PATH] = { 0 };
					OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
					ofn.hwndOwner = hWnd;
					ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All Files (*.*)\0*.*\0\0";
					ofn.lpstrFile = szFile;
					ofn.nMaxFile = MAX_PATH;
					ofn.lpstrTitle = L"Browse";
					ofn.lpstrDefExt = L"exe";
					ofn.Flags = OFN_EXPLORER | OFN_ENABLESIZING | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
					if (GetOpenFileNameW(&ofn))
					{
						if (wcschr(szFile, L' '))
						{
							WCHAR szQuotedFile[MAX_PATH + 2];
							swprintf_s(szQuotedFile, L"\"%s\"", szFile);
							SetDlgItemTextW(hWnd, IDD_COMMAND, szQuotedFile);
						}
						else
						{
							SetDlgItemTextW(hWnd, IDD_COMMAND, szFile);
						}
					}
				}
			}
			return TRUE;
	}

	return FALSE;
}

bool EnablePrivilege(LPCWSTR lpPrivilegeName)
{
	wil::unique_handle hToken;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &hToken))
		return false;

	LUID luid;
	if (!LookupPrivilegeValueW(nullptr, lpPrivilegeName, &luid))
		return false;

	TOKEN_PRIVILEGES tp;
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	BOOL fSucceeded = AdjustTokenPrivileges(hToken.get(), FALSE, &tp, sizeof(tp), nullptr, nullptr);
	return fSucceeded;
}

bool GetProcessIdByName(LPCWSTR lpProcessName, LPDWORD lpdwPid)
{
	*lpdwPid = -1;

	wil::unique_handle hSnapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL));
	if (hSnapshot.get() == INVALID_HANDLE_VALUE)
		return false;
	
	PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W)};
	if (Process32FirstW(hSnapshot.get(), &pe))
	{
		do
		{
			if (0 == wcscmp(pe.szExeFile, lpProcessName))
			{
				*lpdwPid = pe.th32ProcessID;
				break;
			}
		} while (Process32NextW(hSnapshot.get(), &pe));
	}
	
	return (*lpdwPid != -1);
}

bool ImpersonateSystem(void)
{
	DWORD dwWinlogonPid;
	if (!GetProcessIdByName(L"winlogon.exe", &dwWinlogonPid))
		return false;

	wil::unique_handle hWinlogonProcess(OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION, FALSE, dwWinlogonPid));
	if (!hWinlogonProcess.get())
		return false;

	wil::unique_handle hWinlogonToken;
	if (!OpenProcessToken(
		hWinlogonProcess.get(),
		MAXIMUM_ALLOWED,
		&hWinlogonToken
	))
		return false;

	wil::unique_handle hDupToken;
	SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES) };
	if (!DuplicateTokenEx(
		hWinlogonToken.get(),
		MAXIMUM_ALLOWED,
		&sa,
		SecurityImpersonation,
		TokenImpersonation,
		&hDupToken
	))
		return false;

	if (!ImpersonateLoggedOnUser(hDupToken.get()))
		return false;

	return true;
}

bool StartTrustedInstallerService(LPDWORD lpdwPid)
{
	*lpdwPid = -1;

	wil::unique_schandle hSCManager(OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASE, GENERIC_EXECUTE));
	if (!hSCManager.get())
		return false;

	wil::unique_schandle hService(OpenServiceW(hSCManager.get(), L"TrustedInstaller", GENERIC_READ | GENERIC_EXECUTE));
	if (!hService.get())
		return false;

	SERVICE_STATUS_PROCESS status;
	DWORD cbNeeded;
	while (QueryServiceStatusEx(
		hService.get(),
		SC_STATUS_PROCESS_INFO,
		(LPBYTE)&status,
		sizeof(status),
		&cbNeeded
	))
	{
		switch (status.dwCurrentState)
		{
			case SERVICE_STOPPED:
				if (!StartServiceW(hService.get(), 0, nullptr))
					return false;
				break;
			case SERVICE_START_PENDING:
			case SERVICE_STOP_PENDING:
				Sleep(status.dwWaitHint);
				continue;
			case SERVICE_RUNNING:
				*lpdwPid = status.dwProcessId;
				return true;
		}
	}

	return false;
}

bool CreateProcessAsTrustedInstaller(DWORD dwTIPid, LPCWSTR pszCommandLine, LPPROCESS_INFORMATION ppi)
{
	if (!EnablePrivilege(SE_DEBUG_NAME) || !EnablePrivilege(SE_IMPERSONATE_NAME) || !ImpersonateSystem())
		return false;

	wil::unique_handle hTIProcess(OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION, FALSE, dwTIPid));
	if (!hTIProcess.get())
		return false;

	wil::unique_handle hTIToken;
	if (!OpenProcessToken(
		hTIProcess.get(),
		MAXIMUM_ALLOWED,
		&hTIToken
	))
		return false;

	wil::unique_handle hDupToken;
	SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES) };
	if (!DuplicateTokenEx(
		hTIToken.get(),
		MAXIMUM_ALLOWED,
		&sa,
		SecurityImpersonation,
		TokenImpersonation,
		&hDupToken
	))
		return false;

	STARTUPINFOW si = { 0 };
	si.lpDesktop = (LPWSTR)L"Winsta0\\Default";
	ZeroMemory(ppi, sizeof(PROCESS_INFORMATION));
	if (!CreateProcessWithTokenW(
		hDupToken.get(),
		LOGON_WITH_PROFILE,
		nullptr,
		(LPWSTR)pszCommandLine,
		CREATE_UNICODE_ENVIRONMENT,
		nullptr,
		nullptr,
		&si,
		ppi
	))
		return false;

	return true;
}

int WINAPI wWinMain(
	_In_     HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_     LPWSTR    lpCmdLine,
	_In_     int       nCmdShow
)
{
	INT_PTR iResult = DialogBoxParamW(
		hInstance, MAKEINTRESOURCEW(DLG_RUN),
		NULL, RunDlgProc, NULL
	);

	if (iResult == IDOK)
	{
		DWORD dwTIPid;
		if (!StartTrustedInstallerService(&dwTIPid))
		{
			MessageBoxW(NULL, L"Failed to start the TrustedInstaller service", L"RunTI", MB_ICONERROR);
			return 1;
		}

		PROCESS_INFORMATION pi;
		if (!CreateProcessAsTrustedInstaller(dwTIPid, g_pszCommandLine, &pi))
		{
			MessageBoxW(NULL, L"Failed to run the command as TrustedInstaller", L"RunTI", MB_ICONERROR);
			return 1;
		}

		delete[] g_pszCommandLine;
	}

	return 0;
}