// chrlauncher
// Copyright (c) 2016 Henry++

#include <windows.h>
#include <psapi.h>
#include <regex>

#include "main.h"
#include "rapp.h"
#include "routine.h"

#include "resource.h"

#include "unzip.h"

rapp app (APP_NAME, APP_NAME_SHORT, APP_VERSION, APP_COPYRIGHT);

#define CHROMIUM_UPDATE_URL L"http://chromium.woolyss.com/api/v2/?os=windows&bit=%d&out=xml"

rstring browser_name;
rstring browser_name_full;
rstring browser_directory;
rstring browser_exe;
rstring browser_args;
rstring browser_version;
rstring browser_url;

DWORD browser_architecture = 0;

VOID _Application_SetPercent (DWORD v, DWORD t)
{
	INT percent = 0;

	if (t)
	{
		percent = (double (v) / double (t)) * 100.0;
	}

	SendDlgItemMessage (app.GetHWND (), IDC_PROGRESS, PBM_SETPOS, percent, 0);

	_r_status_settext (app.GetHWND (), IDC_STATUSBAR, 1, _r_fmt (L"%s/%s", _r_fmt_size64 (v), _r_fmt_size64 (t)));
}

VOID _Application_SetStatus (LPCWSTR text)
{
	_r_status_settext (app.GetHWND (), IDC_STATUSBAR, 0, text);
}

rstring xml_parse (std::wstring s, LPCWSTR v)
{
	std::wsmatch m;
	std::wregex e (_r_fmt (L"<%s>(.*?)<\\/%s>", v, v).GetString ());

	return std::regex_search (s, m, e) ? m[1].str ().c_str () : nullptr;
}

BOOL _Browser_InstallZip (LPCWSTR path, LPCWSTR version)
{
	BOOL result = FALSE;

	ZIPENTRY ze = {0};

	HZIP hz = OpenZip (path, nullptr);

	const size_t title_length = 12;

	if (IsZipHandleU (hz))
	{
		DWORD total_size = 0;
		DWORD count_all = 0; // this is our progress so far

		size_t version_len = wcslen (version);

		// count total files
		GetZipItem (hz, -1, &ze);
		INT total_files = ze.index;

		// check archive is right package
		GetZipItem (hz, 0, &ze);

		if (wcsncmp (ze.name, L"chrome-win32", title_length) == 0)
		{
			// count size of unpacked files
			for (INT i = 0; i < total_files; i++)
			{
				GetZipItem (hz, i, &ze);

				total_size += ze.unc_size;
			}

			rstring path;
			rstring file;

			for (INT i = 1; i < total_files; i++)
			{
				GetZipItem (hz, i, &ze);

				file = ze.name + title_length + 1;
				path.Format (L"%s\\%s", browser_directory, file);

				if ((ze.attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
				{
					SHCreateDirectoryEx (nullptr, path, nullptr);
				}
				else
				{
					// don't unpack all manifest files, except manifest with main version
					if (file.Match (L"*.manifest") && wcsncmp (version, file, version_len) != 0)
					{
						count_all += ze.unc_size; // increment position

						DeleteFile (path); // delete rubish

						continue;
					}

					HANDLE f = CreateFile (path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

					if (f != INVALID_HANDLE_VALUE)
					{
						CHAR* buff = new CHAR[_R_BUFFER_LENGTH * 2];
						DWORD count_file = 0;

						for (ZRESULT zr = ZR_MORE; zr == ZR_MORE;)
						{
							size_t bufsize = _R_BUFFER_LENGTH;

							zr = UnzipItem (hz, i, buff, bufsize);

							if (zr == ZR_OK)
							{
								bufsize = ze.unc_size - count_file;
							}

							DWORD written = 0;
							WriteFile (f, buff, bufsize, &written, nullptr);

							count_file += bufsize;
							count_all += bufsize;

							_Application_SetPercent (count_all, total_size);
						}

						delete[] buff;

						CloseHandle (f);
					}
				}
			}

			result = TRUE;
		}

		CloseZip (hz);
	}

	return result;
}

BOOL _Browser_InstallUpdate (LPCWSTR path, LPCWSTR version)
{
	BOOL result = TRUE;
	BOOL is_ready = TRUE;

	// installing update
	_Application_SetStatus (I18N (&app, IDS_STATUS_INSTALL, 0));
	_Application_SetPercent (0, 0);

	// check install folder for running processes
	while (1)
	{
		is_ready = !_r_process_is_exists (browser_directory, browser_directory.GetLength ());

		if (is_ready || _r_msg (app.GetHWND (), MB_RETRYCANCEL | MB_ICONEXCLAMATION, APP_NAME, I18N (&app, IDS_STATUS_CLOSEBROWSER, 0)) != IDRETRY)
		{
			break;
		}
	}

	// create directory
	SHCreateDirectoryEx (nullptr, browser_directory, nullptr);

	if (is_ready)
	{
		result = _Browser_InstallZip (path, version);

		if (result)
			app.ConfigSet (L"BrowserCheckDate", DWORD (_r_unixtime_now ()));

		DeleteFile (path);
	}

	return result;
}

BOOL _Browser_DownloadUpdate (LPCWSTR url, LPCWSTR version)
{
	BOOL result = FALSE;

	HINTERNET internet = nullptr;
	HINTERNET connect = nullptr;

	// get path
	WCHAR path[MAX_PATH] = {0};

	GetTempPath (MAX_PATH, path);
	GetTempFileName (path, nullptr, 0, path);

	// download file
	_Application_SetStatus (I18N (&app, IDS_STATUS_DOWNLOAD, 0));
	_Application_SetPercent (0, 0);

	internet = InternetOpen (app.GetUserAgent (), INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);

	if (internet)
	{
		connect = InternetOpenUrl (internet, url, nullptr, 0, INTERNET_FLAG_RESYNCHRONIZE | INTERNET_FLAG_NO_COOKIES, 0);

		if (connect)
		{
			DWORD status = 0, size = sizeof (status);
			HttpQueryInfo (connect, HTTP_QUERY_FLAG_NUMBER | HTTP_QUERY_STATUS_CODE, &status, &size, nullptr);

			if (status == HTTP_STATUS_OK)
			{
				DWORD total_size = 0;

				size = sizeof (total_size);

				HttpQueryInfo (connect, HTTP_QUERY_FLAG_NUMBER | HTTP_QUERY_CONTENT_LENGTH, &total_size, &size, nullptr);

				DWORD out = 0;
				DWORD total_written = 0;

				CHAR buff[_R_BUFFER_LENGTH];

				HANDLE f = CreateFile (path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

				if (f != INVALID_HANDLE_VALUE)
				{
					while (1)
					{
						if (!InternetReadFile (connect, buff, _R_BUFFER_LENGTH - 1, &out) || !out)
						{
							break;
						}

						DWORD written = 0;
						WriteFile (f, buff, out, &written, nullptr);

						total_written += out;

						_Application_SetPercent (total_written, total_size);
					}

					CloseHandle (f);
				}

				_Browser_InstallUpdate (path, version);
			}
		}
	}

	return result;
}

UINT WINAPI _Browser_CheckUpdate (LPVOID)
{
	HINTERNET internet = nullptr, connect = nullptr;

	rstring::map_one result;

	DWORD days = app.ConfigGet (L"BrowserCheckPeriod", 1);
	BOOL is_exists = _r_file_is_exists (browser_exe);
	BOOL is_success = FALSE;

	if (days || !is_exists)
	{
		if (!is_exists || _r_unixtime_now () - app.ConfigGet (L"BrowserCheckDate", 0) >= (86400 * days))
		{
			internet = InternetOpen (app.GetUserAgent (), INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);

			if (internet)
			{
				rstring url;
				url.Format (CHROMIUM_UPDATE_URL, browser_architecture);

				connect = InternetOpenUrl (internet, url, nullptr, 0, INTERNET_FLAG_RESYNCHRONIZE | INTERNET_FLAG_NO_COOKIES, 0);

				if (connect)
				{
					DWORD status = 0, size = sizeof (status);
					HttpQueryInfo (connect, HTTP_QUERY_FLAG_NUMBER | HTTP_QUERY_STATUS_CODE, &status, &size, nullptr);

					if (status == HTTP_STATUS_OK)
					{
						DWORD out = 0;

						CHAR buffera[_R_BUFFER_LENGTH] = {0};
						rstring bufferw;

						while (1)
						{
							if (!InternetReadFile (connect, buffera, _R_BUFFER_LENGTH - 1, &out) || !out)
								break;

							buffera[out] = 0;
							bufferw.Append (buffera);
						}

						if (!bufferw.IsEmpty ())
						{
							result[L"version"] = xml_parse (bufferw.GetString (), L"version");
							result[L"url"] = xml_parse (bufferw.GetString (), L"download");
							result[L"date"] = _r_fmt_date (wcstoull (xml_parse (bufferw.GetString (), L"timestamp"), nullptr, 10), FDTF_SHORTDATE | FDTF_SHORTTIME);

							if (result[L"url"].IsEmpty ())
							{
								result[L"url"].Format (L"https://storage.googleapis.com/chromium-browser-continuous/%s/%s/chrome-win32.zip", browser_architecture == 32 ? L"Win" : L"Win_x64", xml_parse (bufferw.GetString (), L"revision"));
							}

							is_success = TRUE;
						}
					}
				}
			}

			InternetCloseHandle (connect);
			InternetCloseHandle (internet);
		}

		if (is_success)
		{
			SetDlgItemText (app.GetHWND (), IDC_BROWSER, _r_fmt (I18N (&app, IDS_BROWSER, 0), browser_name_full));
			SetDlgItemText (app.GetHWND (), IDC_CURRENTVERSION, _r_fmt (I18N (&app, IDS_CURRENTVERSION, 0), browser_version.IsEmpty () ? L"<not found>" : browser_version));
			SetDlgItemText (app.GetHWND (), IDC_VERSION, _r_fmt (I18N (&app, IDS_VERSION, 0), result[L"version"]));
			SetDlgItemText (app.GetHWND (), IDC_DATE, _r_fmt (I18N (&app, IDS_DATE, 0), result[L"date"]));

			// check for new version
			if (!is_exists || result[L"version"].CompareNoCase (browser_version) != 0)
			{
				_r_windowtoggle (app.GetHWND (), TRUE);

				_Browser_DownloadUpdate (result[L"url"], result[L"version"]);
			}
		}
		else
		{
			app.ConfigSet (L"BrowserCheckDate", DWORD (_r_unixtime_now ()));
		}
	}

	PostMessage (app.GetHWND (), WM_DESTROY, 0, 0);

	return ERROR_SUCCESS;
}

rstring _Browser_GetVersion (LPCWSTR path)
{
	rstring result;

	DWORD verHandle = 0;
	DWORD verSize = GetFileVersionInfoSize (path, &verHandle);

	if (verSize)
	{
		LPSTR verData = new char[verSize];

		if (GetFileVersionInfo (path, verHandle, verSize, verData))
		{
			LPBYTE buffer = nullptr;
			UINT size = 0;

			if (VerQueryValue (verData, L"\\", (VOID FAR* FAR*)&buffer, &size))
			{
				if (size)
				{
					VS_FIXEDFILEINFO *verInfo = (VS_FIXEDFILEINFO*)buffer;

					if (verInfo->dwSignature == 0xfeef04bd)
					{
						// Doesn't matter if you are on 32 bit or 64 bit,
						// DWORD is always 32 bits, so first two revision numbers
						// come from dwFileVersionMS, last two come from dwFileVersionLS

						result.Format (L"%d.%d.%d.%d", (verInfo->dwFileVersionMS >> 16) & 0xffff, (verInfo->dwFileVersionMS >> 0) & 0xffff, (verInfo->dwFileVersionLS >> 16) & 0xffff, (verInfo->dwFileVersionLS >> 0) & 0xffff);
					}
				}
			}
		}

		delete[] verData;
	}

	return result;
}

BOOL _Browser_Run ()
{
	if (!browser_url.IsEmpty ())
	{
		browser_args.Append (L" -- \"");
		browser_args.Append (browser_url);
		browser_args.Append (L"\"");
	}

	return _r_run (nullptr, _r_fmt (L"\"%s\" %s", browser_exe, browser_args).GetBuffer (), browser_directory);
}

INT_PTR CALLBACK DlgProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
		case WM_INITDIALOG:
		{
			app.SetHWND (hwnd);

			SetCurrentDirectory (app.GetDirectory ());

			INT parts[] = {app.GetDPI (230), -1};
			SendDlgItemMessage (hwnd, IDC_STATUSBAR, SB_SETPARTS, 2, (LPARAM)parts);

			// parse command line
			INT numargs = 0;
			LPWSTR* arga = CommandLineToArgvW (GetCommandLine (), &numargs);

			for (INT i = 1; i < numargs; i++)
			{
				if (_wcsicmp (arga[i], L"/url") == 0 && i < numargs)
				{
					browser_url = arga[i + 1];
				}
			}

			LocalFree (arga);

			// get browser architecture...
			browser_architecture = app.ConfigGet (L"BrowserArchitecture", 0);

			if (!browser_architecture || (browser_architecture != 64 && browser_architecture != 32))
			{
				browser_architecture = 0;

				// on XP only 32-bit supported
				if (_r_system_validversion (5, 1, VER_EQUAL))
				{
					browser_architecture = 32;
				}

				// ...by executable
				DWORD exe_type = 0;

				if (GetBinaryType (browser_exe, &exe_type))
				{
					if (exe_type == SCS_32BIT_BINARY)
					{
						browser_architecture = 32;
					}
					else if (exe_type == SCS_64BIT_BINARY)
					{
						browser_architecture = 64;
					}
				}

				if (!browser_architecture)
				{
					// ...by processor browser_architecture
					SYSTEM_INFO si = {0};

					GetNativeSystemInfo (&si);

					if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) { browser_architecture = 64; }
					else { browser_architecture = 32; }
				}
			}

			// configure paths
			browser_name_full.Format (L"Chromium (%d-bit)", browser_architecture);
			browser_directory = _r_normalize_path (app.ConfigGet (L"ChromiumDirectory", L".\\bin"));
			browser_exe.Format (L"%s\\chrome.exe", browser_directory);
			browser_args = app.ConfigGet (L"ChromiumCommandLine", L"--user-data-dir=..\\profile --no-default-browser-check");

			// get current version
			browser_version = _Browser_GetVersion (browser_exe);

			// start update checking
			_beginthreadex (nullptr, 0, &_Browser_CheckUpdate, nullptr, 0, nullptr);

			break;
		}

		case WM_CLOSE:
		{
			if (_r_msg (hwnd, MB_YESNO | MB_ICONQUESTION, APP_NAME, I18N (&app, IDS_QUESTION_BUSY, 0)) != IDYES)
			{
				return TRUE;
			}

			DestroyWindow (hwnd);

			break;
		}

		case WM_DESTROY:
		{
			if (!_Browser_Run ())
			{
				_r_msg (hwnd, MB_OK | MB_ICONSTOP, APP_NAME, I18N (&app, IDS_STATUS_ERROR, 0), GetLastError ());
			}

			PostQuitMessage (0);

			break;
		}

		case WM_QUERYENDSESSION:
		{
			SetWindowLongPtr (hwnd, DWLP_MSGRESULT, TRUE);
			return TRUE;
		}

		case WM_LBUTTONDOWN:
		{
			SendMessage (hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
			break;
		}

		case WM_ENTERSIZEMOVE:
		case WM_EXITSIZEMOVE:
		case WM_CAPTURECHANGED:
		{
			LONG_PTR exstyle = GetWindowLongPtr (hwnd, GWL_EXSTYLE);

			if ((exstyle & WS_EX_LAYERED) == 0) { SetWindowLongPtr (hwnd, GWL_EXSTYLE, exstyle | WS_EX_LAYERED); }

			SetLayeredWindowAttributes (hwnd, 0, (msg == WM_ENTERSIZEMOVE) ? 100 : 255, LWA_ALPHA);
			SetCursor (LoadCursor (nullptr, (msg == WM_ENTERSIZEMOVE) ? IDC_SIZEALL : IDC_ARROW));

			break;
		}

		case WM_NOTIFY:
		{
			switch (LPNMHDR (lparam)->code)
			{
				case NM_CLICK:
				case NM_RETURN:
				{
					ShellExecute (nullptr, nullptr, PNMLINK (lparam)->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
					break;
				}
			}

			break;
		}

		case WM_COMMAND:
		{
			switch (LOWORD (wparam))
			{
				case IDCANCEL: // process Esc key
				case IDM_EXIT:
				{
					SendMessage (hwnd, WM_CLOSE, 0, 0);
					break;
				}

				case IDM_WEBSITE:
				{
					ShellExecute (hwnd, nullptr, _APP_WEBSITE_URL, nullptr, nullptr, SW_SHOWDEFAULT);
					break;
				}

				case IDM_DONATE:
				{
					ShellExecute (hwnd, nullptr, _APP_DONATION_URL, nullptr, nullptr, SW_SHOWDEFAULT);
					break;
				}

				case IDM_ABOUT:
				{
					app.CreateAboutWindow ();
					break;
				}
			}

			break;
		}
	}

	return FALSE;
}

INT APIENTRY wWinMain (HINSTANCE, HINSTANCE, LPWSTR, INT)
{
	if (app.CreateMainWindow (&DlgProc))
	{
		MSG msg = {0};

		while (GetMessage (&msg, nullptr, 0, 0))
		{
			if (!IsDialogMessage (app.GetHWND (), &msg))
			{
				TranslateMessage (&msg);
				DispatchMessage (&msg);
			}
		}
	}

	return ERROR_SUCCESS;
}
