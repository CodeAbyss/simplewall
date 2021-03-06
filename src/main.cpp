// simplewall
// Copyright (c) 2016-2018 Henry++

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windns.h>
#include <mstcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <subauth.h>
#include <fwpmu.h>
#include <dbt.h>
#include <aclapi.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <sddl.h>
#include <ws2tcpip.h>
#include <wintrust.h>
#include <softpub.h>
#include <algorithm>
#include <userenv.h>

#include "main.hpp"
#include "rapp.hpp"
#include "routine.hpp"

#include "pugiconfig.hpp"
#include "..\..\pugixml\src\pugixml.hpp"

#include "resource.hpp"

CONST UINT WM_FINDMSGSTRING = RegisterWindowMessage (FINDMSGSTRING);

rapp app (APP_NAME, APP_NAME_SHORT, APP_VERSION, APP_COPYRIGHT);

std::unordered_map<size_t, ITEM_APP> apps;
std::unordered_map<size_t, time_t> apps_timer;
std::unordered_map<size_t, bool> apps_undelete;
std::unordered_map<size_t, time_t> notifications_last;

std::unordered_map<size_t, LPWSTR> cache_signatures;
std::unordered_map<size_t, LPWSTR> cache_versions;

std::unordered_map<rstring, bool, rstring::hash, rstring::is_equal> rules_config;

std::vector<ITEM_COLOR> colors;
std::vector<ITEM_PROTOCOL> protocols;
std::vector<ITEM_ADD> processes;
std::vector<ITEM_ADD> packages;
std::vector<ITEM_ADD> services;
std::vector<time_t> timers;

std::vector<PITEM_RULE> rules_blocklist;
std::vector<PITEM_RULE> rules_system;
std::vector<PITEM_RULE> rules_custom;

std::vector<PITEM_LOG> notifications;

STATIC_DATA config;

FWPM_SESSION session;

EXTERN_C const IID IID_IImageList;

_R_FASTLOCK lock_apply;
_R_FASTLOCK lock_access;
_R_FASTLOCK lock_writelog;
_R_FASTLOCK lock_notification;

PSLIST_HEADER log_stack = nullptr;

bool _wfp_initialize (bool is_full);
void _wfp_uninitialize (bool is_full);

bool _app_timer_apply (HWND hwnd, bool is_forceremove);

BOOL initializer_callback (HWND hwnd, DWORD msg, LPVOID, LPVOID);

UINT WINAPI ApplyThread (LPVOID lparam);
UINT WINAPI LogThread (LPVOID lparam);
LRESULT CALLBACK NotificationProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

bool _app_notifysettimeout (HWND hwnd, UINT_PTR id, bool is_create, UINT timeout);
bool _app_notifyrefresh ();

bool _wfp_logsubscribe ();
bool _wfp_logunsubscribe ();

void _app_logerror (LPCWSTR fn, DWORD result, LPCWSTR desc, bool is_nopopups = false)
{
	_r_dbg_write (APP_NAME_SHORT, APP_VERSION, fn, result, desc);

	if (!is_nopopups && app.ConfigGet (L"IsErrorNotificationsEnabled", true).AsBool ()) // check for timeout (sec.)
	{
		config.is_popuperrors = true;

		app.TrayPopup (UID, NIIF_USER | (app.ConfigGet (L"IsNotificationsSound", true).AsBool () ? 0 : NIIF_NOSOUND), APP_NAME, app.LocaleString (IDS_STATUS_ERROR, nullptr));
	}
}

void _mps_changeconfig (bool is_stop)
{
	DWORD result = 0;
	bool is_started = false;

	SC_HANDLE scm = OpenSCManager (nullptr, nullptr, SC_MANAGER_ALL_ACCESS);

	if (!scm)
	{
		_app_logerror (L"OpenSCManager", GetLastError (), nullptr);
	}
	else
	{
		LPCWSTR arr[] = {
			L"mpssvc",
			L"mpsdrv",
		};

		for (INT i = 0; i < _countof (arr); i++)
		{
			SC_HANDLE sc = OpenService (scm, arr[i], SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP);

			if (!sc)
			{
				result = GetLastError ();

				if (result != ERROR_ACCESS_DENIED)
					_app_logerror (L"OpenService", GetLastError (), arr[i]);
			}
			else
			{
				if (!is_started)
				{
					SERVICE_STATUS status;

					if (QueryServiceStatus (sc, &status))
						is_started = (status.dwCurrentState == SERVICE_RUNNING);
				}

				if (!ChangeServiceConfig (sc, SERVICE_NO_CHANGE, is_stop ? SERVICE_DISABLED : SERVICE_AUTO_START, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
					_app_logerror (L"ChangeServiceConfig", GetLastError (), arr[i]);

				CloseServiceHandle (sc);
			}
		}

		// start services
		if (is_stop)
		{
			_r_run (nullptr, L"netsh advfirewall set allprofiles state off", nullptr, SW_HIDE);
		}
		else
		{
			for (INT i = 0; i < _countof (arr); i++)
			{
				SC_HANDLE sc = OpenService (scm, arr[i], SERVICE_QUERY_STATUS | SERVICE_START);

				if (!sc)
				{
					_app_logerror (L"OpenService", GetLastError (), arr[i]);
				}
				else
				{
					DWORD dwBytesNeeded = 0;
					SERVICE_STATUS_PROCESS ssp = {0};

					if (!QueryServiceStatusEx (sc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof (ssp), &dwBytesNeeded))
					{
						_app_logerror (L"QueryServiceStatusEx", GetLastError (), arr[i]);
					}
					else
					{
						if (ssp.dwCurrentState != SERVICE_RUNNING)
						{
							if (!StartService (sc, 0, nullptr))
							{
								_app_logerror (L"StartService", GetLastError (), arr[i]);
							}
						}

						CloseServiceHandle (sc);
					}
				}
			}

			_r_sleep (250);

			_r_run (nullptr, L"netsh advfirewall set allprofiles state on", nullptr, SW_HIDE);
		}

		CloseServiceHandle (scm);
	}
}

void _app_listviewresize (HWND hwnd, UINT ctrl_id)
{
	if (!app.ConfigGet (L"AutoSizeColumns", true).AsBool ())
		return;

	RECT rect = {0};
	GetWindowRect (GetDlgItem (hwnd, ctrl_id), &rect);

	const INT width = (rect.right - rect.left) - GetSystemMetrics (SM_CXVSCROLL);

	const INT cx2 = max (app.GetDPI (110), min (app.GetDPI (190), _R_PERCENT_VAL (28, width)));
	const INT cx1 = width - cx2;

	_r_listview_setcolumn (hwnd, ctrl_id, 0, nullptr, cx1);
	_r_listview_setcolumn (hwnd, ctrl_id, 1, nullptr, cx2);
}

void _app_listviewsetimagelist (HWND hwnd, UINT ctrl_id)
{
	HIMAGELIST himg = nullptr;

	const bool is_large = app.ConfigGet (L"IsLargeIcons", false).AsBool () && ctrl_id == IDC_LISTVIEW;
	const bool is_iconshidden = app.ConfigGet (L"IsIconsHidden", false).AsBool ();

	if (SUCCEEDED (SHGetImageList (is_large ? SHIL_LARGE : SHIL_SMALL, IID_IImageList, (LPVOID*)&himg)))
	{
		SendDlgItemMessage (hwnd, ctrl_id, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)himg);
		SendDlgItemMessage (hwnd, ctrl_id, LVM_SETIMAGELIST, LVSIL_NORMAL, (LPARAM)himg);
	}

	if (ctrl_id != IDC_LISTVIEW)
		return;

	SendDlgItemMessage (hwnd, ctrl_id, LVM_SCROLL, 0, GetScrollPos (GetDlgItem (hwnd, ctrl_id), SB_VERT)); // scrollbar-hack!!!

	CheckMenuRadioItem (GetMenu (hwnd), IDM_ICONSSMALL, IDM_ICONSLARGE, (is_large ? IDM_ICONSLARGE : IDM_ICONSSMALL), MF_BYCOMMAND);
	CheckMenuItem (GetMenu (hwnd), IDM_ICONSISHIDDEN, MF_BYCOMMAND | (is_iconshidden ? MF_CHECKED : MF_UNCHECKED));
}

bool _app_listviewinitfont (PLOGFONT plf)
{
	if (!plf)
		return false;

	rstring buffer = app.ConfigGet (L"Font", UI_FONT_DEFAULT);

	if (buffer.IsEmpty ())
	{
		return false;
	}
	else
	{
		rstring::rvector vc = buffer.AsVector (L";");

		for (size_t i = 0; i < vc.size (); i++)
		{
			vc.at (i).Trim (L" \r\n");

			if (vc.at (i).IsEmpty ())
				continue;

			if (i == 0)
			{
				StringCchCopy (plf->lfFaceName, LF_FACESIZE, vc.at (i));
			}
			else if (i == 1)
			{
				plf->lfHeight = _r_dc_fontsizetoheight (vc.at (i).AsInt ());
			}
			else if (i == 2)
			{
				plf->lfWeight = vc.at (i).AsInt ();
			}
			else
			{
				break;
			}
		}
	}

	// fill missed font values
	{
		NONCLIENTMETRICS ncm = {0};
		ncm.cbSize = sizeof (ncm);

		if (SystemParametersInfo (SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0))
		{
			PLOGFONT pdeflf = &ncm.lfMessageFont;

			if (!plf->lfFaceName[0])
				StringCchCopy (plf->lfFaceName, LF_FACESIZE, pdeflf->lfFaceName);

			if (!plf->lfHeight)
				plf->lfHeight = pdeflf->lfHeight;

			if (!plf->lfWeight)
				plf->lfWeight = pdeflf->lfWeight;

			// set default values
			plf->lfCharSet = DEFAULT_CHARSET;
			plf->lfQuality = CLEARTYPE_QUALITY;
		}
	}

	return true;
}

void _app_listviewsetfont (HWND hwnd, UINT ctrl_id)
{
	LOGFONT lf = {0};

	if (!config.hfont)
	{
		if (_app_listviewinitfont (&lf))
		{
			const LONG prev_weight = lf.lfWeight;

			lf.lfWeight = FW_BOLD;
			config.hfont_bold = CreateFontIndirect (&lf);

			lf.lfWeight = prev_weight;
			config.hfont = CreateFontIndirect (&lf);

			if (config.hfont)
				SendDlgItemMessage (hwnd, ctrl_id, WM_SETFONT, (WPARAM)config.hfont, TRUE);

			return;
		}
		else
		{
			SendDlgItemMessage (hwnd, ctrl_id, WM_SETFONT, 0, TRUE);
		}
	}
	else
	{
		SendDlgItemMessage (hwnd, ctrl_id, WM_SETFONT, (WPARAM)config.hfont, TRUE);
	}

}

ITEM_APP* _app_getapplication (size_t hash)
{
	ITEM_APP *ptr_app = nullptr;

	if (hash && apps.find (hash) != apps.end ())
		ptr_app = &apps.at (hash);

	return ptr_app;
}

void ShowItem (HWND hwnd, UINT ctrl_id, size_t item, INT scroll_pos)
{
	if (item != LAST_VALUE)
	{
		ListView_SetItemState (GetDlgItem (hwnd, ctrl_id), -1, 0, LVIS_SELECTED); // deselect all
		ListView_SetItemState (GetDlgItem (hwnd, ctrl_id), item, LVIS_SELECTED, LVIS_SELECTED); // select item

		if (scroll_pos == -1)
			SendDlgItemMessage (hwnd, ctrl_id, LVM_ENSUREVISIBLE, item, TRUE); // ensure item visible
	}

	if (scroll_pos != -1)
		SendDlgItemMessage (hwnd, ctrl_id, LVM_SCROLL, 0, scroll_pos); // restore vscroll position
}

void _app_refreshstatus (HWND hwnd, bool first_part, bool second_part)
{
	if (first_part)
	{
		WCHAR buffer[128] = {0};
		StringCchPrintf (buffer, _countof (buffer), app.LocaleString (IDS_STATUS_TOTAL, nullptr), apps.size ());

		const size_t selection_count = SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETSELECTEDCOUNT, 0, 0);

		if (selection_count)
		{
			StringCchCat (buffer, _countof (buffer), L" / ");
			StringCchCat (buffer, _countof (buffer), _r_fmt (app.LocaleString (IDS_STATUS_SELECTED, nullptr), selection_count));
		}

		_r_status_settext (hwnd, IDC_STATUSBAR, 0, buffer);
	}

	if (second_part)
	{
		const size_t total_count = _r_listview_getitemcount (hwnd, IDC_LISTVIEW);
		size_t group1_count = 0;
		size_t group2_count = 0;

		for (auto const &p : apps)
		{
			if (p.second.is_enabled)
				group1_count += 1;
		}

		group2_count = (total_count - group1_count);

		switch (app.ConfigGet (L"Mode", ModeWhitelist).AsUint ())
		{
			case ModeWhitelist:
			{
				_r_listview_setgroup (hwnd, IDC_LISTVIEW, 0, app.LocaleString (IDS_GROUP_ALLOWED, _r_fmt (L" (%d)", group1_count)), 0, 0);
				_r_listview_setgroup (hwnd, IDC_LISTVIEW, 1, app.LocaleString (IDS_GROUP_BLOCKED, _r_fmt (L" (%d)", group2_count)), 0, 0);

				break;
			}

			case ModeBlacklist:
			{
				_r_listview_setgroup (hwnd, IDC_LISTVIEW, 0, app.LocaleString (IDS_GROUP_BLOCKED, _r_fmt (L" (%d)", group1_count)), 0, 0);
				_r_listview_setgroup (hwnd, IDC_LISTVIEW, 1, app.LocaleString (IDS_GROUP_ALLOWED, _r_fmt (L" (%d)", group2_count)), 0, 0);

				break;
			}
		}
	}
}

bool _app_item_get (std::vector<ITEM_ADD>* pvec, size_t hash, rstring* display_name, rstring* real_path, PSID* lpsid, PSECURITY_DESCRIPTOR* lpsd, rstring* /*description*/)
{
	for (size_t i = 0; i < pvec->size (); i++)
	{
		if (pvec->at (i).hash == hash)
		{
			if (display_name)
			{
				if (pvec->at (i).display_name[0])
					*display_name = pvec->at (i).display_name;

				else if (pvec->at (i).real_path[0])
					*display_name = pvec->at (i).real_path;

				else if (pvec->at (i).sid[0])
					*display_name = pvec->at (i).sid;
			}

			if (real_path)
			{
				if (pvec->at (i).real_path[0])
					*real_path = pvec->at (i).real_path;
			}

			if (lpsid)
				*lpsid = pvec->at (i).psid;

			if (lpsd)
				*lpsd = pvec->at (i).psd;

			//if (description)
			//	*description = pvec->at (i).pdesc;

			return true;
		}
	}

	return false;
}

LPCWSTR _app_getdisplayname (size_t hash, ITEM_APP const *ptr_app)
{
	if (hash == config.ntoskrnl_hash)
		return ptr_app->original_path;

	if (ptr_app->type == AppStore)
	{
		rstring display_name;
		_app_item_get (&packages, hash, &display_name, nullptr, nullptr, nullptr, nullptr);

		return display_name;
	}
	else if (ptr_app->type == AppService)
	{
		//rstring display_name;
		//_app_item_get (&services, hash, &display_name, nullptr, nullptr, nullptr, nullptr);

		//return display_name;
		return ptr_app->original_path;
	}
	else
	{
		if (app.ConfigGet (L"ShowFilenames", true).AsBool ())
			return _r_path_extractfile (ptr_app->real_path);

		else
			return ptr_app->real_path;
	}

	//const UINT display_mode = app.ConfigGet (L"DisplayMode", 0).AsUint ();

	//if (ptr_app->type == AppService || ptr_app->type == AppStore)
	//{
	//	rstring display_name;
	//	_app_item_get (&services, hash, &display_name, nullptr, nullptr, nullptr, nullptr);

	//	return display_name;
	//}

	//if (display_mode == 2) // description
	//{
	//	if (cache_description.find (hash) != cache_description.end ())
	//	{
	//		if (cache_description[hash])
	//			return cache_description[hash];
	//	}

	//	cache_description[hash] = nullptr;
	//}

	//if (display_mode == 1) // fullpath
	//{
	//	return ptr_app->real_path;
	//}
	//else /*(display_mode == 0) // filename*/
	//{
	//	return _r_path_extractfile (ptr_app->real_path);
	//}

	//return L"n/a";
}

bool _app_getinformation (size_t hash, LPCWSTR path, LPCWSTR* pinfo)
{
	if (!pinfo)
		return false;

	if (cache_versions.find (hash) != cache_versions.end ())
	{
		*pinfo = cache_versions[hash];

		return (cache_versions[hash] != nullptr);
	}

	bool result = false;
	rstring buffer;

	cache_versions[hash] = nullptr;

	HINSTANCE hlib = LoadLibraryEx (path, nullptr, DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_DATAFILE);

	if (hlib)
	{
		HRSRC hres = FindResource (hlib, MAKEINTRESOURCE (VS_VERSION_INFO), RT_VERSION);

		if (hres)
		{
			HGLOBAL hg = LoadResource (hlib, hres);

			if (hg)
			{
				LPVOID versionInfo = LockResource (hg);

				if (versionInfo)
				{
					UINT vLen = 0, langD = 0;
					LPVOID retbuf = nullptr;

					WCHAR author_entry[86] = {0};
					WCHAR description_entry[86] = {0};
					WCHAR version_entry[86] = {0};

					if (VerQueryValue (versionInfo, L"\\VarFileInfo\\Translation", &retbuf, &vLen) && vLen == 4)
					{
						memcpy (&langD, retbuf, vLen);
						StringCchPrintf (author_entry, _countof (author_entry), L"\\StringFileInfo\\%02X%02X%02X%02X\\CompanyName", (langD & 0xff00) >> 8, langD & 0xff, (langD & 0xff000000) >> 24, (langD & 0xff0000) >> 16);
						StringCchPrintf (description_entry, _countof (description_entry), L"\\StringFileInfo\\%02X%02X%02X%02X\\FileDescription", (langD & 0xff00) >> 8, langD & 0xff, (langD & 0xff000000) >> 24, (langD & 0xff0000) >> 16);
						StringCchPrintf (version_entry, _countof (version_entry), L"\\StringFileInfo\\%02X%02X%02X%02X\\FileVersion", (langD & 0xff00) >> 8, langD & 0xff, (langD & 0xff000000) >> 24, (langD & 0xff0000) >> 16);
					}
					else
					{
						StringCchPrintf (author_entry, _countof (author_entry), L"\\StringFileInfo\\%04X04B0\\CompanyName", GetUserDefaultLangID ());
						StringCchPrintf (description_entry, _countof (description_entry), L"\\StringFileInfo\\%04X04B0\\FileDescription", GetUserDefaultLangID ());
						StringCchPrintf (version_entry, _countof (version_entry), L"\\StringFileInfo\\%04X04B0\\FileVersion", GetUserDefaultLangID ());
					}

					if (VerQueryValue (versionInfo, description_entry, &retbuf, &vLen))
					{
						buffer.Append (TAB_SPACE);
						buffer.Append (LPCWSTR (retbuf));

						{
							const size_t length = wcslen (LPCWSTR (retbuf)) + 1;
							LPWSTR ppointer = new WCHAR[length];

							if (ppointer)
							{
								StringCchCopy (ppointer, length, buffer);
								cache_versions[hash] = ppointer;
							}
						}

						UINT length = 0;
						VS_FIXEDFILEINFO* verInfo = nullptr;

						if (VerQueryValue (versionInfo, L"\\", (LPVOID*)(&verInfo), &length))
						{
							buffer.Append (_r_fmt (L" %d.%d", HIWORD (verInfo->dwFileVersionMS), LOWORD (verInfo->dwFileVersionMS)));

							if (HIWORD (verInfo->dwFileVersionLS) || LOWORD (verInfo->dwFileVersionLS))
							{
								buffer.Append (_r_fmt (L".%d", HIWORD (verInfo->dwFileVersionLS)));

								if (LOWORD (verInfo->dwFileVersionLS))
									buffer.Append (_r_fmt (L".%d", LOWORD (verInfo->dwFileVersionLS)));
							}
						}

						buffer.Append (L"\r\n");
					}

					if (VerQueryValue (versionInfo, author_entry, &retbuf, &vLen))
					{
						buffer.Append (TAB_SPACE);
						buffer.Append (static_cast<LPCWSTR>(retbuf));
						buffer.Append (L"\r\n");
					}

					buffer.Trim (L"\r\n ");

					// get signature information
					{
						const size_t length = buffer.GetLength () + 1;
						LPWSTR ppointer = new WCHAR[length];

						if (ppointer)
							StringCchCopy (ppointer, length, buffer);

						*pinfo = ppointer;
						cache_versions[hash] = ppointer;
					}

					result = true;
				}
			}

			UnlockResource (hg);
			FreeResource (hg);
		}

		FreeLibrary (hlib);
	}

	return result;
}

bool _app_getfileicon (LPCWSTR path, bool is_small, size_t* picon_id, HICON* picon)
{
	if (!picon_id && !picon)
		return false;

	bool result = false;

	SHFILEINFO shfi = {0};
	DWORD flags = 0;

	if (picon_id)
		flags |= SHGFI_SYSICONINDEX;

	if (picon)
		flags |= SHGFI_ICON;

	if (is_small)
		flags |= SHGFI_SMALLICON;

	if (SUCCEEDED (CoInitialize (nullptr)))
	{
		if (SHGetFileInfo (path, 0, &shfi, sizeof (shfi), flags))
		{
			if (picon_id)
				*picon_id = shfi.iIcon;

			if (picon && shfi.hIcon)
			{
				*picon = CopyIcon (shfi.hIcon);
				DestroyIcon (shfi.hIcon);
			}

			result = true;
		}

		CoUninitialize ();
	}

	return result;
}

void _app_getappicon (ITEM_APP const *ptr_app, bool is_small, size_t* picon_id, HICON* picon)
{
	const bool is_iconshidden = app.ConfigGet (L"IsIconsHidden", false).AsBool ();

	if (ptr_app->type == AppRegular)
	{
		if (is_iconshidden || !_app_getfileicon (ptr_app->real_path, is_small, picon_id, picon))
		{
			if (picon_id)
				*picon_id = config.icon_id;

			if (picon)
				*picon = CopyIcon (is_small ? config.hicon_small : config.hicon_large);
		}
	}
	else if (ptr_app->type == AppStore)
	{
		if (picon_id)
			*picon_id = config.icon_package_id;

		if (picon)
			*picon = CopyIcon (is_small ? config.hicon_package : config.hicon_large); // small-only!
	}
	else if (ptr_app->type == AppService)
	{
		if (picon_id)
			*picon_id = config.icon_service_id;

		if (picon)
			*picon = CopyIcon (is_small ? config.hicon_service_small : config.hicon_large); // small-only!
	}
	else
	{
		if (picon_id)
			*picon_id = config.icon_id;

		if (picon)
			*picon = CopyIcon (is_small ? config.hicon_small : config.hicon_large);
	}
}

size_t _app_getposition (HWND hwnd, size_t hash)
{
	for (size_t i = 0; i < _r_listview_getitemcount (hwnd, IDC_LISTVIEW); i++)
	{
		if ((size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, i) == hash)
			return i;
	}

	return LAST_VALUE;
}

rstring _app_getshortcutpath (HWND hwnd, LPCWSTR path)
{
	rstring result;

	IShellLink* psl = nullptr;

	if (SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED)))
	{
		if (SUCCEEDED (CoInitializeSecurity (nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, 0, nullptr)))
		{
			if (SUCCEEDED (CoCreateInstance (CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLink, (LPVOID*)&psl)))
			{
				IPersistFile* ppf = nullptr;

				if (SUCCEEDED (psl->QueryInterface (IID_IPersistFile, (LPVOID*)&ppf)))
				{
					if (SUCCEEDED (ppf->Load (path, STGM_READ)))
					{
						if (SUCCEEDED (psl->Resolve (hwnd, 0)))
						{
							WIN32_FIND_DATA wfd = {0};
							WCHAR buffer[MAX_PATH] = {0};

							if (SUCCEEDED (psl->GetPath (buffer, _countof (buffer), (LPWIN32_FIND_DATA)&wfd, SLGP_RAWPATH)))
								result = buffer;
						}
					}

					ppf->Release ();
				}

				psl->Release ();
			}
		}

		CoUninitialize ();
	}

	return result;
}

bool _app_verifysignature (size_t hash, LPCWSTR path, LPCWSTR* psigner)
{
	if (!app.ConfigGet (L"IsCerificatesEnabled", false).AsBool ())
		return false;

	if (!psigner)
		return false;

	if (cache_signatures.find (hash) != cache_signatures.end ())
	{
		*psigner = cache_signatures[hash];

		return (cache_signatures[hash] != nullptr);
	}

	bool result = false;

	cache_signatures[hash] = nullptr;

	const HANDLE hfile = CreateFile (path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);

	if (hfile == INVALID_HANDLE_VALUE)
	{
		_app_logerror (L"CreateFile", result, path, true);
		return false;
	}
	else
	{
		static GUID WinTrustActionGenericVerifyV2 = WINTRUST_ACTION_GENERIC_VERIFY_V2;

		WINTRUST_FILE_INFO fileInfo = {0};

		fileInfo.cbStruct = sizeof (fileInfo);
		fileInfo.pcwszFilePath = path;
		fileInfo.hFile = hfile;

		WINTRUST_DATA trustData = {0};

		trustData.cbStruct = sizeof (trustData);
		trustData.dwUIChoice = WTD_UI_NONE;
		trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
		trustData.dwProvFlags = WTD_SAFER_FLAG | WTD_CACHE_ONLY_URL_RETRIEVAL;
		trustData.dwUnionChoice = WTD_CHOICE_FILE;
		trustData.pFile = &fileInfo;

		trustData.dwStateAction = WTD_STATEACTION_VERIFY;
		const LONG status = WinVerifyTrust ((HWND)INVALID_HANDLE_VALUE, &WinTrustActionGenericVerifyV2, &trustData);

		if (status == S_OK)
		{
			PCRYPT_PROVIDER_DATA provData = WTHelperProvDataFromStateData (trustData.hWVTStateData);

			if (provData)
			{
				PCRYPT_PROVIDER_SGNR psProvSigner = WTHelperGetProvSignerFromChain (provData, 0, FALSE, 0);

				if (psProvSigner)
				{
					CRYPT_PROVIDER_CERT *psProvCert = WTHelperGetProvCertFromChain (psProvSigner, 0);

					if (psProvCert)
					{
						const DWORD num_chars = CertGetNameString (psProvCert->pCert, CERT_NAME_ATTR_TYPE, 0, szOID_COMMON_NAME, nullptr, 0) + 1;

						if (num_chars > 1)
						{
							LPWSTR ppointer = new WCHAR[num_chars];

							if (ppointer)
								CertGetNameString (psProvCert->pCert, CERT_NAME_ATTR_TYPE, 0, szOID_COMMON_NAME, ppointer, num_chars);

							*psigner = ppointer;
							cache_signatures[hash] = ppointer;
						}
					}
				}
			}

			result = true;
		}

		trustData.dwStateAction = WTD_STATEACTION_CLOSE;
		WinVerifyTrust ((HWND)INVALID_HANDLE_VALUE, &WinTrustActionGenericVerifyV2, &trustData);

		CloseHandle (hfile);
	}

	return result;
}

HBITMAP _app_ico2bmp (HICON hicon)
{
	const INT icon_size = GetSystemMetrics (SM_CXSMICON);

	RECT iconRectangle = {0};

	iconRectangle.right = icon_size;
	iconRectangle.bottom = icon_size;

	HBITMAP hbitmap = nullptr;
	HDC screenHdc = GetDC (nullptr);
	HDC hdc = CreateCompatibleDC (screenHdc);

	if (hdc)
	{
		BITMAPINFO bitmapInfo = {0};
		bitmapInfo.bmiHeader.biSize = sizeof (bitmapInfo);
		bitmapInfo.bmiHeader.biPlanes = 1;
		bitmapInfo.bmiHeader.biCompression = BI_RGB;

		bitmapInfo.bmiHeader.biWidth = icon_size;
		bitmapInfo.bmiHeader.biHeight = icon_size;
		bitmapInfo.bmiHeader.biBitCount = 32;

		hbitmap = CreateDIBSection (hdc, &bitmapInfo, DIB_RGB_COLORS, nullptr, nullptr, 0);

		if (hbitmap)
		{
			ReleaseDC (nullptr, screenHdc);
			HBITMAP oldBitmap = (HBITMAP)SelectObject (hdc, hbitmap);

			BLENDFUNCTION blendFunction = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
			blendFunction.BlendOp = AC_SRC_OVER;
			blendFunction.AlphaFormat = AC_SRC_ALPHA;
			blendFunction.SourceConstantAlpha = 255;

			BP_PAINTPARAMS paintParams = {0};
			paintParams.cbSize = sizeof (paintParams);
			paintParams.dwFlags = BPPF_ERASE;
			paintParams.pBlendFunction = &blendFunction;

			HDC bufferHdc = nullptr;
			HPAINTBUFFER paintBuffer = BeginBufferedPaint (hdc, &iconRectangle, BPBF_DIB, &paintParams, &bufferHdc);

			if (paintBuffer)
			{
				DrawIconEx (bufferHdc, 0, 0, hicon, icon_size, icon_size, 0, nullptr, DI_NORMAL);
				EndBufferedPaint (paintBuffer, TRUE);
			}
			else
			{
				_r_dc_fillrect (hdc, &iconRectangle, GetSysColor (COLOR_MENU));
				DrawIconEx (hdc, 0, 0, hicon, icon_size, icon_size, 0, nullptr, DI_NORMAL);
				SelectObject (hdc, oldBitmap);
			}

			SelectObject (hdc, oldBitmap);
		}

		DeleteDC (hdc);
	}

	return hbitmap;
}

void _app_applycasestyle (LPWSTR buffer, size_t length)
{
	if (length > 1)
	{
		for (size_t i = 1; i < length; i++)
			buffer[i] = _r_str_lower (buffer[i]);
	}
}

void _app_generate_packages ()
{
	for (size_t i = 0; i < packages.size (); i++)
	{
		if (packages.at (i).psid)
		{
			LocalFree (packages.at (i).psid);
			packages.at (i).psid = nullptr;
		}
	}

	packages.clear ();

	HKEY hkey = nullptr;
	HKEY hsubkey = nullptr;
	LONG result = RegOpenKeyEx (HKEY_CLASSES_ROOT, L"Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\Mappings", 0, KEY_READ, &hkey);

	if (result != ERROR_SUCCESS)
	{
		_app_logerror (L"RegOpenKeyEx", result, nullptr, true);
	}
	else
	{
		DWORD index = 0;

		while (true)
		{
			WCHAR package_sid[MAX_PATH] = {0};
			DWORD size = _countof (package_sid);

			if (RegEnumKeyEx (hkey, index++, package_sid, &size, 0, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
				break;

			result = RegOpenKeyEx (hkey, package_sid, 0, KEY_READ, &hsubkey);

			if (result != ERROR_SUCCESS)
			{
				if (result != ERROR_FILE_NOT_FOUND)
					_app_logerror (L"RegOpenKeyEx", result, package_sid, true);
			}
			else
			{
				const size_t hash = _r_str_hash (package_sid);

				ITEM_ADD item = {0};

				item.hash = hash;
				StringCchCopy (item.sid, _countof (item.sid), package_sid);

				size = _countof (item.display_name) * sizeof (WCHAR);
				result = RegQueryValueEx (hsubkey, L"DisplayName", nullptr, nullptr, (LPBYTE)item.display_name, &size);

				if (result == ERROR_SUCCESS)
				{
					if (item.display_name[0] == L'@')
					{
						if (!SUCCEEDED (SHLoadIndirectString (rstring (item.display_name), item.display_name, _countof (item.display_name), nullptr)))
							item.display_name[0] = 0;
					}
				}

				if (!item.display_name[0])
				{
					size = _countof (item.display_name) * sizeof (WCHAR);
					RegQueryValueEx (hsubkey, L"Moniker", nullptr, nullptr, (LPBYTE)item.display_name, &size);
				}

				if (!item.display_name[0])
					StringCchCopy (item.display_name, _countof (item.display_name), package_sid);

				ConvertStringSidToSid (package_sid, &item.psid);

				packages.push_back (item);

				RegCloseKey (hsubkey);
			}
		}

		std::sort (packages.begin (), packages.end (),
			[](const ITEM_ADD& a, const ITEM_ADD& b)->bool {
			return StrCmpLogicalW (a.display_name, b.display_name) == -1;
		});

		RegCloseKey (hkey);
	}
}

void _app_generate_services ()
{
	for (size_t i = 0; i < services.size (); i++)
	{
		if (services.at (i).psd)
			LocalFree (services.at (i).psd);
	}

	services.clear ();

	SC_HANDLE hsvcmgr = OpenSCManager (nullptr, nullptr, SC_MANAGER_ALL_ACCESS);

	if (hsvcmgr)
	{
		ENUM_SERVICE_STATUS service;

		DWORD dwBytesNeeded = 0;
		DWORD dwServicesReturned = 0;
		DWORD dwResumedHandle = 0;
		DWORD dwServiceType = SERVICE_WIN32;
		const DWORD dwServiceState = SERVICE_STATE_ALL;

		// win10 services
		if (_r_sys_validversion (10, 0))
			dwServiceType |= SERVICE_INTERACTIVE_PROCESS | SERVICE_USER_SERVICE | SERVICE_USERSERVICE_INSTANCE;

		if (!EnumServicesStatus (hsvcmgr, dwServiceType, dwServiceState, &service, sizeof (ENUM_SERVICE_STATUS), &dwBytesNeeded, &dwServicesReturned, &dwResumedHandle))
		{
			if (GetLastError () == ERROR_MORE_DATA)
			{
				// Set the buffer
				DWORD dwBytes = sizeof (ENUM_SERVICE_STATUS) + dwBytesNeeded;
				LPENUM_SERVICE_STATUS pServices = new ENUM_SERVICE_STATUS[dwBytes];

				// Now query again for services
				if (EnumServicesStatus (hsvcmgr, dwServiceType, dwServiceState, (LPENUM_SERVICE_STATUS)pServices, dwBytes, &dwBytesNeeded, &dwServicesReturned, &dwResumedHandle))
				{
					// now traverse each service to get information
					for (DWORD i = 0; i < dwServicesReturned; i++)
					{
						//						LPCWSTR display_name = (pServices + i)->lpDisplayName;
						LPCWSTR service_name = (pServices + i)->lpServiceName;

						WCHAR buffer[MAX_PATH] = {0};
						WCHAR real_path[MAX_PATH] = {0};
						LPWSTR sidstring = nullptr;

						// get binary path
						SC_HANDLE hsvc = OpenService (hsvcmgr, service_name, SERVICE_QUERY_CONFIG);

						if (hsvc)
						{
							LPQUERY_SERVICE_CONFIG lpqsc = {0};
							DWORD bytes_needed = 0;

							if (!QueryServiceConfig (hsvc, nullptr, 0, &bytes_needed))
							{
								lpqsc = new QUERY_SERVICE_CONFIG[bytes_needed];

								if (QueryServiceConfig (hsvc, lpqsc, bytes_needed, &bytes_needed))
								{
									if (lpqsc->dwStartType == SERVICE_DISABLED)
										continue;

									// query path
									StringCchCopy (real_path, _countof (real_path), lpqsc->lpBinaryPathName);
									PathRemoveArgs (real_path);
									PathUnquoteSpaces (real_path);

									_app_applycasestyle (real_path, wcslen (real_path)); // apply case-style

									//// query description
									//LPSERVICE_DESCRIPTION lpsd = nullptr;

									//QueryServiceConfig2 (hsvc, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &bytes_needed);

									//if (GetLastError () == ERROR_INSUFFICIENT_BUFFER)
									//{
									//	lpsd = new SERVICE_DESCRIPTION[bytes_needed];

									//	if (lpsd)
									//	{
									//		if (QueryServiceConfig2 (hsvc, SERVICE_CONFIG_DESCRIPTION, (LPBYTE)lpsd, bytes_needed, &bytes_needed))
									//		{
									//			if (lpsd->lpDescription)
									//			{
									//				const size_t len = wcslen (lpsd->lpDescription) + 1;
									//				ptrdesc = new WCHAR[len];
									//				StringCchCopy (ptrdesc, len, lpsd->lpDescription);
									//			}
									//		}

									//		delete[] lpsd;
									//	}
									//}
								}
								else
								{
									continue;
								}
							}

							CloseServiceHandle (hsvc);
						}

						UNICODE_STRING serviceNameUs = {0};

						serviceNameUs.Buffer = buffer;
						serviceNameUs.Length = (USHORT)(wcslen (service_name) * sizeof (WCHAR));
						serviceNameUs.MaximumLength = serviceNameUs.Length;

						StringCchCopy (buffer, _countof (buffer), service_name);

						PSID serviceSid = nullptr;
						ULONG serviceSidLength = 0;

						// get service security identifier
						if (RtlCreateServiceSid (&serviceNameUs, serviceSid, &serviceSidLength) == 0xC0000023 /*STATUS_BUFFER_TOO_SMALL*/)
						{
							serviceSid = new BYTE[serviceSidLength];

							if (NT_SUCCESS (RtlCreateServiceSid (&serviceNameUs, serviceSid, &serviceSidLength)))
							{
								ConvertSidToStringSid (serviceSid, &sidstring);
							}
							else
							{
								delete[] serviceSid;
								serviceSid = nullptr;
							}
						}

						if (serviceSid && sidstring)
						{
							ITEM_ADD item = {0};

							StringCchCopy (item.display_name, _countof (item.display_name), service_name/*display_name*/);
							StringCchCopy (item.service_name, _countof (item.service_name), service_name);
							StringCchCopy (item.real_path, _countof (item.real_path), real_path);
							StringCchCopy (item.sid, _countof (item.sid), sidstring);
							item.hash = _r_str_hash (item.service_name);

							if (!ConvertStringSecurityDescriptorToSecurityDescriptor (_r_fmt (SERVICE_SECURITY_DESCRIPTOR, sidstring).ToUpper (), SDDL_REVISION_1, &item.psd, nullptr))
								_app_logerror (L"ConvertStringSecurityDescriptorToSecurityDescriptor", GetLastError (), service_name);

							else
								services.push_back (item);
						}

						if (sidstring)
							LocalFree (sidstring);

						if (serviceSid)
							delete[] serviceSid;
					}

					std::sort (services.begin (), services.end (),
						[](const ITEM_ADD& a, const ITEM_ADD& b)->bool {
						return StrCmpLogicalW (a.display_name, b.display_name) == -1;
					});

					delete[] pServices;
					pServices = nullptr;
				}
			}
		}

		CloseServiceHandle (hsvcmgr);
	}
}

size_t _app_addapplication (HWND hwnd, rstring path, time_t timestamp, bool is_silent, bool is_enabled, bool is_fromdb)
{
	if (path.IsEmpty ())
		return 0;

	// if file is shortcut - get location
	if (!is_fromdb)
	{
		if (_wcsnicmp (PathFindExtension (path), L".lnk", 4) == 0)
			path = _app_getshortcutpath (hwnd, path);
	}

	const size_t hash = path.Hash ();

	if (apps.find (hash) != apps.end ())
		return 0; // already exists

	ITEM_APP *ptr_app = &apps[hash]; // application pointer

	const bool is_ntoskrnl = (hash == config.ntoskrnl_hash);

	rstring real_path;

	if (_wcsnicmp (path, L"\\device\\", 8) == 0) // device path
	{
		real_path = path;

		ptr_app->type = AppDevice;
		ptr_app->icon_id = config.icon_id;
	}
	else if (_wcsnicmp (path, L"S-1-", 4) == 0) // windows store (win8+)
	{
		ptr_app->type = AppStore;
		ptr_app->icon_id = config.icon_package_id;

		_app_item_get (&packages, hash, nullptr, &real_path, &ptr_app->psid, nullptr, nullptr);
	}
	else if (PathIsNetworkPath (path)) // network path
	{
		real_path = path;

		ptr_app->type = AppNetwork;
		ptr_app->icon_id = config.icon_id;
	}
	else
	{
		real_path = path;

		if (!is_ntoskrnl && real_path.Find (L'\\') == rstring::npos)
		{
			if (_app_item_get (&services, hash, nullptr, &real_path, nullptr, &ptr_app->psd, nullptr))
			{
				ptr_app->type = AppService;
				ptr_app->icon_id = config.icon_service_id;
			}
			else
			{
				ptr_app->type = AppPico;
				ptr_app->icon_id = config.icon_id;
			}
		}
		else
		{
			ptr_app->type = AppRegular;

			if (is_ntoskrnl) // "system" process
			{
				//display_name = path;
				real_path = _r_path_expand (PATH_NTOSKRNL);
			}

			const DWORD dwAttr = GetFileAttributes (real_path);
			ptr_app->is_system = is_ntoskrnl || ((dwAttr != INVALID_FILE_ATTRIBUTES && (dwAttr & FILE_ATTRIBUTE_SYSTEM) != 0)) || (_wcsnicmp (real_path, config.windows_dir, config.wd_length) == 0);
			ptr_app->is_temp = ((dwAttr != INVALID_FILE_ATTRIBUTES && (dwAttr & FILE_ATTRIBUTE_TEMPORARY) != 0)) || (_wcsnicmp (real_path, config.tmp1_dir, config.tmp1_length) == 0);

			ptr_app->is_signed = _app_verifysignature (hash, real_path, &ptr_app->signer);
		}
	}

	_app_applycasestyle (real_path.GetBuffer (), real_path.GetLength ()); // apply case-style

	StringCchCopy (ptr_app->original_path, _countof (ptr_app->original_path), path);
	StringCchCopy (ptr_app->real_path, _countof (ptr_app->real_path), real_path);
	StringCchCopy (ptr_app->display_name, _countof (ptr_app->display_name), _app_getdisplayname (hash, ptr_app));

	ptr_app->is_enabled = is_enabled;
	ptr_app->is_silent = is_silent;

	ptr_app->timestamp = timestamp ? timestamp : _r_unixtime_now ();

	_app_getappicon (ptr_app, false, &ptr_app->icon_id, nullptr);

	const size_t item = _r_listview_getitemcount (hwnd, IDC_LISTVIEW);

	config.is_nocheckboxnotify = true;

	_r_listview_additem (hwnd, IDC_LISTVIEW, item, 0, ptr_app->display_name, ptr_app->icon_id, ptr_app->is_enabled ? 0 : 1, hash);
	_r_listview_setitem (hwnd, IDC_LISTVIEW, item, 1, _r_fmt_date (ptr_app->timestamp, FDTF_SHORTDATE | FDTF_SHORTTIME));

	_r_listview_setitemcheck (hwnd, IDC_LISTVIEW, item, is_enabled);

	config.is_nocheckboxnotify = false;

	return hash;
}

bool _app_apphaverule (size_t hash)
{
	if (!hash)
		return false;

	for (size_t i = 0; i < rules_custom.size (); i++)
	{
		PITEM_RULE const ptr_rule = rules_custom.at (i);

		if (ptr_rule)
		{
			if (ptr_rule->is_enabled && ptr_rule->apps.find (hash) != ptr_rule->apps.end ())
				return true;
		}
	}

	return false;
}

bool _app_resolveaddress (ADDRESS_FAMILY af, LPVOID paddr, LPWSTR buffer, DWORD length)
{
	SOCKADDR_IN ipv4Address = {0};
	SOCKADDR_IN6 ipv6Address = {0};
	PSOCKADDR psock = {0};
	socklen_t size = 0;

	if (af == AF_INET)
	{
		ipv4Address.sin_family = af;
		ipv4Address.sin_addr = *PIN_ADDR (paddr);
		psock = (PSOCKADDR)&ipv4Address;
		size = sizeof (ipv4Address);
	}
	else if (af == AF_INET6)
	{
		ipv6Address.sin6_family = af;
		ipv6Address.sin6_addr = *PIN6_ADDR (paddr);

		psock = (PSOCKADDR)&ipv6Address;
		size = sizeof (ipv6Address);
	}
	else
	{
		return false;
	}

	if (GetNameInfoW (psock, size, buffer, length, nullptr, 0, NI_NAMEREQD) != ERROR_SUCCESS)
		return false;

	return true;
}

rstring _app_rulesexpand (PITEM_RULE const ptr_rule)
{
	rstring result;

	if (ptr_rule)
	{
		for (auto const &p : ptr_rule->apps)
		{
			ITEM_APP const *ptr_app = _app_getapplication (p.first);

			if (ptr_app)
			{
				if (ptr_app->type == AppStore || ptr_app->type == AppService)
					result.Append (ptr_app->display_name);

				else
					result.Append (ptr_app->original_path);

				result.Append (L"\r\n" TAB_SPACE);
			}
		}

		result.Trim (L"\r\n" TAB_SPACE);
	}

	return result;
}

void _app_freenotify (size_t idx, size_t hash)
{
	const size_t count = notifications.size ();

	if (!count)
		return;

	std::vector<size_t> idx_array;

	bool is_idxadded = false;

	if (hash)
	{
		for (size_t i = 0; i < notifications.size (); i++)
		{
			PITEM_LOG const ptr_log = notifications.at (i);

			if (!ptr_log || (ptr_log && ptr_log->hash == hash))
			{
				idx_array.push_back (i);

				if (!is_idxadded && idx != LAST_VALUE && idx == i)
					is_idxadded = true;
			}
		}
	}

	if (idx != LAST_VALUE && !is_idxadded)
		idx_array.push_back (idx);

	if (idx_array.empty ())
		return;

	for (size_t i = (idx_array.size () - 1); i != LAST_VALUE; i--)
	{
		const size_t vc_idx = idx_array.at (i);

		PITEM_LOG ptr_log = notifications.at (vc_idx);

		if (ptr_log)
		{
			delete ptr_log;
			ptr_log = nullptr;
		}

		notifications.erase (notifications.begin () + vc_idx);
	}
}

bool _app_freeapplication (size_t hash)
{
	bool is_enabled = false;

	ITEM_APP* ptr_app = _app_getapplication (hash);

	if (ptr_app)
		is_enabled = ptr_app->is_enabled;

	if (hash)
	{
		if (cache_signatures.find (hash) != cache_signatures.end ())
		{
			delete[] cache_signatures[hash];
			cache_signatures.erase (hash);
		}

		if (cache_versions.find (hash) != cache_versions.end ())
		{
			delete[] cache_versions[hash];
			cache_versions.erase (hash);
		}

		for (size_t i = 0; i < rules_custom.size (); i++)
		{
			PITEM_RULE ptr_rule = rules_custom.at (i);

			if (ptr_rule)
			{
				if (ptr_rule->apps.find (hash) != ptr_rule->apps.end ())
				{
					ptr_rule->apps.erase (hash);

					if (ptr_rule->is_enabled && !is_enabled)
						is_enabled = true;

					if (ptr_rule->is_enabled && ptr_rule->apps.empty ())
						ptr_rule->is_enabled = false;
				}
			}
		}

		_r_fastlock_acquireexclusive (&lock_notification);
		_app_freenotify (LAST_VALUE, hash);
		_r_fastlock_releaseexclusive (&lock_notification);

		_app_notifyrefresh ();

		apps.erase (hash);
	}

	return is_enabled;
}

void _app_freerule (PITEM_RULE* ptr)
{
	if (ptr && *ptr)
	{
		PITEM_RULE ptr_rule = *ptr;

		if (ptr_rule)
		{
			if (ptr_rule->pname)
			{
				delete[] (ptr_rule->pname);
				ptr_rule->pname = nullptr;
			}

			if (ptr_rule->prule)
			{
				delete[] (ptr_rule->prule);
				ptr_rule->prule = nullptr;
			}

			delete ptr_rule;

			ptr_rule = nullptr;
			*ptr = nullptr;
		}
	}
}

DWORD_PTR _app_getcolorvalue (size_t hash, bool is_brush)
{
	size_t idx = LAST_VALUE;

	for (size_t i = 0; i < colors.size (); i++)
	{
		if (colors.at (i).hash == hash)
		{
			idx = i;
			break;
		}
	}

	if (idx != LAST_VALUE)
	{
		if (is_brush)
			return (DWORD_PTR)colors.at (idx).hbrush;

		else
			return colors.at (idx).clr;
	}

	return 0;
}

bool _app_isexists (PITEM_APP ptr_app)
{
	if (ptr_app->is_enabled && ptr_app->is_haveerrors)
		return false;

	if (ptr_app->type == AppRegular)
		return _r_fs_exists (ptr_app->real_path);

	else if (ptr_app->type == AppStore)
		return _app_item_get (&packages, _r_str_hash (ptr_app->original_path), nullptr, nullptr, nullptr, nullptr, nullptr);

	else if (ptr_app->type == AppService)
		return _app_item_get (&services, _r_str_hash (ptr_app->original_path), nullptr, nullptr, nullptr, nullptr, nullptr);

	return true;
}

bool _app_istimeractive (size_t hash)
{
	return apps_timer.find (hash) != apps_timer.end () && apps_timer[hash] > _r_unixtime_now ();
}

bool _app_istimersactive ()
{
	for (auto const &p : apps_timer)
	{
		if (_app_istimeractive (p.first))
			return true;
	}

	return false;
}

DWORD_PTR _app_getcolor (size_t hash, bool is_brush, HDC hdc)
{
	_r_fastlock_acquireshared (&lock_access);

	rstring color_value;
	PITEM_APP const ptr_app = _app_getapplication (hash);

	if (ptr_app)
	{
		if (hdc && hash == config.myhash)
			SelectObject (hdc, config.hfont_bold);

		if (app.ConfigGet (L"IsHighlightInvalid", true).AsBool () && !_app_isexists (ptr_app))
			color_value = L"ColorInvalid";

		else if (app.ConfigGet (L"IsHighlightTimer", true).AsBool () && _app_istimeractive (hash))
			color_value = L"ColorTimer";

		else if (app.ConfigGet (L"IsHighlightSpecial", true).AsBool () && _app_apphaverule (hash))
			color_value = L"ColorSpecial";

		else if (ptr_app->is_silent && app.ConfigGet (L"IsHighlightSilent", true).AsBool ())
			color_value = L"ColorSilent";

		else if (ptr_app->is_signed && app.ConfigGet (L"IsHighlightSigned", true).AsBool () && app.ConfigGet (L"IsCerificatesEnabled", false).AsBool ())
			color_value = L"ColorSigned";

		else if ((ptr_app->type == AppService) && app.ConfigGet (L"IsHighlightService", true).AsBool ())
			color_value = L"ColorService";

		else if ((ptr_app->type == AppStore) && app.ConfigGet (L"IsHighlightPackage", true).AsBool ())
			color_value = L"ColorPackage";

		else if ((ptr_app->type == AppPico) && app.ConfigGet (L"IsHighlightPico", true).AsBool ())
			color_value = L"ColorPico";

		else if (ptr_app->is_system && app.ConfigGet (L"IsHighlightSystem", true).AsBool ())
			color_value = L"ColorSystem";
	}

	_r_fastlock_releaseshared (&lock_access);

	return _app_getcolorvalue (color_value.Hash (), is_brush);
}

rstring _app_gettooltip (size_t hash)
{
	rstring result;

	_r_fastlock_acquireshared (&lock_access);

	PITEM_APP ptr_app = _app_getapplication (hash);

	if (ptr_app)
	{
		result = ptr_app->real_path[0] ? ptr_app->real_path : ptr_app->display_name;

		// file information
		if (ptr_app->type == AppRegular)
		{
			rstring buffer;

			_app_getinformation (hash, ptr_app->real_path, &ptr_app->description);
			buffer = ptr_app->description;

			if (!buffer.IsEmpty ())
				result.AppendFormat (L"\r\n%s:\r\n" TAB_SPACE L"%s" TAB_SPACE, app.LocaleString (IDS_FILE, nullptr).GetString (), buffer.GetString ());
		}
		else if (ptr_app->type == AppService)
		{
			rstring buffer;

			_app_item_get (&services, hash, &buffer, nullptr, nullptr, nullptr, nullptr);

			if (!buffer.IsEmpty ())
				result.AppendFormat (L"\r\n%s:\r\n" TAB_SPACE L"%s\r\n" TAB_SPACE L"%s" TAB_SPACE, app.LocaleString (IDS_FILE, nullptr).GetString (), ptr_app->display_name, buffer.GetString ());
		}

		// signature information
		if (ptr_app->is_signed && ptr_app->signer && app.ConfigGet (L"IsCerificatesEnabled", false).AsBool ())
			result.AppendFormat (L"\r\n%s:\r\n" TAB_SPACE L"%s", app.LocaleString (IDS_SIGNATURE, nullptr).GetString (), ptr_app->signer);

		// timer information
		if (_app_istimeractive (hash))
		{
			result.AppendFormat (L"\r\n%s:\r\n" TAB_SPACE L"%s", app.LocaleString (IDS_TIMELEFT, nullptr).GetString (), _r_fmt_interval (apps_timer[hash] - _r_unixtime_now ()).GetString ());
		}

		// notes
		{
			rstring buffer;

			if (!_app_isexists (ptr_app))
				buffer.AppendFormat (TAB_SPACE L"%s\r\n", app.LocaleString (IDS_HIGHLIGHT_INVALID, nullptr).GetString ());

			if (ptr_app->is_silent)
				buffer.AppendFormat (TAB_SPACE L"%s\r\n", app.LocaleString (IDS_HIGHLIGHT_SILENT, nullptr).GetString ());

			if (_app_apphaverule (hash))
				buffer.AppendFormat (TAB_SPACE L"%s\r\n", app.LocaleString (IDS_HIGHLIGHT_SPECIAL, nullptr).GetString ());

			if (ptr_app->is_system)
				buffer.AppendFormat (TAB_SPACE L"%s\r\n", app.LocaleString (IDS_HIGHLIGHT_SYSTEM, nullptr).GetString ());

			// app type
			{
				if (ptr_app->type == AppNetwork)
					buffer.AppendFormat (TAB_SPACE L"%s\r\n", app.LocaleString (IDS_HIGHLIGHT_NETWORK, nullptr).GetString ());

				else if (ptr_app->type == AppPico)
					buffer.AppendFormat (TAB_SPACE L"%s\r\n", app.LocaleString (IDS_HIGHLIGHT_PICO, nullptr).GetString ());

				else if (ptr_app->type == AppStore)
					buffer.AppendFormat (TAB_SPACE L"%s\r\n", app.LocaleString (IDS_HIGHLIGHT_PACKAGE, nullptr).GetString ());

				else if (ptr_app->type == AppService)
					buffer.AppendFormat (TAB_SPACE L"%s\r\n", app.LocaleString (IDS_HIGHLIGHT_SERVICE, nullptr).GetString ());
			}

			if (!buffer.IsEmpty ())
			{
				buffer.InsertFormat (0, L"\r\n%s:\r\n", app.LocaleString (IDS_NOTES, nullptr).GetString ());
				result.Append (buffer);
			}
		}
	}

	_r_fastlock_releaseshared (&lock_access);

	return result;
}

void _wfp_destroyfilters (bool is_full)
{
	// dropped packets logging (win7+)
	if (_r_sys_validversion (6, 1))
		_wfp_logunsubscribe ();

	_r_fastlock_acquireexclusive (&lock_access);

	for (auto &p : apps)
		p.second.is_haveerrors = false;

	for (size_t i = 0; i < rules_blocklist.size (); i++)
	{
		PITEM_RULE ptr_rule = rules_blocklist.at (i);

		if (ptr_rule)
			ptr_rule->is_haveerrors = false;
	}

	for (size_t i = 0; i < rules_system.size (); i++)
	{
		PITEM_RULE ptr_rule = rules_system.at (i);

		if (ptr_rule)
			ptr_rule->is_haveerrors = false;
	}

	for (size_t i = 0; i < rules_custom.size (); i++)
	{
		PITEM_RULE ptr_rule = rules_custom.at (i);

		if (ptr_rule)
			ptr_rule->is_haveerrors = false;
	}

	_r_fastlock_releaseexclusive (&lock_access);

	DWORD result = FwpmTransactionBegin (config.hengine, 0);

	if (result != ERROR_SUCCESS)
	{
		_app_logerror (L"FwpmTransactionBegin", result, nullptr);
	}
	else
	{
		HANDLE henum = nullptr;

		result = FwpmFilterCreateEnumHandle (config.hengine, nullptr, &henum);

		if (result != ERROR_SUCCESS)
		{
			_app_logerror (L"FwpmFilterCreateEnumHandle", result, nullptr);
		}
		else
		{
			UINT32 count = 0;
			FWPM_FILTER** matchingFwpFilter = nullptr;

			result = FwpmFilterEnum (config.hengine, henum, 0xFFFFFFFF, &matchingFwpFilter, &count);

			if (result != ERROR_SUCCESS)
			{
				_app_logerror (L"FwpmFilterEnum", result, nullptr);
			}
			else
			{
				if (matchingFwpFilter)
				{
					for (UINT32 i = 0; i < count; i++)
					{
						if (matchingFwpFilter[i]->providerKey && memcmp (matchingFwpFilter[i]->providerKey, &GUID_WfpProvider, sizeof (GUID)) == 0)
						{
							result = FwpmFilterDeleteById (config.hengine, matchingFwpFilter[i]->filterId);

							if (result != ERROR_SUCCESS)
								_app_logerror (L"FwpmFilterDeleteById", result, nullptr);
						}

						if (WaitForSingleObjectEx (config.stop_evt, 0, FALSE) == WAIT_OBJECT_0)
						{
							FwpmFreeMemory ((LPVOID*)&matchingFwpFilter);
							FwpmTransactionAbort (config.hengine);

							return;
						}
					}

					FwpmFreeMemory ((LPVOID*)&matchingFwpFilter);
				}
			}
		}

		if (henum)
			FwpmFilterDestroyEnumHandle (config.hengine, henum);

		FwpmTransactionCommit (config.hengine);
	}

	if (is_full)
	{
		// set icons
		app.SetIcon (app.GetHWND (), IDI_INACTIVE, true);
		app.TraySetInfo (UID, _r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_INACTIVE), GetSystemMetrics (SM_CXSMICON)), APP_NAME);

		SetDlgItemText (app.GetHWND (), IDC_START_BTN, app.LocaleString (IDS_TRAY_START, nullptr));
	}
}

DWORD _wfp_createfilter (LPCWSTR name, FWPM_FILTER_CONDITION* lpcond, UINT32 const count, UINT8 weight, GUID layer, const GUID* callout, BOOL is_block, bool is_boottime)
{
	FWPM_FILTER filter = {0};

	WCHAR fltr_name[128] = {0};
	StringCchCopy (fltr_name, _countof (fltr_name), name ? name : APP_NAME);

	filter.displayData.name = fltr_name;
	filter.displayData.description = fltr_name;

	if (is_boottime)
		filter.flags = FWPM_FILTER_FLAG_BOOTTIME;

	else
		filter.flags = FWPM_FILTER_FLAG_PERSISTENT;

	// filter is indexed to help enable faster lookup during classification (win8+)
	if (!is_boottime && _r_sys_validversion (6, 2))
		filter.flags |= FWPM_FILTER_FLAG_INDEXED;

	filter.providerKey = (LPGUID)&GUID_WfpProvider;
	filter.layerKey = layer;
	filter.subLayerKey = GUID_WfpSublayer;

	if (count)
	{
		filter.numFilterConditions = count;
		filter.filterCondition = lpcond;
	}

	if (is_block == FWP_ACTION_CALLOUT_TERMINATING)
		filter.action.type = FWP_ACTION_CALLOUT_TERMINATING;
	else
		filter.action.type = ((is_block) ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT);

	if (callout)
		memcpy (&filter.action.calloutKey, callout, sizeof (GUID));

	filter.weight.type = FWP_UINT8;
	filter.weight.uint8 = weight;

	UINT64 filter_id = 0;

	return FwpmFilterAdd (config.hengine, &filter, nullptr, &filter_id);
}

INT CALLBACK _app_listviewcmp_appsrules (LPARAM item1, LPARAM item2, LPARAM lparam)
{
	HWND hwnd = (HWND)lparam;

	const bool is_checked1 = _r_listview_isitemchecked (hwnd, IDC_FILES_LV, (size_t)item1);
	const bool is_checked2 = _r_listview_isitemchecked (hwnd, IDC_FILES_LV, (size_t)item2);

	if (is_checked1 < is_checked2)
	{
		return 1;
	}
	else if (is_checked1 > is_checked2)
	{
		return -1;
	}

	return _r_listview_getitemtext (hwnd, IDC_FILES_LV, (size_t)item1, 0).CompareNoCase (_r_listview_getitemtext (hwnd, IDC_FILES_LV, (size_t)item2, 0));
}

INT CALLBACK _app_listviewcmp_rules (LPARAM lp1, LPARAM lp2, LPARAM)
{
	if (lp1 > lp2)
	{
		return 1;
	}
	else if (lp1 < lp2)
	{
		return -1;
	}

	return 0;
}

INT CALLBACK _app_listviewcompare (LPARAM lp1, LPARAM lp2, LPARAM lparam)
{
	const UINT column_id = LOWORD (lparam);
	const BOOL is_descend = HIWORD (lparam);

	const size_t hash1 = static_cast<size_t>(lp1);
	const size_t hash2 = static_cast<size_t>(lp2);

	INT result = 0;

	PITEM_APP const ptr_app1 = _app_getapplication (hash1);

	if (!ptr_app1)
		return 0;

	PITEM_APP const ptr_app2 = _app_getapplication (hash2);

	if (!ptr_app1)
		return 0;

	if (ptr_app1->is_enabled && !ptr_app2->is_enabled)
	{
		result = -1;
	}
	else if (!ptr_app1->is_enabled && ptr_app2->is_enabled)
	{
		result = 1;
	}
	else
	{
		if (column_id == 0)
		{
			// file
			result = _wcsicmp (ptr_app1->display_name, ptr_app2->display_name);
		}
		else if (column_id == 1)
		{
			// timestamp
			if (ptr_app1->timestamp == ptr_app2->timestamp)
			{
				result = 0;
			}
			else if (ptr_app1->timestamp < ptr_app2->timestamp)
			{
				result = -1;
			}
			else if (ptr_app1->timestamp > ptr_app2->timestamp)
			{
				result = 1;
			}
		}
	}

	return is_descend ? -result : result;
}

void _app_listviewsort_appsrules (HWND hwnd, UINT ctrl_id)
{
	SendDlgItemMessage (hwnd, ctrl_id, LVM_SORTITEMSEX, (LPARAM)hwnd, (LPARAM)&_app_listviewcmp_appsrules);
}

void _app_listviewsort_rules (HWND hwnd, UINT ctrl_id)
{
	_r_fastlock_acquireshared (&lock_access);

	SendDlgItemMessage (hwnd, ctrl_id, LVM_SORTITEMS, 0, (LPARAM)&_app_listviewcmp_rules);

	_r_fastlock_releaseshared (&lock_access);
}

void _app_listviewsort (HWND hwnd, UINT ctrl_id, INT subitem, bool is_notifycode)
{
	bool is_descend = app.ConfigGet (L"IsSortDescending", true).AsBool ();

	if (is_notifycode)
		is_descend = !is_descend;

	if (subitem == -1)
		subitem = app.ConfigGet (L"SortColumn", 1).AsBool ();

	LPARAM lparam = MAKELPARAM (subitem, is_descend);

	if (is_notifycode)
	{
		app.ConfigSet (L"IsSortDescending", is_descend);
		app.ConfigSet (L"SortColumn", (DWORD)subitem);
	}

	_r_listview_setcolumnsortindex (hwnd, ctrl_id, 0, 0);
	_r_listview_setcolumnsortindex (hwnd, ctrl_id, 1, 0);

	_r_listview_setcolumnsortindex (hwnd, ctrl_id, subitem, is_descend ? -1 : 1);

	_r_fastlock_acquireshared (&lock_access);

	SendDlgItemMessage (hwnd, ctrl_id, LVM_SORTITEMS, lparam, (LPARAM)&_app_listviewcompare);

	_r_fastlock_releaseshared (&lock_access);

	_app_refreshstatus (hwnd, true, true);
}

bool _app_canihaveaccess ()
{
	bool result = false;

	_r_fastlock_acquireshared (&lock_access);

	PITEM_APP const ptr_app = _app_getapplication (config.myhash);

	if (ptr_app)
	{
		const EnumMode mode = (EnumMode)app.ConfigGet (L"Mode", ModeWhitelist).AsUint ();

		result = (mode == ModeWhitelist && ptr_app->is_enabled || mode == ModeBlacklist && !ptr_app->is_enabled);
	}

	_r_fastlock_releaseshared (&lock_access);

	return result;
}

bool _app_ruleisport (LPCWSTR rule)
{
	if (!rule)
		return false;

	const size_t length = wcslen (rule);

	for (size_t i = 0; i < length; i++)
	{
		if (iswdigit (rule[i]) == 0 && rule[i] != L'-')
			return false;
	}

	return true;
}

bool _app_ruleishost (LPCWSTR rule)
{
	if (!rule)
		return false;

	size_t dotcounter = 0;
	size_t semicoloncounter = 0;
	bool have_nonhex = false;

	for (size_t i = (wcslen (rule) - 1); i != LAST_VALUE; i--)
	{
		const WCHAR ch = _r_str_lower (rule[i]);

		if (ch == L'-')
			continue;

		else if (ch == L'/') // prefix length set
			return false;

		else if (ch == L':' && (++semicoloncounter > 1)) // ipv6
			return false;

		else if (ch == L'.' && (++dotcounter >= 4))
			return false;

		else if ((ch >= L'g' && ch <= L'z'))
			return true;
	}

	if (have_nonhex && dotcounter <= 3)
		return true;

	return false;
}

rstring _app_parsehostaddress (LPCWSTR host, USHORT port)
{
	rstring result;

	PDNS_RECORD ppQueryResultsSet = nullptr;
	PIP4_ARRAY pSrvList = nullptr;

	DWORD options = DNS_QUERY_NO_HOSTS_FILE | DNS_QUERY_NO_NETBT | DNS_QUERY_NO_MULTICAST | DNS_QUERY_NO_LOCAL_NAME | DNS_QUERY_DONT_RESET_TTL_VALUES | DNS_QUERY_TREAT_AS_FQDN;

	// use custom dns-server (if present)
	WCHAR dnsServer[INET_ADDRSTRLEN] = {0};
	StringCchCopy (dnsServer, _countof (dnsServer), app.ConfigGet (L"DnsServerV4", nullptr)); // ipv4 dns-server address

	if (dnsServer[0])
	{
		pSrvList = new IP4_ARRAY;

		if (pSrvList)
		{
			if (InetPton (AF_INET, dnsServer, &(pSrvList->AddrArray[0])))
			{
				pSrvList->AddrCount = 1;
				options = DNS_QUERY_WIRE_ONLY | DNS_QUERY_DONT_RESET_TTL_VALUES | DNS_QUERY_TREAT_AS_FQDN;
			}
			else
			{
				delete pSrvList;
				pSrvList = nullptr;
			}
		}
	}

	const DNS_STATUS dnsStatus = DnsQuery (host, DNS_TYPE_ALL, options, pSrvList, &ppQueryResultsSet, nullptr);

	if (dnsStatus != ERROR_SUCCESS)
	{
		_app_logerror (L"DnsQuery", dnsStatus, host, false);
	}
	else
	{
		for (auto current = ppQueryResultsSet; current != nullptr; current = current->pNext)
		{
			if (current->wType == DNS_TYPE_A)
			{
				// ipv4 address
				WCHAR str[INET_ADDRSTRLEN] = {0};
				InetNtop (AF_INET, &(current->Data.A.IpAddress), str, _countof (str));

				result.Append (str);

				if (port)
					result.AppendFormat (L":%d", port);

				result.Append (RULE_DELIMETER);
			}
			else if (current->wType == DNS_TYPE_AAAA)
			{
				// ipv6 address
				WCHAR str[INET6_ADDRSTRLEN] = {0};
				InetNtop (AF_INET6, &(current->Data.AAAA.Ip6Address), str, _countof (str));

				result.Append (str);

				if (port)
					result.AppendFormat (L":%d", port);

				result.Append (RULE_DELIMETER);
			}
			else if (current->wType == DNS_TYPE_CNAME)
			{
				// canonical name
				if (current->Data.CNAME.pNameHost)
					result.Append (_app_parsehostaddress (current->Data.CNAME.pNameHost, port));
			}
		}

		if (pSrvList)
		{
			delete pSrvList;
			pSrvList = nullptr;
		}

		DnsRecordListFree (ppQueryResultsSet, DnsFreeRecordList);
	}

	return result.Trim (RULE_DELIMETER);
}

bool _app_parsenetworkstring (rstring network_string, NET_ADDRESS_FORMAT* format_ptr, USHORT* port_ptr, FWP_V4_ADDR_AND_MASK* paddr4, FWP_V6_ADDR_AND_MASK* paddr6, LPWSTR paddr_dns)
{
	NET_ADDRESS_INFO ni;
	SecureZeroMemory (&ni, sizeof (ni));

	USHORT port = 0;
	BYTE prefix_length = 0;

	const DWORD types = (app.ConfigGet (L"IsHostsEnabled", true).AsBool () && _app_canihaveaccess () ? (NET_STRING_ANY_ADDRESS | NET_STRING_ANY_SERVICE | NET_STRING_IP_NETWORK | NET_STRING_ANY_ADDRESS_NO_SCOPE | NET_STRING_ANY_SERVICE_NO_SCOPE) : (NET_STRING_IP_ADDRESS | NET_STRING_IP_SERVICE | NET_STRING_IP_NETWORK | NET_STRING_IP_ADDRESS_NO_SCOPE));
	const DWORD errcode = ParseNetworkString (network_string, types, &ni, &port, &prefix_length);

	if (errcode != ERROR_SUCCESS)
	{
		_app_logerror (L"ParseNetworkString", errcode, network_string, true);
		return false;
	}
	else
	{
		if (format_ptr)
			*format_ptr = ni.Format;

		if (port_ptr)
			*port_ptr = port;

		if (ni.Format == NET_ADDRESS_IPV4)
		{
			if (paddr4)
			{
				ULONG mask = 0;
				ConvertLengthToIpv4Mask (prefix_length, &mask);

				paddr4->mask = ntohl (mask);
				paddr4->addr = ntohl (ni.Ipv4Address.sin_addr.S_un.S_addr);
			}

			return true;
		}
		else if (ni.Format == NET_ADDRESS_IPV6)
		{
			if (paddr6)
			{
				paddr6->prefixLength = min ((INT)prefix_length, 128);
				memcpy (paddr6->addr, ni.Ipv6Address.sin6_addr.u.Byte, FWP_V6_ADDR_SIZE);
			}

			return true;
		}
		else if (ni.Format == NET_ADDRESS_DNS_NAME)
		{
			if (paddr_dns)
			{
				const rstring host = _app_parsehostaddress (ni.NamedAddress.Address, port);

				if (!host.IsEmpty ())
					StringCchCopy (paddr_dns, LEN_HOST_MAX, host);
			}

			return true;
		}
	}

	return false;
}

bool _app_parserulestring (rstring rule, PITEM_ADDRESS ptr_addr, EnumRuleType *ptype)
{
	rule.Trim (L"\r\n "); // trim whitespace

	if (rule.IsEmpty ())
		return false;

	if (rule.At (0) == L'*')
		return true;

	EnumRuleType type = TypeUnknown;
	size_t range_pos = rstring::npos;

	if (ptype)
		type = *ptype;

	// auto-parse rule type
	if (type == TypeUnknown)
	{
		if (_app_ruleisport (rule))
			type = TypePort;

		else
			type = _app_ruleishost (rule) ? TypeHost : TypeIp;
	}

	if (type == TypeUnknown)
		return false;

	if (type == TypePort || type == TypeIp)
	{
		range_pos = rule.Find (L'-');

		if (ptr_addr && range_pos != rstring::npos)
			ptr_addr->is_range = true;
	}

	WCHAR range_start[LEN_IP_MAX] = {0};
	WCHAR range_end[LEN_IP_MAX] = {0};

	if (range_pos != rstring::npos)
	{
		StringCchCopy (range_start, _countof (range_start), rule.Midded (0, range_pos));
		StringCchCopy (range_end, _countof (range_end), rule.Midded (range_pos + 1));
	}

	if (type == TypePort)
	{
		if (range_pos == rstring::npos)
		{
			// ...port
			if (ptr_addr)
			{
				ptr_addr->type = TypePort;
				ptr_addr->port = (UINT16)rule.AsUlong ();
			}

			return true;
		}
		else
		{
			// ...port range
			if (ptr_addr)
			{
				ptr_addr->type = TypePort;

				if (ptr_addr->prange)
				{
					ptr_addr->prange->valueLow.type = FWP_UINT16;
					ptr_addr->prange->valueLow.uint16 = (UINT16)wcstoul (range_start, nullptr, 10);

					ptr_addr->prange->valueHigh.type = FWP_UINT16;
					ptr_addr->prange->valueHigh.uint16 = (UINT16)wcstoul (range_end, nullptr, 10);
				}
			}

			return true;
		}
	}
	else
	{
		NET_ADDRESS_FORMAT format;

		FWP_V4_ADDR_AND_MASK addr4 = {0};
		FWP_V6_ADDR_AND_MASK addr6 = {0};

		USHORT port2 = 0;

		if (range_pos == rstring::npos)
		{
			// ...ip/host
			if (_app_parsenetworkstring (rule, &format, &port2, &addr4, &addr6, ptr_addr ? ptr_addr->host : nullptr))
			{
				if (format == NET_ADDRESS_IPV4)
				{
					if (ptr_addr && ptr_addr->paddr4)
					{
						ptr_addr->paddr4->mask = addr4.mask;
						ptr_addr->paddr4->addr = addr4.addr;
					}
				}
				else if (format == NET_ADDRESS_IPV6)
				{
					if (ptr_addr && ptr_addr->paddr6)
					{
						ptr_addr->paddr6->prefixLength = addr6.prefixLength;
						memcpy (ptr_addr->paddr6->addr, addr6.addr, FWP_V6_ADDR_SIZE);
					}
				}
				else if (format == NET_ADDRESS_DNS_NAME)
				{
					if (ptr_addr)
					{
						ptr_addr->type = TypeHost;
						//ptr_addr->host = <hosts>;
					}
				}
				else
				{
					return false;
				}

				if (ptr_addr)
				{
					ptr_addr->format = format;
					ptr_addr->type = TypeIp;

					if (port2)
						ptr_addr->port = port2;
				}

				return true;
			}
			else
			{
				return false;
			}
		}
		else
		{
			// ...ip range (start)
			if (_app_parsenetworkstring (range_start, &format, &port2, &addr4, &addr6, nullptr))
			{
				if (format == NET_ADDRESS_IPV4)
				{
					if (ptr_addr && ptr_addr->prange)
					{
						ptr_addr->prange->valueLow.type = FWP_UINT32;
						ptr_addr->prange->valueLow.uint32 = addr4.addr;
					}
				}
				else if (format == NET_ADDRESS_IPV6)
				{
					if (ptr_addr && ptr_addr->prange)
					{
						ptr_addr->prange->valueLow.type = FWP_BYTE_ARRAY16_TYPE;
						memcpy (ptr_addr->prange->valueLow.byteArray16->byteArray16, addr6.addr, FWP_V6_ADDR_SIZE);
					}
				}
				else
				{
					return false;
				}

				if (port2 && ptr_addr && !ptr_addr->port)
					ptr_addr->port = port2;
			}
			else
			{
				return false;
			}

			// ...ip range (end)
			if (_app_parsenetworkstring (range_end, &format, &port2, &addr4, &addr6, nullptr))
			{
				if (format == NET_ADDRESS_IPV4)
				{
					if (ptr_addr && ptr_addr->prange)
					{
						ptr_addr->prange->valueHigh.type = FWP_UINT32;
						ptr_addr->prange->valueHigh.uint32 = addr4.addr;
					}
				}
				else if (format == NET_ADDRESS_IPV6)
				{
					if (ptr_addr && ptr_addr->prange)
					{
						ptr_addr->prange->valueHigh.type = FWP_BYTE_ARRAY16_TYPE;
						memcpy (ptr_addr->prange->valueHigh.byteArray16->byteArray16, addr6.addr, FWP_V6_ADDR_SIZE);
					}
				}
				else
				{
					return false;
				}
			}
			else
			{
				return false;
			}

			if (ptr_addr)
			{
				ptr_addr->format = format;
				ptr_addr->type = TypeIp;
			}
		}
	}

	return true;
}

bool ByteBlobAlloc (PVOID data, size_t length, FWP_BYTE_BLOB** lpblob)
{
	if (!data || !length || !lpblob)
		return false;

	*lpblob = new FWP_BYTE_BLOB;

	if (*lpblob)
	{
		const PUINT8 tmp_ptr = new UINT8[length];

		if (!tmp_ptr)
		{
			delete *lpblob;
			*lpblob = nullptr;

			return false;
		}
		else
		{
			(*lpblob)->data = tmp_ptr;
			(*lpblob)->size = (UINT32)length;

			memcpy ((*lpblob)->data, data, length);

			return true;
		}
	}

	return false;
}

void ByteBlobFree (FWP_BYTE_BLOB** lpblob)
{
	if (lpblob && *lpblob)
	{
		FWP_BYTE_BLOB* blob = *lpblob;

		if (blob)
		{
			if (blob->data)
			{
				delete[] blob->data;
				blob->data = nullptr;
			}

			delete blob;
			blob = nullptr;
			*lpblob = nullptr;
		}
	}
}

DWORD _FwpmGetAppIdFromFileName1 (LPCWSTR path, FWP_BYTE_BLOB** lpblob, EnumAppType type)
{
	if (!path || !lpblob)
		return ERROR_BAD_ARGUMENTS;

	rstring path_buff;

	DWORD result = (DWORD)-1;

	if (type == AppRegular || type == AppNetwork || type == AppService)
	{
		path_buff = path;

		if (_r_str_hash (path) == config.ntoskrnl_hash)
		{
			result = ERROR_SUCCESS;
		}
		else
		{
			result = _r_path_ntpathfromdos (path_buff);

			// file is inaccessible or not found, maybe low-level driver preventing file access?
			// try another way!
			if (result == ERROR_ACCESS_DENIED || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND)
			{
				// file path (root)
				WCHAR path_root[128] = {0};
				StringCchCopy (path_root, _countof (path_root), path);
				PathStripToRoot (path_root);

				// file path (without root)
				WCHAR path_noroot[MAX_PATH] = {0};
				StringCchCopy (path_noroot, _countof (path_noroot), PathSkipRoot (path));

				path_buff = path_root;
				result = _r_path_ntpathfromdos (path_buff);

				if (result != ERROR_SUCCESS)
					return result;

				path_buff.Append (path_noroot);
				path_buff.ToLower (); // lower is important!
			}
			else if (result != ERROR_SUCCESS)
			{
				return result;
			}
		}
	}
	else if (type == AppPico || type == AppDevice)
	{
		path_buff = path;

		if (type == AppDevice)
			path_buff.ToLower (); // lower is important!

		result = ERROR_SUCCESS;
	}
	else
	{
		return ERROR_FILE_NOT_FOUND;
	}

	// allocate buffer
	if (result == ERROR_SUCCESS && !path_buff.IsEmpty ())
	{
		if (!ByteBlobAlloc ((LPVOID)path_buff.GetString (), ((path_buff.GetLength ()) * sizeof (WCHAR)) + sizeof (WCHAR), lpblob))
			return ERROR_OUTOFMEMORY;
	}
	else
	{
		return ERROR_BAD_ARGUMENTS;
	}

	return ERROR_SUCCESS;
}

bool _wfp_createrulefilter (LPCWSTR name, LPCWSTR rule, PITEM_APP const ptr_app, FWP_DIRECTION dir, EnumRuleType* ptype, UINT8 protocol, ADDRESS_FAMILY af, BOOL is_block, UINT8 weight, bool is_boottime)
{
	UINT32 count = 0;
	FWPM_FILTER_CONDITION fwfc[6] = {0};

	// rule without address
	if (rule && rule[0] == L'*')
		rule = nullptr;

	FWP_BYTE_BLOB* blob = nullptr;

	FWP_V4_ADDR_AND_MASK addr4 = {0};
	FWP_V6_ADDR_AND_MASK addr6 = {0};

	FWP_RANGE range;
	SecureZeroMemory (&range, sizeof (range));

	ITEM_ADDRESS addr;
	SecureZeroMemory (&addr, sizeof (addr));

	addr.paddr4 = &addr4;
	addr.paddr6 = &addr6;
	addr.prange = &range;

	UINT32 ip_idx = UINT32 (-1);
	UINT32 port_idx = UINT32 (-1);

	if (ptr_app)
	{
		if (ptr_app->type == AppStore) // windows store app (win8+)
		{
			if (ptr_app->psid)
			{
				fwfc[count].fieldKey = FWPM_CONDITION_ALE_PACKAGE_ID;
				fwfc[count].matchType = FWP_MATCH_EQUAL;
				fwfc[count].conditionValue.type = FWP_SID;
				fwfc[count].conditionValue.sid = (SID*)ptr_app->psid;

				count += 1;
			}
			else
			{
				_app_logerror (TEXT (__FUNCTION__), 0, ptr_app->display_name, true);
				return false;
			}
		}
		else
		{
			if (ptr_app->type != AppService || (ptr_app->type == AppService && ptr_app->real_path[0]))
			{
				LPCWSTR path = ptr_app->type == AppService ? ptr_app->real_path : ptr_app->original_path;
				const DWORD rc = _FwpmGetAppIdFromFileName1 (path, &blob, ptr_app->type);

				if (rc != ERROR_SUCCESS)
				{
					ByteBlobFree (&blob);
					_app_logerror (L"FwpmGetAppIdFromFileName", rc, path, true);

					return false;
				}
				else
				{
					fwfc[count].fieldKey = FWPM_CONDITION_ALE_APP_ID;
					fwfc[count].matchType = FWP_MATCH_EQUAL;
					fwfc[count].conditionValue.type = FWP_BYTE_BLOB_TYPE;
					fwfc[count].conditionValue.byteBlob = blob;

					count += 1;
				}
			}
			else
			{
				return false;
			}

			if (ptr_app->type == AppService) // windows service
			{
				if (ptr_app->psd && ByteBlobAlloc (ptr_app->psd, GetSecurityDescriptorLength (ptr_app->psd), &blob))
				{
					fwfc[count].fieldKey = FWPM_CONDITION_ALE_USER_ID;
					fwfc[count].matchType = FWP_MATCH_EQUAL;
					fwfc[count].conditionValue.type = FWP_SECURITY_DESCRIPTOR_TYPE;
					fwfc[count].conditionValue.sd = blob;

					count += 1;
				}
				else
				{
					ByteBlobFree (&blob);
					_app_logerror (TEXT (__FUNCTION__), 0, ptr_app->display_name, true);

					return false;
				}
			}
		}
	}

	if (protocol)
	{
		fwfc[count].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
		fwfc[count].matchType = FWP_MATCH_EQUAL;
		fwfc[count].conditionValue.type = FWP_UINT8;
		fwfc[count].conditionValue.uint8 = protocol;

		count += 1;
	}

	if (rule)
	{
		if (_app_parserulestring (rule, &addr, ptype))
		{
			if (addr.is_range && (addr.type == TypeIp || addr.type == TypePort))
			{
				if (addr.type == TypeIp)
				{
					if (addr.format == NET_ADDRESS_IPV4)
						af = AF_INET;

					else if (addr.format == NET_ADDRESS_IPV6)
						af = AF_INET6;

					else
						return false;
				}

				fwfc[count].matchType = FWP_MATCH_RANGE;
				fwfc[count].conditionValue.type = FWP_RANGE_TYPE;
				fwfc[count].conditionValue.rangeValue = &range;

				if (addr.type == TypePort)
					port_idx = count;

				else
					ip_idx = count;

				count += 1;
			}
			else if (addr.type == TypePort)
			{
				fwfc[count].matchType = FWP_MATCH_EQUAL;
				fwfc[count].conditionValue.type = FWP_UINT16;
				fwfc[count].conditionValue.uint16 = addr.port;

				port_idx = count;
				count += 1;
			}
			else if (addr.type == TypeHost || addr.type == TypeIp)
			{
				if (addr.format == NET_ADDRESS_IPV4)
				{
					af = AF_INET;

					fwfc[count].matchType = FWP_MATCH_EQUAL;
					fwfc[count].conditionValue.type = FWP_V4_ADDR_MASK;
					fwfc[count].conditionValue.v4AddrMask = &addr4;

					ip_idx = count;
					count += 1;
				}
				else if (addr.format == NET_ADDRESS_IPV6)
				{
					af = AF_INET6;

					fwfc[count].matchType = FWP_MATCH_EQUAL;
					fwfc[count].conditionValue.type = FWP_V6_ADDR_MASK;
					fwfc[count].conditionValue.v6AddrMask = &addr6;

					ip_idx = count;
					count += 1;
				}
				else if (addr.format == NET_ADDRESS_DNS_NAME)
				{
					ByteBlobFree (&blob);

					rstring::rvector arr = rstring (addr.host).AsVector (RULE_DELIMETER);

					if (arr.empty ())
					{
						return false;
					}
					else
					{
						for (size_t i = 0; i < arr.size (); i++)
						{
							EnumRuleType type = TypeIp;

							if (!_wfp_createrulefilter (name, arr.at (i), ptr_app, dir, &type, protocol, af, is_block, weight, is_boottime))
								return false;
						}
					}

					return true;
				}
				else
				{
					ByteBlobFree (&blob);
					return false;
				}

				// set port if available
				if (addr.port)
				{
					fwfc[count].matchType = FWP_MATCH_EQUAL;
					fwfc[count].conditionValue.type = FWP_UINT16;
					fwfc[count].conditionValue.uint16 = addr.port;

					port_idx = count;
					count += 1;
				}
			}
			else
			{
				ByteBlobFree (&blob);
				return false;
			}
		}
		else
		{
			ByteBlobFree (&blob);
			return false;
		}
	}

	// create filters
	DWORD result = 0;

	if (dir == FWP_DIRECTION_OUTBOUND || dir == FWP_DIRECTION_MAX)
	{
		if (ip_idx != UINT32 (-1))
			fwfc[ip_idx].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;

		if (port_idx != UINT32 (-1))
			fwfc[port_idx].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;

		if (af == AF_INET || af == AF_UNSPEC)
		{
			result = _wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, is_block, is_boottime);

			if (result != ERROR_SUCCESS)
				_app_logerror (L"FwpmFilterAdd", result, rule, true);
		}

		if (af == AF_INET6 || af == AF_UNSPEC)
		{
			result = _wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, is_block, is_boottime);

			if (result != ERROR_SUCCESS)
				_app_logerror (L"FwpmFilterAdd", result, rule, true);
		}
	}

	if (dir == FWP_DIRECTION_INBOUND || dir == FWP_DIRECTION_MAX)
	{
		if (ip_idx != UINT32 (-1))
			fwfc[ip_idx].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;

		if (port_idx != UINT32 (-1))
			fwfc[port_idx].fieldKey = FWPM_CONDITION_IP_LOCAL_PORT;

		if (af == AF_INET || af == AF_UNSPEC)
		{
			result = _wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, is_block, is_boottime);

			if (result != ERROR_SUCCESS)
				_app_logerror (L"FwpmFilterAdd", result, rule, true);
		}

		if (af == AF_INET6 || af == AF_UNSPEC)
		{
			result = _wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, is_block, is_boottime);

			if (result != ERROR_SUCCESS)
				_app_logerror (L"FwpmFilterAdd", result, rule, true);
		}
	}

	// apply listen connections filter rule (apps/ports)
	if (!app.ConfigGet (L"AllowListenConnections2", true).AsBool () && !protocol && ip_idx == UINT32 (-1))
	{
		if (dir == FWP_DIRECTION_INBOUND || dir == FWP_DIRECTION_MAX)
		{
			if (af == AF_INET || af == AF_UNSPEC)
			{
				result = _wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_LISTEN_V4, nullptr, is_block, is_boottime);

				if (result != ERROR_SUCCESS)
					_app_logerror (L"FwpmFilterAdd", result, rule, true);
			}

			if (af == AF_INET6 || af == AF_UNSPEC)
			{
				result = _wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_LISTEN_V6, nullptr, is_block, is_boottime);

				if (result != ERROR_SUCCESS)
					_app_logerror (L"FwpmFilterAdd", result, rule, true);
			}
		}
	}

	// proxy connections (win8+)
	if (app.ConfigGet (L"EnableProxySupport", false).AsBool () && _r_sys_validversion (6, 2))
	{
		if (ptr_app && dir == FWP_DIRECTION_MAX && !rule)
		{
			fwfc[count].fieldKey = FWPM_CONDITION_FLAGS;
			fwfc[count].matchType = FWP_MATCH_FLAGS_ANY_SET;
			fwfc[count].conditionValue.type = FWP_UINT32;
			fwfc[count].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_CONNECTION_REDIRECTED | FWP_CONDITION_FLAG_IS_PROXY_CONNECTION;

			count += 1;

			result = _wfp_createfilter (name, fwfc, count, FILTER_WEIGHT_HIGHEST_IMPORTANT, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, FALSE, FALSE);

			if (result != ERROR_SUCCESS)
				_app_logerror (L"FwpmFilterAdd", result, rule, true);

			result = _wfp_createfilter (name, fwfc, count, FILTER_WEIGHT_HIGHEST_IMPORTANT, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, FALSE, FALSE);

			if (result != ERROR_SUCCESS)
				_app_logerror (L"FwpmFilterAdd", result, rule, true);
		}
	}

	ByteBlobFree (&blob);
	return true;
}

LPVOID _app_loadresource (LPCWSTR res, PDWORD size)
{
	const HINSTANCE hinst = app.GetHINSTANCE ();

	HRSRC hres = FindResource (hinst, res, RT_RCDATA);

	if (hres)
	{
		HGLOBAL hloaded = LoadResource (hinst, hres);

		if (hloaded)
		{
			LPVOID pLockedResource = LockResource (hloaded);

			if (pLockedResource)
			{
				DWORD dwResourceSize = SizeofResource (hinst, hres);

				if (dwResourceSize != 0)
				{
					if (size)
						*size = dwResourceSize;

					return pLockedResource;
				}
			}
		}
	}

	return nullptr;
}

void _app_loadrules (HWND hwnd, LPCWSTR path, LPCWSTR path_backup, bool is_internal, std::vector<PITEM_RULE> *ptr_rules)
{
	if (!ptr_rules)
		return;

	// clear it first
	_r_fastlock_acquireexclusive (&lock_access);

	for (size_t i = 0; i < ptr_rules->size (); i++)
	{
		if (ptr_rules->at (i))
			_app_freerule (&ptr_rules->at (i));
	}

	ptr_rules->clear ();

	_r_fastlock_releaseexclusive (&lock_access);

	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file (path, PUGIXML_LOAD_FLAGS, PUGIXML_LOAD_ENCODING);

	if (!result)
	{
		// if file not found or parsing error, load from backup
		if (path_backup)
		{
			if (is_internal)
			{
				DWORD size = 0;
				LPVOID buffer = _app_loadresource (path_backup, &size);

				if (buffer)
					result = doc.load_buffer (buffer, size, PUGIXML_LOAD_FLAGS, PUGIXML_LOAD_ENCODING);
			}
			else
			{
				if (_r_fs_exists (path_backup))
					result = doc.load_file (path_backup, PUGIXML_LOAD_FLAGS, PUGIXML_LOAD_ENCODING);
			}
		}

		// show only syntax, memory and i/o errors...
		if (!result && result.status != pugi::status_file_not_found)
			_app_logerror (L"pugi::load_file", 0, _r_fmt (L"status: %d, offset: %d, text: %s, file: %s", result.status, result.offset, rstring (result.description ()).GetString (), path));
	}

	if (result)
	{
		pugi::xml_node root = doc.child (L"root");

		if (root)
		{
			_r_fastlock_acquireexclusive (&lock_access);

			const bool is_loadextrarules = app.ConfigGet (L"IsExtraRulesEnabled", false).AsBool ();

			for (pugi::xml_node item = root.child (L"item"); item; item = item.next_sibling (L"item"))
			{
				if (!item.attribute (L"is_extra").empty () && item.attribute (L"is_extra").as_bool () && !is_loadextrarules)
					continue;

				const size_t idx = ptr_rules->size ();
				PITEM_RULE rule_ptr = new ITEM_RULE;

				if (rule_ptr)
				{
					// allocate required memory
					{
						rstring attr_name = item.attribute (L"name").as_string ();
						rstring attr_rule = item.attribute (L"rule").as_string ();

						const size_t name_length = min (attr_name.GetLength (), RULE_NAME_CCH_MAX) + 1;
						const size_t rule_length = min (attr_rule.GetLength (), RULE_RULE_CCH_MAX) + 1;

						rule_ptr->pname = new WCHAR[name_length];
						rule_ptr->prule = new WCHAR[rule_length];

						if (rule_ptr->pname)
							StringCchCopy (rule_ptr->pname, name_length, attr_name);

						if (rule_ptr->prule)
							StringCchCopy (rule_ptr->prule, rule_length, attr_rule);
					}

					rule_ptr->dir = (FWP_DIRECTION)item.attribute (L"dir").as_uint ();

					if (!item.attribute (L"type").empty ())
						rule_ptr->type = (EnumRuleType)item.attribute (L"type").as_uint ();

					rule_ptr->protocol = (UINT8)item.attribute (L"protocol").as_uint ();
					rule_ptr->version = (ADDRESS_FAMILY)item.attribute (L"version").as_uint ();

					if (!item.attribute (L"apps").empty ())
					{
						rstring::rvector arr = rstring (item.attribute (L"apps").as_string ()).AsVector (RULE_DELIMETER);

						for (size_t i = 0; i < arr.size (); i++)
						{
							const rstring path_app = _r_path_expand (arr.at (i).Trim (L"\r\n "));
							const size_t hash = path_app.Hash ();

							if (hash)
							{
								if (!_app_getapplication (hash))
									_app_addapplication (hwnd, path_app, 0, false, false, true);

								if (is_internal)
									apps_undelete[hash] = true; // prevent deletion for internal rules apps

								rule_ptr->apps[hash] = true;
							}
						}
					}

					rule_ptr->is_block = item.attribute (L"is_block").as_bool ();
					rule_ptr->is_enabled = item.attribute (L"is_enabled").as_bool ();

					if (is_internal)
					{
						// internal rules
						if (rules_config.find (rule_ptr->pname) != rules_config.end ())
							rule_ptr->is_enabled = rules_config[rule_ptr->pname];

						else
							rules_config[rule_ptr->pname] = rule_ptr->is_enabled;
					}

					ptr_rules->push_back (rule_ptr);
				}
			}

			_r_fastlock_releaseexclusive (&lock_access);
		}
	}
}

void _app_profileload (HWND hwnd, LPCWSTR path_apps = nullptr, LPCWSTR path_rules = nullptr)
{
	// load applications
	{
		const size_t item_id = (size_t)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
		const INT scroll_pos = GetScrollPos (GetDlgItem (hwnd, IDC_LISTVIEW), SB_VERT);
		bool is_meadded = false;

		// generate package list (win8+)
		if (_r_sys_validversion (6, 2))
			_app_generate_packages ();

		// generate services list
		_app_generate_services ();

		// load apps list
		{
			_r_fastlock_acquireexclusive (&lock_access);

			apps.clear ();
			_r_listview_deleteallitems (hwnd, IDC_LISTVIEW);

			pugi::xml_document doc;
			pugi::xml_parse_result result = doc.load_file (path_apps ? path_apps : config.apps_path, PUGIXML_LOAD_FLAGS, PUGIXML_LOAD_ENCODING);

			// load backup
			if (!result)
			{
				rstring path_apps_backup;
				path_apps_backup.Format (L"%s.bak", config.apps_path);

				if (_r_fs_exists (path_apps_backup))
					result = doc.load_file (path_apps_backup, PUGIXML_LOAD_FLAGS, PUGIXML_LOAD_ENCODING);
			}

			if (!result)
			{
				// show only syntax, memory and i/o errors...
				if (result.status != pugi::status_file_not_found)
					_app_logerror (L"pugi::load_file", 0, _r_fmt (L"status: %d, offset: %d, text: %s, file: %s", result.status, result.offset, rstring (result.description ()).GetString (), path_apps ? path_apps : config.apps_path));
			}
			else
			{
				pugi::xml_node root = doc.child (L"root");

				if (root)
				{
					bool is_timerchanged = false;

					for (pugi::xml_node item = root.child (L"item"); item; item = item.next_sibling (L"item"))
					{
						const size_t hash = _r_str_hash (item.attribute (L"path").as_string ());
						bool is_enabled = item.attribute (L"is_enabled").as_bool ();

						if (_r_str_hash (item.attribute (L"path").as_string ()) == config.myhash)
							is_meadded = true;

						if (!item.attribute (L"timer").empty ())
						{
							if (!_app_istimeractive (hash))
							{
								apps_timer[hash] = item.attribute (L"timer").as_ullong ();
								is_timerchanged = true;
							}
						}

						_app_addapplication (hwnd, item.attribute (L"path").as_string (), item.attribute (L"timestamp").as_ullong (), item.attribute (L"is_silent").as_bool (), is_enabled, true);
					}

					_app_timer_apply (hwnd, false);
				}
			}

			_r_fastlock_releaseexclusive (&lock_access);
		}

		if (!is_meadded)
		{
			_r_fastlock_acquireexclusive (&lock_access);
			_app_addapplication (hwnd, app.GetBinaryPath (), 0, false, (app.ConfigGet (L"Mode", ModeWhitelist).AsUint () == ModeWhitelist) ? true : false, true);
			_r_fastlock_releaseexclusive (&lock_access);
		}

		ShowItem (hwnd, IDC_LISTVIEW, item_id, scroll_pos);
	}

	// load rules config
	{
		_r_fastlock_acquireexclusive (&lock_access);

		rules_config.clear ();

		pugi::xml_document doc;
		pugi::xml_parse_result result = doc.load_file (config.rules_config_path, PUGIXML_LOAD_FLAGS, PUGIXML_LOAD_ENCODING);

		if (!result)
			result = doc.load_file (_r_fmt (L"%s.bak", config.rules_config_path), PUGIXML_LOAD_FLAGS, PUGIXML_LOAD_ENCODING);

		if (!result)
		{
			// show only syntax, memory and i/o errors...
			if (result.status != pugi::status_file_not_found)
				_app_logerror (L"pugi::load_file", 0, _r_fmt (L"status: %d, offset: %d, text: %s, file: %s", result.status, result.offset, rstring (result.description ()).GetString (), config.rules_config_path));
		}
		else
		{
			pugi::xml_node root = doc.child (L"root");

			if (root)
			{
				for (pugi::xml_node item = root.child (L"item"); item; item = item.next_sibling (L"item"))
					rules_config[item.attribute (L"name").as_string ()] = item.attribute (L"is_enabled").as_bool ();
			}
		}

		_r_fastlock_releaseexclusive (&lock_access);
	}

	// load blocklist rules (internal)
	if (rules_blocklist.empty ())
		_app_loadrules (hwnd, _r_fmt (L"%s\\" XML_BLOCKLIST, app.GetProfileDirectory ()), MAKEINTRESOURCE (IDR_RULES_BLOCKLIST), true, &rules_blocklist);

	// load system rules (internal)
	if (rules_system.empty ())
		_app_loadrules (hwnd, _r_fmt (L"%s\\" XML_RULES_SYSTEM, app.GetProfileDirectory ()), MAKEINTRESOURCE (IDR_RULES_SYSTEM), true, &rules_system);

	// load custom rules
	_app_loadrules (hwnd, path_rules ? path_rules : config.rules_custom_path, _r_fmt (L"%s.bak", config.rules_custom_path), false, &rules_custom);

	_app_refreshstatus (hwnd, true, true);
}

void _app_profilesave (HWND, LPCWSTR path_apps = nullptr, LPCWSTR path_rules = nullptr)
{
	// apps list
	{
		pugi::xml_document doc;
		pugi::xml_node root = doc.append_child (L"root");

		if (root)
		{
			_r_fastlock_acquireshared (&lock_access);

			const time_t current_time = _r_unixtime_now ();

			for (auto &p : apps)
			{
				const size_t hash = p.first;

				if (hash)
				{
					pugi::xml_node item = root.append_child (L"item");

					if (item)
					{
						PITEM_APP const ptr_app = &p.second;

						item.append_attribute (L"path").set_value (ptr_app->original_path);
						item.append_attribute (L"timestamp").set_value (ptr_app->timestamp);

						// set timer (if presented)
						if (_app_istimeractive (hash))
							item.append_attribute (L"timer").set_value (apps_timer[hash]);

						item.append_attribute (L"is_silent").set_value (ptr_app->is_silent);
						item.append_attribute (L"is_enabled").set_value (ptr_app->is_enabled);
					}
				}
			}

			_r_fastlock_releaseshared (&lock_access);

			doc.save_file (path_apps ? path_apps : config.apps_path, L"\t", PUGIXML_SAVE_FLAGS, PUGIXML_SAVE_ENCODING);

			// make backup
			if (!path_apps && !apps.empty ())
				doc.save_file (_r_fmt (L"%s.bak", config.apps_path), L"\t", PUGIXML_SAVE_FLAGS, PUGIXML_SAVE_ENCODING);
		}
	}

	// internal rules config
	{
		pugi::xml_document doc;
		pugi::xml_node root = doc.append_child (L"root");

		if (root)
		{
			_r_fastlock_acquireshared (&lock_access);

			for (auto const &p : rules_config)
			{
				pugi::xml_node item = root.append_child (L"item");

				if (item)
				{
					item.append_attribute (L"name").set_value (p.first);
					item.append_attribute (L"is_enabled").set_value (p.second);
				}
			}

			_r_fastlock_releaseshared (&lock_access);

			doc.save_file (config.rules_config_path, L"\t", PUGIXML_SAVE_FLAGS, PUGIXML_SAVE_ENCODING);

			// make backup
			if (!rules_config.empty ())
				doc.save_file (_r_fmt (L"%s.bak", config.rules_config_path), L"\t", PUGIXML_SAVE_FLAGS, PUGIXML_SAVE_ENCODING);
		}
	}

	// custom rules
	{
		pugi::xml_document doc;
		pugi::xml_node root = doc.append_child (L"root");

		if (root)
		{
			_r_fastlock_acquireshared (&lock_access);

			for (size_t i = 0; i < rules_custom.size (); i++)
			{
				PITEM_RULE const ptr_rule = rules_custom.at (i);

				if (ptr_rule)
				{
					pugi::xml_node item = root.append_child (L"item");

					if (item)
					{
						item.append_attribute (L"name").set_value (ptr_rule->pname);
						item.append_attribute (L"rule").set_value (ptr_rule->prule);
						item.append_attribute (L"dir").set_value (ptr_rule->dir);

						if (ptr_rule->type != TypeUnknown)
							item.append_attribute (L"type").set_value (ptr_rule->type);

						item.append_attribute (L"protocol").set_value (ptr_rule->protocol);
						item.append_attribute (L"version").set_value (ptr_rule->version);

						// add apps attribute
						if (!ptr_rule->apps.empty ())
						{
							rstring arr;
							bool is_haveapps = false;

							for (auto const &p : ptr_rule->apps)
							{
								PITEM_APP const ptr_app = _app_getapplication (p.first);

								if (ptr_app)
								{
									arr.Append (_r_path_unexpand (ptr_app->original_path));
									arr.Append (RULE_DELIMETER);

									if (!is_haveapps)
										is_haveapps = true;
								}
							}

							if (is_haveapps)
								item.append_attribute (L"apps").set_value (arr.Trim (RULE_DELIMETER));
						}

						item.append_attribute (L"is_block").set_value (ptr_rule->is_block);
						item.append_attribute (L"is_enabled").set_value (ptr_rule->is_enabled);
					}
				}
			}

			_r_fastlock_releaseshared (&lock_access);

			doc.save_file (path_rules ? path_rules : config.rules_custom_path, L"\t", PUGIXML_SAVE_FLAGS, PUGIXML_SAVE_ENCODING);

			// make backup
			if (!path_rules && !rules_custom.empty ())
				doc.save_file (_r_fmt (L"%s.bak", config.rules_custom_path), L"\t", PUGIXML_SAVE_FLAGS, PUGIXML_SAVE_ENCODING);
		}
	}
}

bool _wfp_isfiltersinstalled ()
{
	HKEY hkey = nullptr;
	bool result = false;

	if (RegOpenKeyEx (HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\services\\BFE\\Parameters\\Policy\\Persistent\\Provider", 0, KEY_READ, &hkey) == ERROR_SUCCESS)
	{
		OLECHAR* guidString = nullptr;

		if (SUCCEEDED (StringFromCLSID (GUID_WfpProvider, &guidString)))
		{
			if (RegQueryValueEx (hkey, guidString, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
				result = true;

			CoTaskMemFree (guidString);
		}

		RegCloseKey (hkey);
	}

	return result;
}

void _wfp_installfilters ()
{
	_wfp_destroyfilters (false); // destroy all installed filters first

	DWORD result = FwpmTransactionBegin (config.hengine, 0);

	if (result != ERROR_SUCCESS)
	{
		_app_logerror (L"FwpmTransactionBegin", result, nullptr);
	}
	else
	{
		const EnumMode mode = (EnumMode)app.ConfigGet (L"Mode", ModeWhitelist).AsUint ();

		FWPM_FILTER_CONDITION fwfc[6] = {0};

		// add loopback connections permission
		if (app.ConfigGet (L"AllowLoopbackConnections", false).AsBool ())
		{
			// match all loopback (localhost) data
			fwfc[0].fieldKey = FWPM_CONDITION_FLAGS;
			fwfc[0].matchType = FWP_MATCH_FLAGS_ANY_SET;
			fwfc[0].conditionValue.type = FWP_UINT32;
			fwfc[0].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;

			// tests if the network traffic is (non-)app container loopback traffic (win8+)
			if (_r_sys_validversion (6, 2))
				fwfc[0].conditionValue.uint32 |= (FWP_CONDITION_FLAG_IS_APPCONTAINER_LOOPBACK | FWP_CONDITION_FLAG_IS_NON_APPCONTAINER_LOOPBACK);

			_wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, FALSE, false);
			_wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, FALSE, false);

			// boot-time filters loopback permission
			if (app.ConfigGet (L"InstallBoottimeFilters", false).AsBool ())
			{
				_wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, FALSE, true);
				_wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, FALSE, true);
			}

			// ipv4/ipv6 loopback
			LPCWSTR ip_list[] = {L"10.0.0.0/8", L"172.16.0.0/12", L"169.254.0.0/16", L"192.168.0.0/16", L"224.0.0.0/24", L"fd00::/8", L"fe80::/10"};

			for (size_t i = 0; i < _countof (ip_list); i++)
			{
				if (WaitForSingleObjectEx (config.stop_evt, 0, FALSE) == WAIT_OBJECT_0)
				{
					FwpmTransactionAbort (config.hengine);
					return;
				}

				FWP_V4_ADDR_AND_MASK addr4 = {0};
				FWP_V6_ADDR_AND_MASK addr6 = {0};

				ITEM_ADDRESS addr;
				SecureZeroMemory (&addr, sizeof (addr));

				addr.paddr4 = &addr4;
				addr.paddr6 = &addr6;

				EnumRuleType rule_type = TypeIp;

				if (_app_parserulestring (ip_list[i], &addr, &rule_type))
				{
					//fwfc[1].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
					fwfc[1].matchType = FWP_MATCH_EQUAL;

					if (addr.format == NET_ADDRESS_IPV4)
					{
						fwfc[1].conditionValue.type = FWP_V4_ADDR_MASK;
						fwfc[1].conditionValue.v4AddrMask = &addr4;

						fwfc[1].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
						_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, FALSE, false);

						if (app.ConfigGet (L"InstallBoottimeFilters", false).AsBool ())
							_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, FALSE, true);

						fwfc[1].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
						_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, FALSE, false);

						if (app.ConfigGet (L"InstallBoottimeFilters", false).AsBool ())
							_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, FALSE, true);
					}
					else if (addr.format == NET_ADDRESS_IPV6)
					{
						fwfc[1].conditionValue.type = FWP_V6_ADDR_MASK;
						fwfc[1].conditionValue.v6AddrMask = &addr6;

						fwfc[1].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
						_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, FALSE, false);

						if (app.ConfigGet (L"InstallBoottimeFilters", false).AsBool ())
							_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, FALSE, true);

						fwfc[1].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
						_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, false);

						if (app.ConfigGet (L"InstallBoottimeFilters", false).AsBool ())
							_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, true);
					}
				}
			}
		}

		// apply apps rules
		{
			const bool is_block = (mode == ModeBlacklist) ? true : false;

			_r_fastlock_acquireshared (&lock_access);

			for (auto& p : apps)
			{
				if (WaitForSingleObjectEx (config.stop_evt, 0, FALSE) == WAIT_OBJECT_0)
				{
					_r_fastlock_releaseshared (&lock_access);

					FwpmTransactionAbort (config.hengine);
					return;
				}

				PITEM_APP ptr_app = &p.second;

				if (ptr_app)
				{
					if (ptr_app->is_enabled)
						ptr_app->is_haveerrors = !_wfp_createrulefilter (ptr_app->display_name, nullptr, ptr_app, FWP_DIRECTION_MAX, nullptr, 0, AF_UNSPEC, is_block, FILTER_WEIGHT_APPLICATION, false);
				}
			}

			_r_fastlock_releaseshared (&lock_access);
		}

		// apply system/custom/blocklist rules
		{
			std::vector<PITEM_RULE> const* ptr_rules[] = {
				&rules_system,
				&rules_custom,
				&rules_blocklist,
			};

			_r_fastlock_acquireshared (&lock_access);

			for (size_t i = 0; i < _countof (ptr_rules); i++)
			{
				if (!ptr_rules[i])
					continue;

				UINT8 weight;

				if (i == 0)
					weight = FILTER_WEIGHT_SYSTEM;

				else if (i == 2)
					weight = FILTER_WEIGHT_BLOCKLIST;

				else
					weight = FILTER_WEIGHT_CUSTOM;

				for (size_t j = 0; j < ptr_rules[i]->size (); j++)
				{
					if (WaitForSingleObjectEx (config.stop_evt, 0, FALSE) == WAIT_OBJECT_0)
					{
						FwpmTransactionAbort (config.hengine);
						return;
					}

					PITEM_RULE ptr_rule = ptr_rules[i]->at (j);

					if (ptr_rule && ptr_rule->is_enabled)
					{
						rstring::rvector rule_arr = rstring (ptr_rule->prule).AsVector (RULE_DELIMETER);

						for (size_t k = 0; k < rule_arr.size (); k++)
						{
							if (ptr_rule->apps.empty ())
							{
								// apply rule for all apps (global)
								ptr_rule->is_haveerrors = !_wfp_createrulefilter (ptr_rule->pname, rule_arr.at (k), nullptr, ptr_rule->dir, &ptr_rule->type, ptr_rule->protocol, ptr_rule->version, ptr_rule->is_block, weight, false);
							}
							else
							{
								// apply rule for specified apps (special)
								for (auto const &p : ptr_rule->apps)
								{
									PITEM_APP const ptr_app = _app_getapplication (p.first);

									if (ptr_app)
										ptr_rule->is_haveerrors = !_wfp_createrulefilter (ptr_rule->pname, rule_arr.at (k), ptr_app, ptr_rule->dir, &ptr_rule->type, ptr_rule->protocol, ptr_rule->version, ptr_rule->is_block, weight, false);
								}
							}
						}
					}
				}
			}

			_r_fastlock_releaseshared (&lock_access);
		}

		// firewall service rules
		// https://msdn.microsoft.com/en-us/library/gg462153.aspx
		if (app.ConfigGet (L"AllowIPv6", true).AsBool ())
		{
			// allows 6to4 tunneling, which enables ipv6 to run over an ipv4 network
			fwfc[0].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
			fwfc[0].matchType = FWP_MATCH_EQUAL;
			fwfc[0].conditionValue.type = FWP_UINT8;
			fwfc[0].conditionValue.uint8 = IPPROTO_IPV6; // ipv6 header

			_wfp_createfilter (L"Allow6to4", fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, FALSE, false);

			// allows icmpv6 router solicitation messages, which are required for the ipv6 stack to work properly
			fwfc[0].fieldKey = FWPM_CONDITION_ICMP_TYPE;
			fwfc[0].matchType = FWP_MATCH_EQUAL;
			fwfc[0].conditionValue.type = FWP_UINT16;
			fwfc[0].conditionValue.uint16 = 0x85;

			_wfp_createfilter (L"AllowIcmpV6Type133", fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, false);

			// allows icmpv6 router advertise messages, which are required for the ipv6 stack to work properly
			fwfc[0].conditionValue.uint16 = 0x86;
			_wfp_createfilter (L"AllowIcmpV6Type134", fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, false);

			// allows icmpv6 neighbor solicitation messages, which are required for the ipv6 stack to work properly
			fwfc[0].conditionValue.uint16 = 0x87;
			_wfp_createfilter (L"AllowIcmpV6Type135", fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, false);

			// allows icmpv6 neighbor advertise messages, which are required for the ipv6 stack to work properly
			fwfc[0].conditionValue.uint16 = 0x88;
			_wfp_createfilter (L"AllowIcmpV6Type136", fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, false);
		}

		// prevent port scanning using stealth discards and silent drops
		if (app.ConfigGet (L"UseStealthMode", false).AsBool ())
		{
			// blocks udp port scanners
			fwfc[0].fieldKey = FWPM_CONDITION_ICMP_TYPE;
			fwfc[0].matchType = FWP_MATCH_EQUAL;
			fwfc[0].conditionValue.type = FWP_UINT16;
			fwfc[0].conditionValue.uint16 = 0x03; // destination unreachable

			_wfp_createfilter (L"BlockIcmpErrorV4", fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, FWPM_LAYER_OUTBOUND_ICMP_ERROR_V4, nullptr, TRUE, false);
			_wfp_createfilter (L"BlockIcmpErrorV6", fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, FWPM_LAYER_OUTBOUND_ICMP_ERROR_V6, nullptr, TRUE, false);

			// blocks tcp port scanners (exclude loopback)
			fwfc[0].fieldKey = FWPM_CONDITION_FLAGS;
			fwfc[0].matchType = FWP_MATCH_FLAGS_NONE_SET;
			fwfc[0].conditionValue.type = FWP_UINT32;
			fwfc[0].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;

			// tests if the network traffic is (non-)app container loopback traffic (win8+)
			if (_r_sys_validversion (6, 2))
				fwfc[0].conditionValue.uint32 |= (FWP_CONDITION_FLAG_IS_APPCONTAINER_LOOPBACK | FWP_CONDITION_FLAG_IS_NON_APPCONTAINER_LOOPBACK);

			_wfp_createfilter (L"BlockTcpRstOnCloseV4", fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, FWPM_LAYER_INBOUND_TRANSPORT_V4_DISCARD, &FWPM_CALLOUT_WFP_TRANSPORT_LAYER_V4_SILENT_DROP, FWP_ACTION_CALLOUT_TERMINATING, false);
			_wfp_createfilter (L"BlockTcpRstOnCloseV6", fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, FWPM_LAYER_INBOUND_TRANSPORT_V6_DISCARD, &FWPM_CALLOUT_WFP_TRANSPORT_LAYER_V6_SILENT_DROP, FWP_ACTION_CALLOUT_TERMINATING, false);
		}

		// block all outbound traffic (only on "whitelist" mode)
		if (mode == ModeWhitelist)
		{
			_wfp_createfilter (L"BlockOutboundConnectionsV4", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, TRUE, false);
			_wfp_createfilter (L"BlockOutboundConnectionsV6", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, TRUE, false);

			// proxy connections (win8+)
			if (app.ConfigGet (L"EnableProxySupport", false).AsBool () && _r_sys_validversion (6, 2))
			{
				fwfc[0].fieldKey = FWPM_CONDITION_FLAGS;
				fwfc[0].matchType = FWP_MATCH_FLAGS_ANY_SET;
				fwfc[0].conditionValue.type = FWP_UINT32;
				fwfc[0].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_CONNECTION_REDIRECTED | FWP_CONDITION_FLAG_IS_PROXY_CONNECTION;

				_wfp_createfilter (L"BlockOutboundConnectionsV4 (proxy)", fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, TRUE, false);
				_wfp_createfilter (L"BlockOutboundConnectionsV6 (proxy)", fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, TRUE, false);
			}
		}
		else
		{
			_wfp_createfilter (L"AllowOutboundConnectionsV4", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, FALSE, false);
			_wfp_createfilter (L"AllowOutboundConnectionsV6", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, FALSE, false);
		}

		// block all inbound traffic (only on "stealth" mode)
		if (mode == ModeWhitelist && (app.ConfigGet (L"UseStealthMode", false).AsBool () || !app.ConfigGet (L"AllowInboundConnections", false).AsBool ()))
		{
			_wfp_createfilter (L"BlockInboundConnectionsV4", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, TRUE, false);
			_wfp_createfilter (L"BlockInboundConnectionsV6", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, TRUE, false);
		}

		// block all listen traffic (NOT RECOMMENDED!!!!)
		// issue: https://github.com/henrypp/simplewall/issues/9
		if (mode == ModeWhitelist && !app.ConfigGet (L"AllowListenConnections2", true).AsBool ())
		{
			_wfp_createfilter (L"BlockListenConnectionsV4", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_LISTEN_V4, nullptr, TRUE, false);
			_wfp_createfilter (L"BlockListenConnectionsV6", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_LISTEN_V6, nullptr, TRUE, false);
		}

		// install boot-time filters (enforced at boot-time, even before "base filtering engine" service starts)
		if (app.ConfigGet (L"InstallBoottimeFilters", false).AsBool ())
		{
			_wfp_createfilter (L"BlockOutboundConnectionsV4 [boot-time]", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, TRUE, true);
			_wfp_createfilter (L"BlockOutboundConnectionsV6 [boot-time]", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, TRUE, true);

			_wfp_createfilter (L"BlockInboundConnectionsV4 [boot-time]", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, TRUE, true);
			_wfp_createfilter (L"BlockInboundConnectionsV6 [boot-time]", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, TRUE, true);

			{
				fwfc[0].fieldKey = FWPM_CONDITION_FLAGS;
				fwfc[0].matchType = FWP_MATCH_FLAGS_NONE_SET;
				fwfc[0].conditionValue.type = FWP_UINT32;
				fwfc[0].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;

				_wfp_createfilter (L"BlockIcmpConnectionsV4 [boot-time]", fwfc, 1, FILTER_WEIGHT_LOWEST, FWPM_LAYER_INBOUND_ICMP_ERROR_V4, nullptr, TRUE, true);
				_wfp_createfilter (L"BlockIcmpConnectionsV6 [boot-time]", fwfc, 1, FILTER_WEIGHT_LOWEST, FWPM_LAYER_INBOUND_ICMP_ERROR_V6, nullptr, TRUE, true);
			}

			// win7+
			if (_r_sys_validversion (6, 1))
			{
				fwfc[0].fieldKey = FWPM_CONDITION_FLAGS;
				fwfc[0].matchType = FWP_MATCH_FLAGS_NONE_SET;
				fwfc[0].conditionValue.type = FWP_UINT32;
				fwfc[0].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_OUTBOUND_PASS_THRU | FWP_CONDITION_FLAG_IS_INBOUND_PASS_THRU;

				_wfp_createfilter (L"BlockIpforwardConnectionsV4 [boot-time]", fwfc, 1, FILTER_WEIGHT_LOWEST, FWPM_LAYER_IPFORWARD_V4, nullptr, TRUE, true);
				_wfp_createfilter (L"BlockIpforwardConnectionsV6 [boot-time]", fwfc, 1, FILTER_WEIGHT_LOWEST, FWPM_LAYER_IPFORWARD_V6, nullptr, TRUE, true);
			}


		}

		FwpmTransactionCommit (config.hengine);

		// set icons
		app.SetIcon (app.GetHWND (), IDI_ACTIVE, true);
		app.TraySetInfo (UID, _r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_ACTIVE), GetSystemMetrics (SM_CXSMICON)), APP_NAME);

		SetDlgItemText (app.GetHWND (), IDC_START_BTN, app.LocaleString (IDS_TRAY_STOP, nullptr));

		// dropped packets logging (win7+)
		if (_r_sys_validversion (6, 1))
			_wfp_logsubscribe ();
	}
}

bool _app_installfilters (bool is_forced)
{
	if (is_forced || _wfp_isfiltersinstalled ())
	{
		_r_ctrl_enable (app.GetHWND (), IDC_START_BTN, false);

		if (_r_fastlock_islocked (&lock_apply) || config.hthread)
		{
			SetEvent (config.stop_evt);
			WaitForSingleObjectEx (config.finish_evt, 8000, FALSE);

			if (config.hthread)
			{
				CloseHandle (config.hthread);
				config.hthread = nullptr;
			}
		}

		config.hthread = (HANDLE)_beginthreadex (nullptr, 0, &ApplyThread, (LPVOID)true, 0, nullptr);

		return true;
	}
	else
	{
		_app_listviewsort (app.GetHWND (), IDC_LISTVIEW, -1, false);
		_app_profilesave (app.GetHWND ());

		_r_listview_redraw (app.GetHWND (), IDC_LISTVIEW);
	}

	return false;
}

bool _app_uninstallfilters ()
{
	_r_ctrl_enable (app.GetHWND (), IDC_START_BTN, false);

	if (_r_fastlock_islocked (&lock_apply) || config.hthread)
	{
		SetEvent (config.stop_evt);
		WaitForSingleObjectEx (config.finish_evt, 8000, FALSE);
	}

	config.hthread = (HANDLE)_beginthreadex (nullptr, 0, &ApplyThread, (LPVOID)false, 0, nullptr);

	return true;
}

bool _app_logchecklimit ()
{
	const size_t limit = app.ConfigGet (L"LogSizeLimitKb", 256).AsUint ();

	if (!limit || !config.hlog || config.hlog == INVALID_HANDLE_VALUE)
		return false;

	if (_r_fs_size (config.hlog) >= (limit * _R_BYTESIZE_KB))
	{
		//// make backup before log truncate
		//if (app.ConfigGet (L"IsLogBackup", true).AsBool ())
		//{
		//	const rstring path = _r_path_expand (app.ConfigGet (L"LogPath", PATH_LOG));

		//	_r_fs_delete (path + L".bak");
		//	_r_fs_copy (path, path + L".bak");
		//}

		SetFilePointer (config.hlog, 2, nullptr, FILE_BEGIN);
		SetEndOfFile (config.hlog);

		return true;
	}

	return false;
}

bool _app_loginit (bool is_install)
{
	// dropped packets logging (win7+)
	if (!_r_sys_validversion (6, 1))
		return false;

	// reset all handles
	_r_fastlock_acquireexclusive (&lock_writelog);

	if (config.hlog != nullptr && config.hlog != INVALID_HANDLE_VALUE)
	{
		CloseHandle (config.hlog);
		config.hlog = nullptr;
	}

	_r_fastlock_releaseexclusive (&lock_writelog);

	if (!is_install)
		return true; // already closed

	// check if log enabled
	if (!app.ConfigGet (L"IsLogEnabled", false).AsBool () || !_wfp_isfiltersinstalled ())
		return false;

	bool result = false;

	if (is_install)
	{
		const rstring path = _r_path_expand (app.ConfigGet (L"LogPath", PATH_LOG));

		_r_fastlock_acquireexclusive (&lock_writelog);

		config.hlog = CreateFile (path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);

		if (config.hlog == INVALID_HANDLE_VALUE)
		{
			_app_logerror (L"CreateFile", GetLastError (), path);
		}
		else
		{
			if (GetLastError () != ERROR_ALREADY_EXISTS)
			{
				DWORD written = 0;
				static const BYTE bom[] = {0xFF, 0xFE};

				WriteFile (config.hlog, bom, sizeof (bom), &written, nullptr); // write utf-16 le byte order mask
			}
			else
			{
				_app_logchecklimit ();

				SetFilePointer (config.hlog, 0, nullptr, FILE_END);
			}

			result = true;
		}

		_r_fastlock_releaseexclusive (&lock_writelog);
	}

	return result;
}

bool _app_formataddress (LPWSTR dest, size_t cchDest, PITEM_LOG const ptr_log, FWP_DIRECTION dir, UINT16 port, bool is_appenddns)
{
	if (!dest || !cchDest || !ptr_log)
		return false;

	bool result = false;

	PIN_ADDR addrv4 = nullptr;
	PIN6_ADDR addrv6 = nullptr;

	if (ptr_log->af == AF_INET)
	{
		addrv4 = (dir == FWP_DIRECTION_OUTBOUND) ? &ptr_log->remote_addr : &ptr_log->local_addr;

		InetNtop (ptr_log->af, addrv4, dest, cchDest);

		result = !IN4_IS_ADDR_UNSPECIFIED (addrv4);
	}
	else if (ptr_log->af == AF_INET6)
	{
		addrv6 = (dir == FWP_DIRECTION_OUTBOUND) ? &ptr_log->remote_addr6 : &ptr_log->local_addr6;

		InetNtop (ptr_log->af, addrv6, dest, cchDest);

		result = !IN6_IS_ADDR_UNSPECIFIED (addrv6);
	}

	if (port)
		StringCchCat (dest, cchDest, _r_fmt (L":%d", port));

	if (result && is_appenddns && app.ConfigGet (L"IsNetworkResolutionsEnabled", false).AsBool () && config.is_wsainit && _app_canihaveaccess ())
	{
		WCHAR hostBuff[NI_MAXHOST] = {0};

		if (_app_resolveaddress (ptr_log->af, (ptr_log->af == AF_INET) ? (LPVOID)addrv4 : (LPVOID)addrv6, hostBuff, _countof (hostBuff)))
			StringCchCat (dest, cchDest, _r_fmt (L" (%s)", hostBuff));
	}

	return result;
}

rstring _app_proto2name (UINT8 proto)
{
	for (size_t i = 0; i < protocols.size (); i++)
	{
		if (proto == protocols.at (i).id)
		{
			return protocols.at (i).name;
		}
	}

	return NA_TEXT;
}

void _app_logwrite (PITEM_LOG const ptr_log)
{
	if (!ptr_log)
		return;

	if (config.hlog != nullptr && config.hlog != INVALID_HANDLE_VALUE)
	{
		_r_fastlock_acquireexclusive (&lock_writelog);

		_app_logchecklimit ();

		// parse path
		rstring path;
		{
			_r_fastlock_acquireshared (&lock_access);

			PITEM_APP const ptr_app = _app_getapplication (ptr_log->hash);

			if (ptr_app)
			{
				if (ptr_app->type == AppStore || ptr_app->type == AppService)
				{
					if (ptr_app->real_path[0])
						path = ptr_app->real_path;

					else
						path = ptr_app->display_name;
				}
				else
				{
					path = ptr_app->original_path;
				}
			}
			else
			{
				path = NA_TEXT;
			}

			_r_fastlock_releaseshared (&lock_access);
		}

		// parse filter name
		rstring filter;
		{
			if (ptr_log->provider_name[0])
				filter.Format (L"%s\\%s", ptr_log->provider_name, ptr_log->filter_name);

			else
				filter = ptr_log->filter_name;
		}

		// parse direction
		rstring direction;
		{
			if (ptr_log->direction == FWP_DIRECTION_INBOUND)
				direction = L"IN";

			else if (ptr_log->direction == FWP_DIRECTION_OUTBOUND)
				direction = L"OUT";

			else
				direction = NA_TEXT;

			if (ptr_log->is_loopback)
				direction.Append (L"-Loopback");
		}

		rstring buffer;
		buffer.Format (L"[%s] %s (%s) [Remote: %s] [Local: %s] (%s) %s [%s]\r\n", _r_fmt_date (ptr_log->date, FDTF_SHORTDATE | FDTF_LONGTIME).GetString (), path.GetString (), ptr_log->username, ptr_log->remote_fmt, ptr_log->local_fmt, _app_proto2name (ptr_log->protocol).GetString (), filter.GetString (), direction.GetString ());

		DWORD written = 0;
		WriteFile (config.hlog, buffer.GetString (), DWORD (buffer.GetLength () * sizeof (WCHAR)), &written, nullptr);

		_r_fastlock_releaseexclusive (&lock_writelog);
	}
}

void CALLBACK _app_timer_callback (PVOID lparam, BOOLEAN /*TimerOrWaitFired*/)
{
	HWND hwnd = (HWND)lparam;

	if (_app_timer_apply (hwnd, false))
	{
		_app_profilesave (nullptr);
		_app_installfilters (false);
	}
}

bool _app_timer_apply (HWND hwnd, bool is_forceremove)
{
	bool is_changed = false;
	const time_t current_time = _r_unixtime_now ();
	std::vector<size_t> idx_array;

	_r_fastlock_acquireexclusive (&lock_access);

	if (config.timer_low || config.htimer)
	{
		DeleteTimerQueueTimer (nullptr, config.htimer, nullptr);
		config.htimer = nullptr;
		config.timer_low = 0;
	}

	for (auto const &p : apps_timer)
	{
		if (!config.timer_low || config.timer_low > p.second)
			config.timer_low = p.second;

		const size_t hash = p.first;
		const PITEM_APP ptr_app = _app_getapplication (hash);

		if (ptr_app)
		{
			const bool is_enable = !is_forceremove && (p.second && p.second > current_time);

			if (!is_enable)
				idx_array.push_back (hash);

			if (is_enable != ptr_app->is_enabled)
			{
				ptr_app->is_enabled = is_enable;

				const size_t item = _app_getposition (hwnd, hash);

				config.is_nocheckboxnotify = true;

				_r_listview_setitem (hwnd, IDC_LISTVIEW, item, 0, nullptr, LAST_VALUE, ptr_app->is_enabled ? 0 : 1);
				_r_listview_setitemcheck (hwnd, IDC_LISTVIEW, item, ptr_app->is_enabled);

				config.is_nocheckboxnotify = false;

				if (!is_changed)
					is_changed = true;
			}
		}
	}

	for (size_t i = 0; i < idx_array.size (); i++)
		apps_timer.erase (idx_array.at (i));

	_r_fastlock_releaseexclusive (&lock_access);

	if (config.htimer)
	{
		DeleteTimerQueueTimer (nullptr, config.htimer, nullptr);
		config.htimer = nullptr;
	}

	if (is_forceremove)
	{
		config.timer_low = 0;
	}
	else
	{
		if (config.timer_low)
			CreateTimerQueueTimer (&config.htimer, nullptr, &_app_timer_callback, hwnd, DWORD (config.timer_low - current_time) * _R_SECONDSCLOCK_MSEC, 0, WT_EXECUTEONLYONCE);
	}

	return is_changed;
}

void _app_notifysetpos (HWND hwnd)
{
	RECT windowRect = {0};
	GetWindowRect (hwnd, &windowRect);

	RECT desktopRect = {0};
	SystemParametersInfo (SPI_GETWORKAREA, 0, &desktopRect, 0);

	APPBARDATA appbar = {0};
	appbar.cbSize = sizeof (appbar);
	appbar.hWnd = FindWindow (L"Shell_TrayWnd", nullptr);

	SHAppBarMessage (ABM_GETTASKBARPOS, &appbar);

	const UINT border_x = GetSystemMetrics (SM_CXBORDER) * 2;
	const UINT border_y = GetSystemMetrics (SM_CYBORDER) * 2;

	if (appbar.uEdge == ABE_LEFT)
	{
		windowRect.left = appbar.rc.right + border_x;
		windowRect.top = (desktopRect.bottom - (windowRect.bottom - windowRect.top)) - border_y;
	}
	else if (appbar.uEdge == ABE_TOP)
	{
		windowRect.left = (desktopRect.right - (windowRect.right - windowRect.left)) - border_x;
		windowRect.top = appbar.rc.bottom + border_y;
	}
	else if (appbar.uEdge == ABE_RIGHT)
	{
		windowRect.left = (desktopRect.right - (windowRect.right - windowRect.left)) - border_x;
		windowRect.top = (desktopRect.bottom - (windowRect.bottom - windowRect.top)) - border_y;
	}
	else/* if (appbar.uEdge == ABE_BOTTOM)*/
	{
		windowRect.left = (desktopRect.right - (windowRect.right - windowRect.left)) - border_x;
		windowRect.top = (desktopRect.bottom - (windowRect.bottom - windowRect.top)) - border_y;
	}

	SetWindowPos (hwnd, nullptr, windowRect.left, windowRect.top, 0, 0, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOCOPYBITS);
}

void _app_notifyhide (HWND hwnd)
{
	_app_notifysettimeout (hwnd, 0, false, 0);

	ShowWindow (hwnd, SW_HIDE);
}

void _app_notifycreatewindow ()
{
	WNDCLASSEX wcex = {0};

	wcex.cbSize = sizeof (wcex);
	wcex.style = CS_VREDRAW | CS_HREDRAW;
	wcex.lpfnWndProc = &NotificationProc;
	wcex.hInstance = app.GetHINSTANCE ();
	wcex.hCursor = LoadCursor (nullptr, IDC_ARROW);
	wcex.lpszClassName = NOTIFY_CLASS_DLG;
	wcex.hbrBackground = GetSysColorBrush (COLOR_WINDOW);

	if (RegisterClassEx (&wcex))
	{
		const UINT wnd_width = app.GetDPI (NOTIFY_WIDTH);
		const UINT wnd_height = app.GetDPI (NOTIFY_HEIGHT);
		const UINT btn_width = app.GetDPI (NOTIFY_BTN_WIDTH);

		const INT cxsmIcon = GetSystemMetrics (SM_CXSMICON);
		const INT cysmIcon = GetSystemMetrics (SM_CYSMICON);
		const INT IconSize = cxsmIcon + (cxsmIcon / 2);

		config.hnotification = CreateWindowEx (WS_EX_TOOLWINDOW | WS_EX_TOPMOST, NOTIFY_CLASS_DLG, nullptr, WS_POPUP, 0, 0, wnd_width, wnd_height, nullptr, nullptr, wcex.hInstance, nullptr);

		if (config.hnotification)
		{
			HFONT hfont_title = nullptr;
			HFONT hfont_text = nullptr;

			{
				NONCLIENTMETRICS ncm = {0};
				ncm.cbSize = sizeof (ncm);

				if (SystemParametersInfo (SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0))
				{
					PLOGFONT lf_title = &ncm.lfCaptionFont;
					PLOGFONT lf_text = &ncm.lfMessageFont;

					lf_title->lfHeight = _r_dc_fontsizetoheight (10);
					lf_text->lfHeight = _r_dc_fontsizetoheight (9);

					lf_title->lfWeight = FW_SEMIBOLD;
					lf_text->lfWeight = FW_NORMAL;

					// set default values
					lf_title->lfQuality = CLEARTYPE_QUALITY;
					lf_text->lfQuality = CLEARTYPE_QUALITY;

					StringCchCopy (lf_title->lfFaceName, LF_FACESIZE, UI_FONT);
					StringCchCopy (lf_text->lfFaceName, LF_FACESIZE, UI_FONT);

					hfont_title = CreateFontIndirect (lf_title);
					hfont_text = CreateFontIndirect (lf_text);
				}
			}

			HWND hwnd = CreateWindow (WC_STATIC, APP_NAME, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_WORDELLIPSIS, IconSize + app.GetDPI (12), app.GetDPI (8), wnd_width - app.GetDPI (64 + 12 + 10 + 24), IconSize, config.hnotification, (HMENU)IDC_TITLE_ID, nullptr, nullptr);
			SendMessage (hwnd, WM_SETFONT, (LPARAM)hfont_title, true);

			hwnd = CreateWindow (WC_STATIC, nullptr, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_CENTER | SS_ICON | SS_NOTIFY, app.GetDPI (8), app.GetDPI (8), IconSize, IconSize, config.hnotification, (HMENU)IDC_ICON_ID, nullptr, nullptr);

			hwnd = CreateWindow (WC_STATIC, nullptr, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_CENTER | SS_ICON | SS_NOTIFY, wnd_width - app.GetDPI (48) - app.GetDPI (12), app.GetDPI (8), IconSize, IconSize, config.hnotification, (HMENU)IDC_MUTE_BTN, nullptr, nullptr);
			SendMessage (hwnd, STM_SETIMAGE, IMAGE_ICON, (WPARAM)_r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_MUTE), cxsmIcon));

			hwnd = CreateWindow (WC_STATIC, nullptr, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_CENTER | SS_ICON | SS_NOTIFY, wnd_width - app.GetDPI (20) - app.GetDPI (12), app.GetDPI (8), IconSize, IconSize, config.hnotification, (HMENU)IDC_CLOSE_BTN, nullptr, nullptr);
			SendMessage (hwnd, STM_SETIMAGE, IMAGE_ICON, (WPARAM)_r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_CLOSE), cxsmIcon));

			hwnd = CreateWindow (WC_LINK, nullptr, WS_CHILD | WS_VISIBLE, app.GetDPI (12), app.GetDPI (38), wnd_width - app.GetDPI (24), app.GetDPI (16), config.hnotification, (HMENU)IDC_FILE_ID, nullptr, nullptr);
			SendMessage (hwnd, WM_SETFONT, (LPARAM)hfont_text, true);

			hwnd = CreateWindow (WC_EDIT, nullptr, WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL | ES_MULTILINE, app.GetDPI (12), app.GetDPI (56), wnd_width - app.GetDPI (24), app.GetDPI (16), config.hnotification, (HMENU)IDC_ADDRESS_REMOTE_ID, nullptr, nullptr);
			SendMessage (hwnd, WM_SETFONT, (LPARAM)hfont_text, true);
			SendMessage (hwnd, EM_SETMARGINS, EC_LEFTMARGIN, 0);
			SendMessage (hwnd, EM_SETMARGINS, EC_RIGHTMARGIN, 0);

			hwnd = CreateWindow (WC_EDIT, nullptr, WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL | ES_MULTILINE, app.GetDPI (12), app.GetDPI (74), wnd_width - app.GetDPI (24), app.GetDPI (16), config.hnotification, (HMENU)IDC_ADDRESS_LOCAL_ID, nullptr, nullptr);
			SendMessage (hwnd, WM_SETFONT, (LPARAM)hfont_text, true);
			SendMessage (hwnd, EM_SETMARGINS, EC_LEFTMARGIN, 0);
			SendMessage (hwnd, EM_SETMARGINS, EC_RIGHTMARGIN, 0);

			hwnd = CreateWindow (WC_EDIT, nullptr, WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL | ES_MULTILINE, app.GetDPI (12), app.GetDPI (92), wnd_width - app.GetDPI (24), app.GetDPI (16), config.hnotification, (HMENU)IDC_FILTER_ID, nullptr, nullptr);
			SendMessage (hwnd, WM_SETFONT, (LPARAM)hfont_text, true);
			SendMessage (hwnd, EM_SETMARGINS, EC_LEFTMARGIN, 0);
			SendMessage (hwnd, EM_SETMARGINS, EC_RIGHTMARGIN, 0);

			hwnd = CreateWindow (WC_EDIT, nullptr, WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL | ES_MULTILINE, app.GetDPI (12), app.GetDPI (110), wnd_width - app.GetDPI (24), app.GetDPI (16), config.hnotification, (HMENU)IDC_DATE_ID, nullptr, nullptr);
			SendMessage (hwnd, WM_SETFONT, (LPARAM)hfont_text, true);
			SendMessage (hwnd, EM_SETMARGINS, EC_LEFTMARGIN, 0);
			SendMessage (hwnd, EM_SETMARGINS, EC_RIGHTMARGIN, 0);

			hwnd = CreateWindow (WC_BUTTON, nullptr, WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_CHECKBOX | BS_NOTIFY, app.GetDPI (12), app.GetDPI (138), wnd_width - app.GetDPI (24), app.GetDPI (16), config.hnotification, (HMENU)IDC_CREATERULE_ADDR_ID, nullptr, nullptr);
			SendMessage (hwnd, WM_SETFONT, (LPARAM)hfont_text, true);

			hwnd = CreateWindow (WC_BUTTON, nullptr, WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_CHECKBOX | BS_NOTIFY, app.GetDPI (12), app.GetDPI (156), wnd_width - app.GetDPI (24), app.GetDPI (16), config.hnotification, (HMENU)IDC_CREATERULE_PORT_ID, nullptr, nullptr);
			SendMessage (hwnd, WM_SETFONT, (LPARAM)hfont_text, true);

			hwnd = CreateWindow (WC_COMBOBOX, nullptr, WS_TABSTOP | WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS, app.GetDPI (10), wnd_height - app.GetDPI (36), btn_width - app.GetDPI (12), app.GetDPI (24), config.hnotification, (HMENU)IDC_TIMER_CB, nullptr, nullptr);
			SendMessage (hwnd, WM_SETFONT, (LPARAM)hfont_text, true);

			hwnd = CreateWindow (WC_BUTTON, nullptr, WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, wnd_width - ((btn_width + app.GetDPI (8)) * 2), wnd_height - app.GetDPI (36), btn_width, app.GetDPI (24), config.hnotification, (HMENU)IDC_ALLOW_BTN, nullptr, nullptr);
			SendMessage (hwnd, WM_SETFONT, (LPARAM)hfont_text, true);
			SendMessage (hwnd, BM_SETIMAGE, IMAGE_ICON, (WPARAM)_r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_ALLOW), cxsmIcon));

			hwnd = CreateWindow (WC_BUTTON, nullptr, WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, wnd_width - btn_width - app.GetDPI (10), wnd_height - app.GetDPI (36), btn_width, app.GetDPI (24), config.hnotification, (HMENU)IDC_IGNORE_BTN, nullptr, nullptr);
			SendMessage (hwnd, WM_SETFONT, (LPARAM)hfont_text, true);
			//SendMessage (hwnd, BM_SETIMAGE, IMAGE_ICON, (WPARAM)_r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_BLOCK), cxsmIcon));

			{
				RECT rc2 = {0};
				rc2.left = rc2.right = app.GetDPI (4);

				SendDlgItemMessage (config.hnotification, IDC_ALLOW_BTN, BCM_SETTEXTMARGIN, 0, (LPARAM)&rc2);
				//SendDlgItemMessage (config.hnotification, IDC_IGNORE_BTN, BCM_SETTEXTMARGIN, 0, (LPARAM)&rc2);
			}

			_r_ctrl_settip (config.hnotification, IDC_MUTE_BTN, LPSTR_TEXTCALLBACK);
			_r_ctrl_settip (config.hnotification, IDC_CLOSE_BTN, LPSTR_TEXTCALLBACK);

			_r_ctrl_settip (config.hnotification, IDC_FILE_ID, LPSTR_TEXTCALLBACK);
			_r_ctrl_settip (config.hnotification, IDC_ADDRESS_REMOTE_ID, LPSTR_TEXTCALLBACK);
			_r_ctrl_settip (config.hnotification, IDC_ADDRESS_LOCAL_ID, LPSTR_TEXTCALLBACK);
			_r_ctrl_settip (config.hnotification, IDC_FILTER_ID, LPSTR_TEXTCALLBACK);
			_r_ctrl_settip (config.hnotification, IDC_DATE_ID, LPSTR_TEXTCALLBACK);

			_r_ctrl_settip (config.hnotification, IDC_CREATERULE_ADDR_ID, LPSTR_TEXTCALLBACK);
			_r_ctrl_settip (config.hnotification, IDC_CREATERULE_PORT_ID, LPSTR_TEXTCALLBACK);

			//_app_notifysettimeout (config.hnotification, NOTIFY_TIMER_MOUSELEAVE_ID, true, 250);

			_app_notifyhide (config.hnotification);
		}
	}
}

size_t _app_notifygetcurrent ()
{
	size_t new_idx = LAST_VALUE;
	const size_t count = notifications.size ();

	if (count)
	{
		if (count == 1)
		{
			new_idx = 0;
		}
		else
		{
			const size_t idx = (size_t)GetWindowLongPtr (config.hnotification, GWLP_USERDATA);
			new_idx = max (0, min (idx, count - 1));
		}
	}

	SetWindowLongPtr (config.hnotification, GWLP_USERDATA, new_idx);

	return new_idx;
}

bool _app_notifycommand (HWND hwnd, EnumNotifyCommand command)
{
	_r_fastlock_acquireexclusive (&lock_notification);

	const size_t idx = _app_notifygetcurrent ();

	if (idx != LAST_VALUE)
	{
		PITEM_LOG ptr_log = notifications.at (idx);

		if (ptr_log)
		{
			_r_fastlock_acquireexclusive (&lock_access);

			const size_t hash = ptr_log->hash;
			const size_t item = _app_getposition (app.GetHWND (), hash);
			const time_t current_time = _r_unixtime_now ();

			PITEM_APP ptr_app = _app_getapplication (hash);

			if (ptr_app)
			{
				if (command == CmdAllow)
				{
					const bool is_createaddrrule = (IsDlgButtonChecked (hwnd, IDC_CREATERULE_ADDR_ID) == BST_CHECKED);
					const bool is_createportrule = (IsDlgButtonChecked (hwnd, IDC_CREATERULE_PORT_ID) == BST_CHECKED) && ptr_log->remote_port;

					// just create rule
					if (is_createaddrrule || is_createportrule)
					{
						WCHAR rule[128] = {0};

						if (is_createaddrrule)
							_app_formataddress (rule, _countof (rule), ptr_log, FWP_DIRECTION_OUTBOUND, 0, false);

						if (is_createportrule)
						{
							if (is_createaddrrule)
								StringCchCat (rule, _countof (rule), L":");

							StringCchCat (rule, _countof (rule), _r_fmt (L"%d", ptr_log->remote_port));
						}

						size_t rule_id = LAST_VALUE;

						const size_t rule_length = min (wcslen (rule), RULE_RULE_CCH_MAX) + 1;

						for (size_t i = 0; i < rules_custom.size (); i++)
						{
							PITEM_RULE rule_ptr = rules_custom.at (i);

							if (rule_ptr)
							{
								if (!rule_ptr->is_block && rule_ptr->prule && _wcsnicmp (rule_ptr->prule, rule, rule_length) == 0)
								{
									rule_id = i;
									break;
								}
							}
						}

						if (rule_id == LAST_VALUE)
						{
							// create rule
							PITEM_RULE ptr_rule = new ITEM_RULE;

							if (ptr_rule)
							{
								const size_t name_length = min (wcslen (rule), (size_t)RULE_NAME_CCH_MAX) + 1;

								ptr_rule->pname = new WCHAR[name_length];

								if (ptr_rule->pname)
									StringCchCopy (ptr_rule->pname, name_length, rule);

								ptr_rule->prule = new WCHAR[rule_length];

								if (ptr_rule->prule)
									StringCchCopy (ptr_rule->prule, rule_length, rule);

								ptr_rule->dir = ptr_log->direction;
								ptr_rule->is_block = false;
								ptr_rule->is_enabled = true;

								rules_custom.push_back (ptr_rule);
								rule_id = rules_custom.size () - 1;
							}
						}
						else
						{
							// modify rule
							rules_custom.at (rule_id)->is_enabled = true;
						}

						// add rule for app
						if (rule_id != LAST_VALUE)
							rules_custom.at (rule_id)->apps[hash] = true;
					}
					else
					{
						ptr_app->is_enabled = true;

						config.is_nocheckboxnotify = true;

						_r_listview_setitem (app.GetHWND (), IDC_LISTVIEW, item, 0, nullptr, LAST_VALUE, ptr_app->is_enabled ? 0 : 1);
						_r_listview_setitemcheck (app.GetHWND (), IDC_LISTVIEW, item, ptr_app->is_enabled);

						config.is_nocheckboxnotify = false;
					}

					// create rule timer
					{
						const UINT timer_cb = (UINT)SendDlgItemMessage (hwnd, IDC_TIMER_CB, CB_GETCURSEL, 0, 0);

						if (timer_cb >= 1)
						{
							const size_t timer_idx = timer_cb - 1;

							apps_timer[hash] = current_time + timers.at (timer_idx);

							_app_timer_apply (app.GetHWND (), false);
						}
					}
				}
				else if (command == CmdMute)
				{
					ptr_app->is_silent = true;
				}
				//else if (command == CmdIgnore)
				//{
				//	no changes ;)
				//}

				_r_fastlock_releaseexclusive (&lock_access);

				notifications_last[hash] = current_time;
				_app_freenotify (LAST_VALUE, hash);

				_r_fastlock_releaseexclusive (&lock_notification);

				_app_notifyrefresh ();

				// save and apply rules
				if (command != CmdIgnore)
				{
					_app_profilesave (app.GetHWND ());

					if (command == CmdAllow)
						_app_installfilters (false);

					else
						_r_listview_redraw (app.GetHWND (), IDC_LISTVIEW);
				}

				return true;
			}

			_r_fastlock_releaseexclusive (&lock_access);
		}
	}

	_r_fastlock_releaseexclusive (&lock_notification);

	return false;
}

bool _app_notifysettimeout (HWND hwnd, UINT_PTR timer_id, bool is_create, UINT timeout)
{
	if (is_create)
	{
		if (!hwnd || !timer_id)
			return false;

		if (timer_id == NOTIFY_TIMER_TIMEOUT_ID)
			config.is_notifytimeout = true;

		SetTimer (hwnd, timer_id, timeout, nullptr);
	}
	else
	{
		if (!timer_id || timer_id == NOTIFY_TIMER_TIMEOUT_ID)
			config.is_notifytimeout = false;

		if (timer_id)
		{
			KillTimer (hwnd, timer_id);
		}
		else
		{
			KillTimer (hwnd, NOTIFY_TIMER_POPUP_ID);
			KillTimer (hwnd, NOTIFY_TIMER_TIMEOUT_ID);
		}
	}

	return true;
}

bool _app_notifyshow (size_t idx, bool is_forced)
{
	if (!is_forced)
	{
		QUERY_USER_NOTIFICATION_STATE state = QUNS_NOT_PRESENT;

		if (SUCCEEDED (SHQueryUserNotificationState (&state)) && state != QUNS_ACCEPTS_NOTIFICATIONS)
			return false;
	}

	_r_fastlock_acquireshared (&lock_notification);

	if (!app.ConfigGet (L"IsNotificationsEnabled", true).AsBool () || notifications.empty () || idx == LAST_VALUE)
	{
		if (notifications.empty () || idx == LAST_VALUE)
			SetWindowLongPtr (config.hnotification, GWLP_USERDATA, LAST_VALUE);

		_r_fastlock_releaseshared (&lock_notification);

		return false;
	}

	const size_t total_size = notifications.size ();

	idx = (total_size == 1) ? 0 : max (0, min (idx, total_size - 1));

	PITEM_LOG const ptr_log = notifications.at (idx);

	if (ptr_log)
	{
		_r_fastlock_acquireshared (&lock_access);

		PITEM_APP const ptr_app = _app_getapplication (ptr_log->hash);

		if (ptr_app)
		{
			SetWindowLongPtr (config.hnotification, GWLP_USERDATA, idx);

			SendDlgItemMessage (config.hnotification, IDC_ICON_ID, STM_SETIMAGE, IMAGE_ICON, (WPARAM)(app.GetHICON (false)));

			rstring is_signed;

			if (app.ConfigGet (L"IsCerificatesEnabled", false).AsBool ())
				is_signed.Format (L" [%s]", app.LocaleString (ptr_app->is_signed ? IDS_SIGN_SIGNED : IDS_SIGN_UNSIGNED, nullptr).GetString ());

			_r_ctrl_settext (config.hnotification, IDC_FILE_ID, L"%s: <a href=\"#\">%s</a>%s", app.LocaleString (IDS_FILE, nullptr).GetString (), _r_path_extractfile (ptr_app->display_name).GetString (), is_signed.GetString ());

			_r_ctrl_settext (config.hnotification, IDC_ADDRESS_REMOTE_ID, L"%s: %s [Remote]", app.LocaleString (IDS_ADDRESS, nullptr).GetString (), ptr_log->remote_fmt);
			_r_ctrl_settext (config.hnotification, IDC_ADDRESS_LOCAL_ID, L"%s: %s [Local]", app.LocaleString (IDS_ADDRESS, nullptr).GetString (), ptr_log->local_fmt);

			_r_ctrl_settext (config.hnotification, IDC_FILTER_ID, L"%s: %s", app.LocaleString (IDS_FILTER, nullptr).GetString (), ptr_log->filter_name);
			_r_ctrl_settext (config.hnotification, IDC_DATE_ID, L"%s: %s", app.LocaleString (IDS_DATE, nullptr).GetString (), _r_fmt_date (ptr_log->date, FDTF_LONGDATE | FDTF_LONGTIME).GetString ());

			WCHAR addr_format[LEN_IP_MAX] = {0};
			const bool is_addressset = _app_formataddress (addr_format, _countof (addr_format), ptr_log, FWP_DIRECTION_OUTBOUND, 0, false);

			_r_ctrl_settext (config.hnotification, IDC_CREATERULE_ADDR_ID, app.LocaleString (IDS_NOTIFY_CREATERULE_ADDRESS, nullptr), addr_format);
			_r_ctrl_settext (config.hnotification, IDC_CREATERULE_PORT_ID, app.LocaleString (IDS_NOTIFY_CREATERULE_PORT, nullptr), ptr_log->remote_port);

			_r_ctrl_enable (config.hnotification, IDC_CREATERULE_ADDR_ID, is_addressset);
			_r_ctrl_enable (config.hnotification, IDC_CREATERULE_PORT_ID, ptr_log->remote_port != 0);

			_r_ctrl_settext (config.hnotification, IDC_ALLOW_BTN, app.LocaleString (IDS_ACTION_1, nullptr));
			_r_ctrl_settext (config.hnotification, IDC_IGNORE_BTN, app.LocaleString (IDS_ACTION_3, nullptr));

			// timers
			SendDlgItemMessage (config.hnotification, IDC_TIMER_CB, CB_RESETCONTENT, 0, 0);
			SendDlgItemMessage (config.hnotification, IDC_TIMER_CB, CB_INSERTSTRING, 0, (LPARAM)app.LocaleString (IDS_NOTIMER, nullptr).GetString ());

			for (size_t i = 0; i < timers.size (); i++)
				SendDlgItemMessage (config.hnotification, IDC_TIMER_CB, CB_INSERTSTRING, i + 1, (LPARAM)_r_fmt_interval (timers.at (i)).GetString ());

			SendDlgItemMessage (config.hnotification, IDC_TIMER_CB, CB_SETCURSEL, ptr_app->is_temp ? min (TIMER_DEFAULT + 1, timers.size ()) : 0, 0);

			CheckDlgButton (config.hnotification, IDC_CREATERULE_ADDR_ID, BST_UNCHECKED);
			CheckDlgButton (config.hnotification, IDC_CREATERULE_PORT_ID, BST_UNCHECKED);

			_app_notifysetpos (config.hnotification);

			// redraw icon
			{
				const HWND hctrl = GetDlgItem (config.hnotification, IDC_ICON_ID);

				RECT rect = {0};

				GetClientRect (hctrl, &rect);
				InvalidateRect (hctrl, &rect, TRUE);
				MapWindowPoints (hctrl, config.hnotification, (LPPOINT)&rect, 2);
				RedrawWindow (config.hnotification, &rect, nullptr, RDW_ERASE | RDW_INVALIDATE);
			}

			_r_fastlock_releaseshared (&lock_notification);
			_r_fastlock_releaseshared (&lock_access);

			SendMessage (config.hnotification, WM_COMMAND, MAKEWPARAM (IDC_CREATERULE_ADDR_ID, 0), 0);
			SendMessage (config.hnotification, WM_COMMAND, MAKEWPARAM (IDC_CREATERULE_PORT_ID, 0), 0);

			//SetTimer (config.hnotification, NOTIFY_TIMER_FOREGROUND_ID, 250, nullptr);

			ShowWindow (config.hnotification, is_forced ? SW_SHOW : SW_SHOWNA);

			return true;
		}

		_r_fastlock_releaseshared (&lock_access);
	}

	_r_fastlock_releaseshared (&lock_notification);

	return false;
}

bool _app_notifyrefresh ()
{
	if (!app.ConfigGet (L"IsNotificationsEnabled", true).AsBool ())
	{
		_app_notifyhide (config.hnotification);
		return true;
	}

	_r_fastlock_acquireshared (&lock_notification);

	const size_t idx = _app_notifygetcurrent ();

	if (notifications.empty () || idx == LAST_VALUE || !IsWindowVisible (config.hnotification))
	{
		_app_notifyhide (config.hnotification);
		_r_fastlock_releaseshared (&lock_notification);

		return false;
	}

	_r_fastlock_releaseshared (&lock_notification);

	return _app_notifyshow (idx, false);
}

// Play notification sound even if system have "nosound" mode
void _app_notifyplaysound ()
{
	bool result = false;

	if (!config.notify_snd_path[0])
	{
		HKEY hkey = nullptr;

		if (RegOpenKeyEx (HKEY_CURRENT_USER, L"AppEvents\\Schemes\\Apps\\.Default\\" NOTIFY_SOUND_DEFAULT L"\\.Default", 0, KEY_READ, &hkey) == ERROR_SUCCESS)
		{
			DWORD size = _countof (config.notify_snd_path) * sizeof (WCHAR);

			if (RegQueryValueEx (hkey, nullptr, nullptr, nullptr, (LPBYTE)config.notify_snd_path, &size) == ERROR_SUCCESS)
			{
				StringCchCopy (config.notify_snd_path, _countof (config.notify_snd_path), _r_path_expand (config.notify_snd_path));
				result = true;
			}

			RegCloseKey (hkey);
		}
	}
	else
	{
		result = true;
	}

	if (!result || !_r_fs_exists (config.notify_snd_path) || !PlaySound (config.notify_snd_path, nullptr, SND_SENTRY | SND_SYSTEM | SND_FILENAME | SND_ASYNC))
		PlaySound (NOTIFY_SOUND_DEFAULT, nullptr, SND_SENTRY | SND_SYSTEM | SND_ASYNC);
}

void _app_notifyadd (PITEM_LOG const ptr_log)
{
	// check for last display time
	{
		const UINT notification_timeout = max (app.ConfigGet (L"NotificationsTimeout", NOTIFY_TIMEOUT).AsUint (), NOTIFY_TIMEOUT_MINIMUM);

		if (notification_timeout && ((_r_unixtime_now () - notifications_last[ptr_log->hash]) <= notification_timeout))
			return;
	}

	_r_fastlock_acquireshared (&lock_notification);

	// check limit
	const bool is_limitexceed = (notifications.size () >= NOTIFY_LIMIT_SIZE);

	_r_fastlock_releaseshared (&lock_notification);

	// check limit
	if (is_limitexceed)
	{
		_r_fastlock_acquireexclusive (&lock_notification);
		_app_freenotify (0, 0);
		_r_fastlock_releaseexclusive (&lock_notification);
	}

	//_app_notifyrefresh ();

	// push or replace log item
	_r_fastlock_acquireexclusive (&lock_notification);

	// get existing pool id (if exists)
	size_t chk_idx = LAST_VALUE;

	for (size_t i = 0; i < notifications.size (); i++)
	{
		PITEM_LOG ptr_chk = notifications.at (i);

		if (ptr_chk && ptr_chk->hash == ptr_log->hash)
		{
			chk_idx = i;
			break;
		}
	}

	notifications_last[ptr_log->hash] = _r_unixtime_now ();

	PITEM_LOG ptr_log2 = new ITEM_LOG;

	if (ptr_log2)
	{
		memcpy (ptr_log2, ptr_log, sizeof (ITEM_LOG));

		if (chk_idx != LAST_VALUE)
		{
			//_app_freenotify (chk_idx, 0);
			delete notifications.at (chk_idx);
			notifications.at (chk_idx) = ptr_log2;

			_app_notifyrefresh ();
		}
		else
		{
			notifications.push_back (ptr_log2);
		}

		const size_t total_size = notifications.size ();
		const size_t idx = total_size - 1;

		_r_fastlock_releaseexclusive (&lock_notification);

		if (_app_notifyshow (idx, false))
		{
			if (app.ConfigGet (L"IsNotificationsSound", true).AsBool ())
				_app_notifyplaysound ();

			const UINT display_timeout = app.ConfigGet (L"NotificationsDisplayTimeout", NOTIFY_TIMER_DEFAULT).AsUint ();

			if (display_timeout)
				_app_notifysettimeout (config.hnotification, NOTIFY_TIMER_TIMEOUT_ID, true, (display_timeout * _R_SECONDSCLOCK_MSEC));
		}
	}
}

void CALLBACK _app_logcallback (UINT32 flags, FILETIME const* pft, UINT8 const* app_id, SID* package_id, SID* user_id, UINT8 proto, FWP_IP_VERSION ipver, UINT32 remote_addr, FWP_BYTE_ARRAY16 const* remote_addr6, UINT16 remoteport, UINT32 local_addr, FWP_BYTE_ARRAY16 const* local_addr6, UINT16 localport, UINT16 layer_id, UINT64 filter_id, UINT32 direction, bool is_loopback)
{
	const bool is_logenabled = app.ConfigGet (L"IsLogEnabled", false).AsBool ();
	const bool is_notificationenabled = (app.ConfigGet (L"Mode", ModeWhitelist).AsUint () == ModeWhitelist) && app.ConfigGet (L"IsNotificationsEnabled", true).AsBool (); // only for whitelist mode

	// do not parse when tcp connection has been established, or when non-tcp traffic has been authorized
	if (layer_id)
	{
		FWPM_LAYER* layer = nullptr;

		if (FwpmLayerGetById (config.hengine, layer_id, &layer) == ERROR_SUCCESS)
		{
			if (layer && (memcmp (&layer->layerKey, &FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4, sizeof (GUID)) == 0 || memcmp (&layer->layerKey, &FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6, sizeof (GUID)) == 0))
			{
				FwpmFreeMemory ((LPVOID*)&layer);
				return;
			}

			FwpmFreeMemory ((LPVOID*)&layer);
		}
	}

	PITEM_LIST_ENTRY ptr_entry = (PITEM_LIST_ENTRY)_aligned_malloc (sizeof (ITEM_LIST_ENTRY), MEMORY_ALLOCATION_ALIGNMENT);

	if (!ptr_entry)
		return;

	PITEM_LOG ptr_log = new ITEM_LOG;

	if (!ptr_log)
		return;

	// get package id (win8+)
	LPWSTR sidstring = nullptr;

	if (package_id)
	{
		if (ConvertSidToStringSid (package_id, &sidstring))
		{
			if (!_app_item_get (&packages, _r_str_hash (sidstring), nullptr, nullptr, nullptr, nullptr, nullptr))
			{
				LocalFree (sidstring);
				sidstring = nullptr;
			}
		}
	}

	// copy converted nt device path into win32
	if (sidstring)
	{
		StringCchCopy (ptr_log->path, _countof (ptr_log->path), sidstring);
		ptr_log->hash = _r_str_hash (ptr_log->path);

		LocalFree (sidstring);
		sidstring = nullptr;
	}
	else if (app_id)
	{
		StringCchCopy (ptr_log->path, _countof (ptr_log->path), _r_path_dospathfromnt (LPCWSTR (app_id)));
		ptr_log->hash = _r_str_hash (ptr_log->path);
	}
	else
	{
		StringCchCopy (ptr_log->path, _countof (ptr_log->path), NA_TEXT);
		ptr_log->hash = 0;
	}

	if (is_logenabled || is_notificationenabled)
	{
		// copy date and time
		if (pft)
			ptr_log->date = _r_unixtime_from_filetime (pft);

		// get username (only if log enabled)
		if (is_logenabled)
		{
			if ((flags & FWPM_NET_EVENT_FLAG_USER_ID_SET) != 0 && user_id)
			{
				SID_NAME_USE sid_type = SidTypeInvalid;

				WCHAR username[MAX_PATH] = {0};
				WCHAR domain[MAX_PATH] = {0};

				DWORD length1 = _countof (username);
				DWORD length2 = _countof (domain);

				if (LookupAccountSid (nullptr, user_id, username, &length1, domain, &length2, &sid_type) && length1 && length2)
					StringCchPrintf (ptr_log->username, _countof (ptr_log->username), L"%s\\%s", domain, username);

				else
					StringCchCopy (ptr_log->username, _countof (ptr_log->username), NA_TEXT);
			}
			else
			{
				StringCchCopy (ptr_log->username, _countof (ptr_log->username), NA_TEXT);
			}
		}

		// read filter information
		if (filter_id)
		{
			bool is_filternameset = false;

			FWPM_FILTER* filter = nullptr;
			FWPM_PROVIDER* provider = nullptr;

			if (FwpmFilterGetById (config.hengine, filter_id, &filter) == ERROR_SUCCESS)
			{
				StringCchCopy (ptr_log->filter_name, _countof (ptr_log->filter_name), (filter->displayData.description ? filter->displayData.description : filter->displayData.name));

				if (filter->providerKey)
				{
					if (memcmp (filter->providerKey, &GUID_WfpProvider, sizeof (GUID)) == 0)
						ptr_log->is_myprovider = true;

					if (filter->weight.type == FWP_UINT8 && filter->weight.uint8 == FILTER_WEIGHT_BLOCKLIST)
						ptr_log->is_blocklist = true;

					if (FwpmProviderGetByKey (config.hengine, filter->providerKey, &provider) == ERROR_SUCCESS)
					{
						StringCchCopy (ptr_log->provider_name, _countof (ptr_log->provider_name), (provider->displayData.description ? provider->displayData.description : provider->displayData.name));
						is_filternameset = true;
					}
				}
			}

			if (filter)
				FwpmFreeMemory ((LPVOID*)&filter);

			if (provider)
				FwpmFreeMemory ((LPVOID*)&provider);

			if (!is_filternameset)
				StringCchPrintf (ptr_log->filter_name, _countof (ptr_log->filter_name), L"#%llu", filter_id);
		}

		// destination
		{
			// ipv4 address
			if (ipver == FWP_IP_VERSION_V4)
			{
				ptr_log->af = AF_INET;

				// remote address
				ptr_log->remote_addr.S_un.S_addr = ntohl (remote_addr);

				if (remoteport)
					ptr_log->remote_port = remoteport;

				// local address
				ptr_log->local_addr.S_un.S_addr = ntohl (local_addr);

				if (localport)
					ptr_log->local_port = localport;
			}
			else if (ipver == FWP_IP_VERSION_V6)
			{
				ptr_log->af = AF_INET6;

				// remote address
				memcpy (ptr_log->remote_addr6.u.Byte, remote_addr6->byteArray16, FWP_V6_ADDR_SIZE);

				if (remoteport)
					ptr_log->remote_port = remoteport;

				// local address
				memcpy (&ptr_log->local_addr6.u.Byte, local_addr6->byteArray16, FWP_V6_ADDR_SIZE);

				if (localport)
					ptr_log->local_port = localport;
			}
		}

		// protocol
		ptr_log->protocol = proto;

		// indicates whether the packet originated from (or was heading to) the loopback adapter
		ptr_log->is_loopback = is_loopback;

		// indicates the direction of the packet transmission
		if (direction == FWP_DIRECTION_OUTBOUND || direction == FWP_DIRECTION_OUT)
			ptr_log->direction = FWP_DIRECTION_OUTBOUND;

		else if (direction == FWP_DIRECTION_INBOUND || direction == FWP_DIRECTION_IN)
			ptr_log->direction = FWP_DIRECTION_INBOUND;
	}

	// push to lock-free stack
	{
		ptr_entry->Body = (LONG_PTR)ptr_log;

		InterlockedPushEntrySList (log_stack, &ptr_entry->ListEntry);

		SetEvent (config.log_evt);
	}
}

// win7 callback
void CALLBACK _app_logcallback0 (LPVOID, const FWPM_NET_EVENT1 *pEvent)
{
	if (pEvent)
	{
		UINT16 layer_id = 0;
		UINT64 filter_id = 0;
		UINT32 direction = 0;
		bool is_loopback = false;

		if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && pEvent->classifyDrop)
		{
			layer_id = pEvent->classifyDrop->layerId;
			filter_id = pEvent->classifyDrop->filterId;
			direction = pEvent->classifyDrop->msFwpDirection;
			is_loopback = pEvent->classifyDrop->isLoopback;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP && pEvent->ipsecDrop)
		{
			layer_id = pEvent->ipsecDrop->layerId;
			filter_id = pEvent->ipsecDrop->filterId;
			direction = pEvent->ipsecDrop->direction;
		}
		else
		{
			return;
		}

		_app_logcallback (pEvent->header.flags, &pEvent->header.timeStamp, pEvent->header.appId.data, nullptr, pEvent->header.userId, pEvent->header.ipProtocol, pEvent->header.ipVersion, pEvent->header.remoteAddrV4, &pEvent->header.remoteAddrV6, pEvent->header.remotePort, pEvent->header.localAddrV4, &pEvent->header.localAddrV6, pEvent->header.localPort, layer_id, filter_id, direction, is_loopback);
	}
}

// win8 callback
void CALLBACK _app_logcallback1 (LPVOID, const FWPM_NET_EVENT2 *pEvent)
{
	if (pEvent)
	{
		UINT16 layer_id = 0;
		UINT64 filter_id = 0;
		UINT32 direction = 0;
		bool is_loopback = false;

		if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && pEvent->classifyDrop)
		{
			layer_id = pEvent->classifyDrop->layerId;
			filter_id = pEvent->classifyDrop->filterId;
			direction = pEvent->classifyDrop->msFwpDirection;
			is_loopback = pEvent->classifyDrop->isLoopback;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP && pEvent->ipsecDrop)
		{
			layer_id = pEvent->ipsecDrop->layerId;
			filter_id = pEvent->ipsecDrop->filterId;
			direction = pEvent->ipsecDrop->direction;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC && pEvent->classifyDropMac)
		{
			layer_id = pEvent->classifyDropMac->layerId;
			filter_id = pEvent->classifyDropMac->filterId;
			direction = pEvent->classifyDropMac->msFwpDirection;
			is_loopback = pEvent->classifyDropMac->isLoopback;
		}
		else
		{
			return;
		}

		_app_logcallback (pEvent->header.flags, &pEvent->header.timeStamp, pEvent->header.appId.data, pEvent->header.packageSid, pEvent->header.userId, pEvent->header.ipProtocol, pEvent->header.ipVersion, pEvent->header.remoteAddrV4, &pEvent->header.remoteAddrV6, pEvent->header.remotePort, pEvent->header.localAddrV4, &pEvent->header.localAddrV6, pEvent->header.localPort, layer_id, filter_id, direction, is_loopback);
	}
}

// win10 callback
void CALLBACK _app_logcallback2 (LPVOID, const FWPM_NET_EVENT3 *pEvent)
{
	if (pEvent)
	{
		UINT16 layer_id = 0;
		UINT64 filter_id = 0;
		UINT32 direction = 0;
		bool is_loopback = false;

		if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && pEvent->classifyDrop)
		{
			layer_id = pEvent->classifyDrop->layerId;
			filter_id = pEvent->classifyDrop->filterId;
			direction = pEvent->classifyDrop->msFwpDirection;
			is_loopback = pEvent->classifyDrop->isLoopback;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP && pEvent->ipsecDrop)
		{
			layer_id = pEvent->ipsecDrop->layerId;
			filter_id = pEvent->ipsecDrop->filterId;
			direction = pEvent->ipsecDrop->direction;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC && pEvent->classifyDropMac)
		{
			layer_id = pEvent->classifyDropMac->layerId;
			filter_id = pEvent->classifyDropMac->filterId;
			direction = pEvent->classifyDropMac->msFwpDirection;
			is_loopback = pEvent->classifyDropMac->isLoopback;
		}
		else
		{
			return;
		}

		_app_logcallback (pEvent->header.flags, &pEvent->header.timeStamp, pEvent->header.appId.data, pEvent->header.packageSid, pEvent->header.userId, pEvent->header.ipProtocol, pEvent->header.ipVersion, pEvent->header.remoteAddrV4, &pEvent->header.remoteAddrV6, pEvent->header.remotePort, pEvent->header.localAddrV4, &pEvent->header.localAddrV6, pEvent->header.localPort, layer_id, filter_id, direction, is_loopback);
	}
}

UINT WINAPI ApplyThread (LPVOID lparam)
{
	if (WaitForSingleObjectEx (config.stop_evt, 1000, FALSE) == WAIT_OBJECT_0)
	{
		SetEvent (config.finish_evt);
		config.hthread = nullptr;

		return ERROR_SUCCESS;
	}

	_r_fastlock_acquireexclusive (&lock_apply);

	const bool is_install = lparam ? true : false;

	_r_ctrl_enable (app.GetHWND (), IDC_START_BTN, false);

	if (is_install)
	{
		if (_wfp_initialize (true))
			_wfp_installfilters ();

		_app_listviewsort (app.GetHWND (), IDC_LISTVIEW, -1, false);
		_app_profilesave (app.GetHWND ());

		_r_listview_redraw (app.GetHWND (), IDC_LISTVIEW);
	}
	else
	{
		if (_wfp_initialize (false))
			_wfp_destroyfilters (true);

		_wfp_uninitialize (true);
	}

	_r_fastlock_releaseexclusive (&lock_apply);

	_r_ctrl_enable (app.GetHWND (), IDC_START_BTN, true);

	SetEvent (config.finish_evt);

	config.hthread = nullptr;

	return ERROR_SUCCESS;
}

UINT WINAPI LogThread (LPVOID lparam)
{
	HWND hwnd = (HWND)lparam;

	while (true)
	{
		const DWORD result = WaitForSingleObjectEx (config.log_evt, TIMER_LOG_CALLBACK, FALSE);

		if (result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT)
		{
			const bool is_logenabled = app.ConfigGet (L"IsLogEnabled", false).AsBool ();
			const bool is_notificationenabled = (app.ConfigGet (L"Mode", ModeWhitelist).AsUint () == ModeWhitelist) && app.ConfigGet (L"IsNotificationsEnabled", true).AsBool (); // only for whitelist mode

			for (;;)
			{
				PITEM_LIST_ENTRY pNode = (PITEM_LIST_ENTRY)InterlockedPopEntrySList (log_stack);

				if (!pNode)
					break;

				PITEM_LOG ptr_log = (PITEM_LOG)pNode->Body;

				_aligned_free (pNode);

				if (ptr_log)
				{
					// apps collector
					if (ptr_log->hash && apps.find (ptr_log->hash) == apps.end ())
					{
						_r_fastlock_acquireexclusive (&lock_access);
						_app_addapplication (hwnd, ptr_log->path, 0, false, false, true);
						_r_fastlock_releaseexclusive (&lock_access);

						_app_profilesave (hwnd);
						_app_listviewsort (hwnd, IDC_LISTVIEW, -1, false);
					}

					if (is_logenabled || is_notificationenabled)
					{
						_app_formataddress (ptr_log->remote_fmt, _countof (ptr_log->remote_fmt), ptr_log, FWP_DIRECTION_OUTBOUND, ptr_log->remote_port, true);
						_app_formataddress (ptr_log->local_fmt, _countof (ptr_log->local_fmt), ptr_log, FWP_DIRECTION_INBOUND, ptr_log->local_port, true);

						// write log to a file
						if (is_logenabled)
							_app_logwrite (ptr_log);

						// show notification (only for my own provider and file is present)
						if (is_notificationenabled && ptr_log->is_myprovider && ptr_log->hash)
						{
							if (!(ptr_log->is_blocklist && app.ConfigGet (L"IsNotificationsExcludeBlocklist", true).AsBool ()))
							{
								bool is_silent = true;

								// read app config
								{
									_r_fastlock_acquireshared (&lock_access);

									PITEM_APP const ptr_app = _app_getapplication (ptr_log->hash);

									if (ptr_app)
										is_silent = ptr_app->is_silent;

									_r_fastlock_releaseshared (&lock_access);
								}

								if (!is_silent)
									_app_notifyadd (ptr_log);
							}
						}
					}

					delete ptr_log;
				}
			}
		}
		else
		{
			break;
		}
	}

	return ERROR_SUCCESS;
}

void addcolor (UINT locale_id, LPCWSTR config_name, bool is_enabled, LPCWSTR config_value, COLORREF default_clr)
{
	ITEM_COLOR color = {0};
	SecureZeroMemory (&color, sizeof (color));

	size_t length = 0;

	if (config_name)
	{
		length = wcslen (config_name) + 1;
		color.config_name = new WCHAR[length];

		if (color.config_name)
			StringCchCopy (color.config_name, length, config_name);
	}

	if (config_value)
	{
		length = wcslen (config_value) + 1;
		color.config_value = new WCHAR[length];

		if (color.config_value)
			StringCchCopy (color.config_value, length, config_value);
	}

	color.hash = _r_str_hash (config_value);
	color.is_enabled = is_enabled;
	color.locale_id = locale_id;
	color.default_clr = default_clr;
	color.clr = app.ConfigGet (config_value, default_clr).AsUlong ();
	color.hbrush = CreateSolidBrush (color.clr);

	colors.push_back (color);
}

void addprotocol (LPCWSTR name, UINT8 id)
{
	ITEM_PROTOCOL protocol = {0};
	SecureZeroMemory (&protocol, sizeof (protocol));

	if (name)
	{
		const size_t length = wcslen (name) + 1;
		protocol.name = new WCHAR[length];

		if (protocol.name)
			StringCchCopy (protocol.name, length, name);
	}

	protocol.id = id;

	protocols.push_back (protocol);
}

void _app_generate_processes ()
{
	// clear previous result
	{
		for (size_t i = 0; i < processes.size (); i++)
		{
			if (processes.at (i).hbmp)
				DeleteObject (processes.at (i).hbmp); // free memory
		}
	}

	processes.clear (); // clear previous result

	NTSTATUS status = 0;

	ULONG length = 0x4000;
	PVOID buffer = new BYTE[length];

	while (true)
	{
		status = NtQuerySystemInformation (SystemProcessInformation, buffer, length, &length);

		if (status == 0xC0000023L /*STATUS_BUFFER_TOO_SMALL*/ || status == 0xc0000004 /*STATUS_INFO_LENGTH_MISMATCH*/)
		{
			delete[] buffer;
			buffer = new BYTE[length];
		}
		else
		{
			break;
		}
	}

	if (NT_SUCCESS (status))
	{
		PSYSTEM_PROCESS_INFORMATION spi = (PSYSTEM_PROCESS_INFORMATION)buffer;

		std::unordered_map<size_t, bool> checker;

		do
		{
			const DWORD pid = (DWORD)(DWORD_PTR)spi->UniqueProcessId;

			if (!pid) // skip "system idle process"
				continue;

			const HANDLE hprocess = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

			if (hprocess)
			{
				WCHAR display_name[128] = {0};
				WCHAR real_path[MAX_PATH] = {0};

				size_t hash = 0;

				StringCchPrintf (display_name, _countof (display_name), L"%s (%lu)", spi->ImageName.Buffer, pid);

				if (pid == PROC_SYSTEM_PID)
				{
					StringCchCopy (real_path, _countof (real_path), _r_path_expand (PATH_NTOSKRNL));
					hash = _r_str_hash (spi->ImageName.Buffer);
				}
				else
				{
					DWORD size = _countof (real_path) - 1;

					if (QueryFullProcessImageName (hprocess, 0, real_path, &size))
					{
						_app_applycasestyle (real_path, wcslen (real_path)); // apply case-style
						hash = _r_str_hash (real_path);
					}
					else
					{
						// cannot get file path because it's not filesystem process (Pico maybe?)
						if (GetLastError () == ERROR_GEN_FAILURE)
						{
							StringCchCopy (real_path, _countof (real_path), spi->ImageName.Buffer);
							hash = _r_str_hash (spi->ImageName.Buffer);
						}
						else
						{
							CloseHandle (hprocess);
							continue;
						}
					}
				}

				if (hash && apps.find (hash) == apps.end () && checker.find (hash) == checker.end ())
				{
					checker[hash] = true;

					ITEM_ADD item;
					SecureZeroMemory (&item, sizeof (item));

					StringCchCopy (item.display_name, _countof (item.display_name), display_name);
					StringCchCopy (item.real_path, _countof (item.real_path), ((pid == PROC_SYSTEM_PID) ? PROC_SYSTEM_NAME : real_path));

					// get file icon
					{
						HICON hicon = nullptr;

						if (_app_getfileicon (real_path, true, nullptr, &hicon))
						{
							item.hbmp = _app_ico2bmp (hicon);
							DestroyIcon (hicon);
						}
						else
						{
							item.hbmp = _app_ico2bmp (config.hicon_small);
						}
					}

					processes.push_back (item);
				}

				CloseHandle (hprocess);
			}
		}
		while ((spi = ((spi->NextEntryOffset ? (PSYSTEM_PROCESS_INFORMATION)((PCHAR)(spi)+(spi)->NextEntryOffset) : nullptr))) != nullptr);

		std::sort (processes.begin (), processes.end (),
			[](const ITEM_ADD& a, const ITEM_ADD& b)->bool {
			return StrCmpLogicalW (a.display_name, b.display_name) == -1;
		});
	}

	delete[] buffer; // free the allocated buffer
}

bool _app_installmessage (HWND hwnd, bool is_install)
{
	WCHAR main[256] = {0};

	WCHAR mode[128] = {0};
	WCHAR mode1[128] = {0};
	WCHAR mode2[128] = {0};

	WCHAR flag[64] = {0};

	INT result = 0;
	BOOL is_flagchecked = FALSE;
	INT radio_checked = 0;

	TASKDIALOGCONFIG tdc = {0};
	TASKDIALOG_BUTTON tdr[2] = {0};

	tdc.cbSize = sizeof (tdc);
	tdc.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
	tdc.hwndParent = hwnd;
	tdc.pszWindowTitle = APP_NAME;
	tdc.pfCallback = &_r_msg_callback;
	tdc.pszMainIcon = TD_WARNING_ICON;
	tdc.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
	tdc.pszMainInstruction = main;
	tdc.pszVerificationText = flag;
	tdc.lpCallbackData = 1; // always on top

	if (is_install)
	{
		StringCchCopy (main, _countof (main), app.LocaleString (IDS_QUESTION_START, nullptr));
		StringCchCopy (flag, _countof (flag), app.LocaleString (IDS_DISABLEWINDOWSFIREWALL_CHK, nullptr));

		tdc.pszContent = mode;
		tdc.pRadioButtons = tdr;
		tdc.cRadioButtons = _countof (tdr);
		tdc.nDefaultRadioButton = IDM_TRAY_MODEWHITELIST + app.ConfigGet (L"Mode", ModeWhitelist).AsUint ();

		tdr[0].nButtonID = IDM_TRAY_MODEWHITELIST;
		tdr[0].pszButtonText = mode1;

		tdr[1].nButtonID = IDM_TRAY_MODEBLACKLIST;
		tdr[1].pszButtonText = mode2;

		StringCchCopy (mode, _countof (mode), app.LocaleString (IDS_TRAY_MODE, L":"));
		StringCchCopy (mode1, _countof (mode1), app.LocaleString (IDS_MODE_WHITELIST, nullptr));
		StringCchCopy (mode2, _countof (mode2), app.LocaleString (IDS_MODE_BLACKLIST, nullptr));

		if (app.ConfigGet (L"IsDisableWindowsFirewallChecked", true).AsBool ())
			tdc.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
	}
	else
	{
		StringCchCopy (main, _countof (main), app.LocaleString (IDS_QUESTION_STOP, nullptr));
		StringCchCopy (flag, _countof (flag), app.LocaleString (IDS_ENABLEWINDOWSFIREWALL_CHK, nullptr));

		if (app.ConfigGet (L"IsEnableWindowsFirewallChecked", true).AsBool ())
			tdc.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
	}

	if (_r_msg_taskdialog (&tdc, &result, &radio_checked, &is_flagchecked))
	{
		if (result == IDYES)
		{
			if (is_install)
			{
				app.ConfigSet (L"IsDisableWindowsFirewallChecked", is_flagchecked ? true : false);

				app.ConfigSet (L"Mode", DWORD ((radio_checked == IDM_TRAY_MODEWHITELIST) ? ModeWhitelist : ModeBlacklist));
				CheckMenuRadioItem (GetMenu (hwnd), IDM_TRAY_MODEWHITELIST, IDM_TRAY_MODEBLACKLIST, IDM_TRAY_MODEWHITELIST + app.ConfigGet (L"Mode", ModeWhitelist).AsUint (), MF_BYCOMMAND);

				if (is_flagchecked)
					_mps_changeconfig (true);
			}
			else
			{
				app.ConfigSet (L"IsEnableWindowsFirewallChecked", is_flagchecked ? true : false);

				if (is_flagchecked)
					_mps_changeconfig (false);
			}

			return true;
		}
	}

	return false;
}

BOOL initializer_callback (HWND hwnd, DWORD msg, LPVOID, LPVOID)
{
	switch (msg)
	{
		case _RM_ARGUMENTS:
		{
			if (wcsstr (GetCommandLine (), L"/uninstall"))
			{
				const bool is_enabled = _wfp_isfiltersinstalled ();

				if (is_enabled)
				{
					if (_app_installmessage (hwnd, false))
					{
						if (_wfp_initialize (false))
							_wfp_destroyfilters (true);

						_wfp_uninitialize (true);
					}
				}

				return TRUE;
			}

			break;
		}

		case _RM_INITIALIZE:
		{
			if (app.ConfigGet (L"ShowTitleID", true).AsBool ())
				SetWindowText (hwnd, config.title);

			else
				SetWindowText (hwnd, APP_NAME);

			const bool state = _wfp_isfiltersinstalled ();

			// set icons
			app.SetIcon (hwnd, state ? IDI_ACTIVE : IDI_INACTIVE, true);
			app.TrayCreate (hwnd, UID, WM_TRAYICON, _r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (state ? IDI_ACTIVE : IDI_INACTIVE), GetSystemMetrics (SM_CXSMICON)), false);
			SetDlgItemText (hwnd, IDC_START_BTN, app.LocaleString (state ? IDS_TRAY_STOP : IDS_TRAY_START, nullptr));

			CheckMenuItem (GetMenu (hwnd), IDM_ALWAYSONTOP_CHK, MF_BYCOMMAND | (app.ConfigGet (L"AlwaysOnTop", false).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_SHOWFILENAMESONLY_CHK, MF_BYCOMMAND | (app.ConfigGet (L"ShowFilenames", true).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_AUTOSIZECOLUMNS_CHK, MF_BYCOMMAND | (app.ConfigGet (L"AutoSizeColumns", true).AsBool () ? MF_CHECKED : MF_UNCHECKED));

			CheckMenuRadioItem (GetMenu (hwnd), IDM_TRAY_MODEWHITELIST, IDM_TRAY_MODEBLACKLIST, IDM_TRAY_MODEWHITELIST + app.ConfigGet (L"Mode", ModeWhitelist).AsUint (), MF_BYCOMMAND);

			CheckMenuItem (GetMenu (hwnd), IDM_ENABLELOG_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsLogEnabled", false).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsNotificationsEnabled", true).AsBool () ? MF_CHECKED : MF_UNCHECKED));

			// dropped packets logging (win7+)
			if (!_r_sys_validversion (6, 1))
			{
				EnableMenuItem (GetMenu (hwnd), IDM_ENABLELOG_CHK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
				EnableMenuItem (GetMenu (hwnd), IDM_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
			}

			break;
		}

		case _RM_LOCALIZE:
		{
			const HMENU menu = GetMenu (hwnd);

			app.LocaleMenu (menu, IDS_FILE, 0, true, nullptr);
			app.LocaleMenu (GetSubMenu (menu, 0), IDS_EXPORT, 0, true, nullptr);
			app.LocaleMenu (GetSubMenu (menu, 0), IDS_IMPORT, 1, true, nullptr);
			app.LocaleMenu (menu, IDS_EXPORT_XML, IDM_EXPORT_APPS, false, L" " XML_APPS);
			app.LocaleMenu (menu, IDS_EXPORT_XML, IDM_EXPORT_RULES, false, L" " XML_RULES_CUSTOM);
			app.LocaleMenu (menu, IDS_IMPORT_XML, IDM_IMPORT_APPS, false, L" " XML_APPS);
			app.LocaleMenu (menu, IDS_IMPORT_XML, IDM_IMPORT_RULES, false, L" " XML_RULES_CUSTOM);
			app.LocaleMenu (menu, IDS_EXIT, IDM_EXIT, false, nullptr);

			app.LocaleMenu (menu, IDS_EDIT, 1, true, nullptr);

			app.LocaleMenu (menu, IDS_PURGE_UNUSED, IDM_PURGE_UNUSED, false, L"\tCtrl+Shift+X");
			app.LocaleMenu (menu, IDS_PURGE_ERRORS, IDM_PURGE_ERRORS, false, L"\tCtrl+Shift+E");
			app.LocaleMenu (menu, IDS_PURGE_TIMERS, IDM_PURGE_TIMERS, false, L"\tCtrl+Shift+T");

			app.LocaleMenu (menu, IDS_FIND, IDM_FIND, false, L"\tCtrl+F");
			app.LocaleMenu (menu, IDS_FINDNEXT, IDM_FINDNEXT, false, L"\tF3");

			app.LocaleMenu (menu, IDS_REFRESH, IDM_REFRESH, false, L"\tF5");

			app.LocaleMenu (menu, IDS_VIEW, 2, true, nullptr);

			app.LocaleMenu (menu, IDS_ALWAYSONTOP_CHK, IDM_ALWAYSONTOP_CHK, false, nullptr);
			app.LocaleMenu (menu, IDS_SHOWFILENAMESONLY_CHK, IDM_SHOWFILENAMESONLY_CHK, false, nullptr);
			app.LocaleMenu (menu, IDS_AUTOSIZECOLUMNS_CHK, IDM_AUTOSIZECOLUMNS_CHK, false, nullptr);

			app.LocaleMenu (GetSubMenu (menu, 2), IDS_ICONS, 4, true, nullptr);
			app.LocaleMenu (menu, IDS_ICONSSMALL, IDM_ICONSSMALL, false, nullptr);
			app.LocaleMenu (menu, IDS_ICONSLARGE, IDM_ICONSLARGE, false, nullptr);
			app.LocaleMenu (menu, IDS_ICONSISHIDDEN, IDM_ICONSISHIDDEN, false, nullptr);

			app.LocaleMenu (GetSubMenu (menu, 2), IDS_LANGUAGE, LANG_MENU, true, L" (Language)");

			app.LocaleMenu (menu, IDS_FONT, IDM_FONT, false, nullptr);

			app.LocaleMenu (menu, IDS_SETTINGS, 3, true, nullptr);

			app.LocaleMenu (GetSubMenu (menu, 3), IDS_TRAY_MODE, 0, true, nullptr);

			app.LocaleMenu (menu, IDS_MODE_WHITELIST, IDM_TRAY_MODEWHITELIST, false, nullptr);
			app.LocaleMenu (menu, IDS_MODE_BLACKLIST, IDM_TRAY_MODEBLACKLIST, false, nullptr);

			app.LocaleMenu (GetSubMenu (menu, 3), IDS_TRAY_LOG, 1, true, nullptr);

			app.LocaleMenu (menu, IDS_ENABLELOG_CHK, IDM_ENABLELOG_CHK, false, nullptr);
			app.LocaleMenu (menu, IDS_ENABLENOTIFICATIONS_CHK, IDM_ENABLENOTIFICATIONS_CHK, false, nullptr);
			app.LocaleMenu (menu, IDS_LOGSHOW, IDM_LOGSHOW, false, L"\tCtrl+I");
			app.LocaleMenu (menu, IDS_LOGCLEAR, IDM_LOGCLEAR, false, L"\tCtrl+X");

			app.LocaleMenu (menu, IDS_SETTINGS, IDM_SETTINGS, false, L"...\tF2");

			app.LocaleMenu (menu, IDS_HELP, 4, true, nullptr);
			app.LocaleMenu (menu, IDS_WEBSITE, IDM_WEBSITE, false, nullptr);
			app.LocaleMenu (menu, IDS_CHECKUPDATES, IDM_CHECKUPDATES, false, nullptr);
			app.LocaleMenu (menu, IDS_ABOUT, IDM_ABOUT, false, L"\tF1");

			app.LocaleEnum ((HWND)GetSubMenu (menu, 2), LANG_MENU, true, IDX_LANGUAGE); // enum localizations

			{
				const bool state = _wfp_isfiltersinstalled ();
				SetDlgItemText (hwnd, IDC_START_BTN, app.LocaleString (_wfp_isfiltersinstalled () ? IDS_TRAY_STOP : IDS_TRAY_START, nullptr));
			}

			SetDlgItemText (hwnd, IDC_SETTINGS_BTN, app.LocaleString (IDS_SETTINGS, nullptr));
			SetDlgItemText (hwnd, IDC_EXIT_BTN, app.LocaleString (IDS_EXIT, nullptr));

			_r_wnd_addstyle (hwnd, IDC_START_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (hwnd, IDC_SETTINGS_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (hwnd, IDC_EXIT_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);

			_r_wnd_addstyle (config.hnotification, IDC_ALLOW_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			//_r_wnd_addstyle (config.hnotification, IDC_BLOCK_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (config.hnotification, IDC_IGNORE_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);

			_app_refreshstatus (hwnd, true, true);

			_r_listview_setcolumn (hwnd, IDC_LISTVIEW, 0, app.LocaleString (IDS_FILEPATH, nullptr), 0);
			_r_listview_setcolumn (hwnd, IDC_LISTVIEW, 1, app.LocaleString (IDS_ADDED, nullptr), 0);

			SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_RESETEMPTYTEXT, 0, 0);

			_app_notifyrefresh ();

			break;
		}

		case _RM_UNINITIALIZE:
		{
			app.TrayDestroy (UID);
			break;
		}
	}

	return FALSE;
}

LONG _app_wmcustdraw (LPNMLVCUSTOMDRAW lpnmlv, LPARAM lparam)
{
	LONG result = CDRF_DODEFAULT;

	if (!app.ConfigGet (L"UseHighlighting", true).AsBool ())
		return result;

	switch (lpnmlv->nmcd.dwDrawStage)
	{
		case CDDS_PREPAINT:
		{
			result = CDRF_NOTIFYITEMDRAW;
			break;
		}

		case CDDS_ITEMPREPAINT:
		{
			if (
				lpnmlv->nmcd.hdr.idFrom == IDC_LISTVIEW ||
				lpnmlv->nmcd.hdr.idFrom == IDC_FILES_LV
				)
			{
				const size_t hash = lpnmlv->nmcd.lItemlParam;

				if (hash)
				{
					const COLORREF new_clr = (COLORREF)_app_getcolor (hash, false, lpnmlv->nmcd.hdc);

					if (new_clr)
					{
						lpnmlv->clrTextBk = new_clr;
						lpnmlv->clrText = _r_dc_getcolorbrightness (new_clr);

						_r_dc_fillrect (lpnmlv->nmcd.hdc, &lpnmlv->nmcd.rc, new_clr);

						result = CDRF_NEWFONT;
					}
				}
			}
			else if (lpnmlv->nmcd.hdr.idFrom == IDC_COLORS)
			{
				PITEM_COLOR const ptr_clr = &colors.at (lpnmlv->nmcd.lItemlParam);

				if (ptr_clr)
				{
					lpnmlv->clrTextBk = ptr_clr->clr;
					lpnmlv->clrText = _r_dc_getcolorbrightness (ptr_clr->clr);

					_r_dc_fillrect (lpnmlv->nmcd.hdc, &lpnmlv->nmcd.rc, lpnmlv->clrTextBk);

					result = CDRF_NEWFONT;
				}
			}
			else if (lpnmlv->nmcd.hdr.idFrom == IDC_EDITOR)
			{
				COLORREF const custclr = (COLORREF)lparam;

				if (custclr)
				{
					lpnmlv->clrTextBk = custclr;
					lpnmlv->clrText = _r_dc_getcolorbrightness (custclr);

					_r_dc_fillrect (lpnmlv->nmcd.hdc, &lpnmlv->nmcd.rc, custclr);
				}
			}

			break;
		}
	}

	return result;
}

INT_PTR CALLBACK EditorProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	static PITEM_RULE ptr_rule = nullptr;
	static size_t idx = LAST_VALUE;

	switch (msg)
	{
		case WM_INITDIALOG:
		{
			ptr_rule = (PITEM_RULE)lparam;
			idx = GetWindowLongPtr (app.GetHWND (), GWLP_USERDATA);

			// configure window
			_r_wnd_center (hwnd, GetParent (hwnd));

			app.SetIcon (hwnd, 0, false);

			// localize window
			SetWindowText (hwnd, (ptr_rule && ptr_rule->pname ? _r_fmt (L"%s - \"%s\"", app.LocaleString (IDS_EDITOR, nullptr).GetString (), ptr_rule->pname) : app.LocaleString (IDS_EDITOR, nullptr)));

			SetDlgItemText (hwnd, IDC_NAME, app.LocaleString (IDS_NAME, L":"));
			SetDlgItemText (hwnd, IDC_RULES, app.LocaleString (IDS_RULE, L":"));
			SetDlgItemText (hwnd, IDC_FILES, app.LocaleString (IDS_APPLYTO, L":"));
			SetDlgItemText (hwnd, IDC_DIRECTION, app.LocaleString (IDS_DIRECTION, L":"));
			SetDlgItemText (hwnd, IDC_PROTOCOL, app.LocaleString (IDS_PROTOCOL, L":"));
			SetDlgItemText (hwnd, IDC_PORTVERSION, app.LocaleString (IDS_PORTVERSION, L":"));
			SetDlgItemText (hwnd, IDC_ACTION, app.LocaleString (IDS_ACTION, L":"));

			_r_ctrl_settext (hwnd, IDC_RULES_WIKI, app.LocaleString (IDS_RULES_WIKI, nullptr), WIKI_URL);
			SetDlgItemText (hwnd, IDC_ENABLED_CHK, app.LocaleString (IDS_ENABLERULE_CHK, nullptr));

			SetDlgItemText (hwnd, IDC_SAVE, app.LocaleString (IDS_SAVE, nullptr));
			SetDlgItemText (hwnd, IDC_CLOSE, app.LocaleString (IDS_CLOSE, nullptr));

			// configure listview
			_r_listview_setstyle (hwnd, IDC_FILES_LV, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);

			_r_listview_addcolumn (hwnd, IDC_FILES_LV, 0, nullptr, 95, LVCFMT_LEFT);

			_app_listviewsetimagelist (hwnd, IDC_FILES_LV);
			_app_listviewsetfont (hwnd, IDC_FILES_LV);

			// name
			if (ptr_rule && ptr_rule->pname)
				SetDlgItemText (hwnd, IDC_NAME_EDIT, ptr_rule->pname);

			// rule
			if (ptr_rule && ptr_rule->prule)
				SetDlgItemText (hwnd, IDC_RULES_EDIT, ptr_rule->prule);

			// apps (apply to)
			{
				size_t item = 0;

				_r_fastlock_acquireshared (&lock_access);

				for (auto& p : apps)
				{
					PITEM_APP const ptr_app = &p.second;

					if (ptr_app)
					{
						// windows store apps (win8+)
						if (ptr_app->type == AppStore && !_r_sys_validversion (6, 2))
							continue;

						config.is_nocheckboxnotify = true;

						_r_listview_additem (hwnd, IDC_FILES_LV, item, 0, _r_path_extractfile (ptr_app->display_name), ptr_app->icon_id, LAST_VALUE, p.first);
						_r_listview_setitemcheck (hwnd, IDC_FILES_LV, item, ptr_rule && !ptr_rule->apps.empty () && (ptr_rule->apps.find (p.first) != ptr_rule->apps.end ()));

						config.is_nocheckboxnotify = false;

						item += 1;
					}
				}

				_r_fastlock_releaseshared (&lock_access);

				// sort column
				_app_listviewsort_appsrules (hwnd, IDC_FILES_LV);

				// resize column
				RECT rc = {0};
				GetClientRect (GetDlgItem (hwnd, IDC_FILES_LV), &rc);

				_r_listview_setcolumn (hwnd, IDC_FILES_LV, 0, nullptr, (rc.right - rc.left));
			}

			// direction
			SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_INSERTSTRING, 0, (LPARAM)app.LocaleString (IDS_DIRECTION_1, nullptr).GetString ());
			SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_INSERTSTRING, 1, (LPARAM)app.LocaleString (IDS_DIRECTION_2, nullptr).GetString ());
			SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_INSERTSTRING, 2, (LPARAM)app.LocaleString (IDS_DIRECTION_3, nullptr).GetString ());

			if (ptr_rule)
				SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_SETCURSEL, (WPARAM)ptr_rule->dir, 0);

			// protocol
			SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_INSERTSTRING, 0, (LPARAM)app.LocaleString (IDS_ALL, nullptr).GetString ());
			SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_SETCURSEL, 0, 0);

			SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_SETEXTENDEDUI, 1, 0);

			for (size_t i = 0; i < protocols.size (); i++)
			{
				SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_INSERTSTRING, i + 1, (LPARAM)protocols.at (i).name);
				SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_SETITEMDATA, i + 1, (LPARAM)protocols.at (i).id);

				if (ptr_rule && ptr_rule->protocol == protocols.at (i).id)
					SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_SETCURSEL, (WPARAM)i + 1, 0);
			}

			// family (ports-only)
			SendDlgItemMessage (hwnd, IDC_PORTVERSION_EDIT, CB_INSERTSTRING, 0, (LPARAM)app.LocaleString (IDS_ALL, nullptr).GetString ());
			SendDlgItemMessage (hwnd, IDC_PORTVERSION_EDIT, CB_SETITEMDATA, 0, (LPARAM)AF_UNSPEC);

			SendDlgItemMessage (hwnd, IDC_PORTVERSION_EDIT, CB_INSERTSTRING, 1, (LPARAM)L"IPv4");
			SendDlgItemMessage (hwnd, IDC_PORTVERSION_EDIT, CB_SETITEMDATA, 1, (LPARAM)AF_INET);

			SendDlgItemMessage (hwnd, IDC_PORTVERSION_EDIT, CB_INSERTSTRING, 2, (LPARAM)L"IPv6");
			SendDlgItemMessage (hwnd, IDC_PORTVERSION_EDIT, CB_SETITEMDATA, 2, (LPARAM)AF_INET6);

			if (ptr_rule)
			{
				if (ptr_rule->version == AF_UNSPEC)
					SendDlgItemMessage (hwnd, IDC_PORTVERSION_EDIT, CB_SETCURSEL, (WPARAM)0, 0);

				else if (ptr_rule->version == AF_INET)
					SendDlgItemMessage (hwnd, IDC_PORTVERSION_EDIT, CB_SETCURSEL, (WPARAM)1, 0);

				else if (ptr_rule->version == AF_INET6)
					SendDlgItemMessage (hwnd, IDC_PORTVERSION_EDIT, CB_SETCURSEL, (WPARAM)2, 0);
			}

			// action
			SendDlgItemMessage (hwnd, IDC_ACTION_EDIT, CB_INSERTSTRING, 0, (LPARAM)app.LocaleString (IDS_ACTION_1, nullptr).GetString ());
			SendDlgItemMessage (hwnd, IDC_ACTION_EDIT, CB_INSERTSTRING, 1, (LPARAM)app.LocaleString (IDS_ACTION_2, nullptr).GetString ());

			if (ptr_rule)
				SendDlgItemMessage (hwnd, IDC_ACTION_EDIT, CB_SETCURSEL, (WPARAM)ptr_rule->is_block, 0);

			// state
			CheckDlgButton (hwnd, IDC_ENABLED_CHK, ptr_rule && ptr_rule->is_enabled ? BST_CHECKED : BST_UNCHECKED);

			// set limitation
			SendDlgItemMessage (hwnd, IDC_NAME_EDIT, EM_LIMITTEXT, RULE_NAME_CCH_MAX - 1, 0);
			SendDlgItemMessage (hwnd, IDC_RULES_EDIT, EM_LIMITTEXT, RULE_RULE_CCH_MAX - 1, 0);

			_r_wnd_addstyle (hwnd, IDC_SAVE, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (hwnd, IDC_CLOSE, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);

			_r_ctrl_enable (hwnd, IDC_SAVE, (SendDlgItemMessage (hwnd, IDC_NAME_EDIT, WM_GETTEXTLENGTH, 0, 0) > 0) && (SendDlgItemMessage (hwnd, IDC_RULES_EDIT, WM_GETTEXTLENGTH, 0, 0) > 0)); // enable apply button

			break;
		}

		case WM_CONTEXTMENU:
		{
			if (GetDlgCtrlID ((HWND)wparam) == IDC_FILES_LV)
			{
				const HMENU menu = LoadMenu (nullptr, MAKEINTRESOURCE (IDM_EDITOR));
				const HMENU submenu = GetSubMenu (menu, 0);

				app.LocaleMenu (submenu, IDS_CHECK, IDM_CHECK, false, nullptr);
				app.LocaleMenu (submenu, IDS_UNCHECK, IDM_UNCHECK, false, nullptr);

				if (!SendDlgItemMessage (hwnd, IDC_FILES_LV, LVM_GETSELECTEDCOUNT, 0, 0))
				{
					EnableMenuItem (submenu, IDM_CHECK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					EnableMenuItem (submenu, IDM_UNCHECK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
				}

				DeleteMenu (submenu, IDM_ADD, MF_BYCOMMAND);
				DeleteMenu (submenu, IDM_EDIT, MF_BYCOMMAND);
				DeleteMenu (submenu, IDM_DELETE, MF_BYCOMMAND);
				DeleteMenu (submenu, 0, MF_BYPOSITION);

				POINT pt = {0};
				GetCursorPos (&pt);

				TrackPopupMenuEx (submenu, TPM_RIGHTBUTTON | TPM_LEFTBUTTON, pt.x, pt.y, hwnd, nullptr);

				DestroyMenu (menu);
			}

			break;
		}

		case WM_NOTIFY:
		{
			LPNMHDR nmlp = (LPNMHDR)lparam;

			switch (nmlp->code)
			{
				case NM_CLICK:
				case NM_RETURN:
				{
					if (nmlp->idFrom == IDC_RULES_WIKI)
					{
						PNMLINK nmlink = (PNMLINK)lparam;

						if (nmlink->item.szUrl)
							ShellExecute (hwnd, nullptr, nmlink->item.szUrl, nullptr, nullptr, SW_SHOWDEFAULT);
					}

					break;
				}

				case NM_CUSTOMDRAW:
				{
					if (nmlp->idFrom != IDC_FILES_LV)
						break;

					SetWindowLongPtr (hwnd, DWLP_MSGRESULT, _app_wmcustdraw ((LPNMLVCUSTOMDRAW)lparam, 0));
					return TRUE;
				}

				case LVN_ITEMCHANGED:
				{
					LPNMLISTVIEW lpnmlv = (LPNMLISTVIEW)lparam;

					if (lpnmlv->uNewState == 8192 || lpnmlv->uNewState == 4096)
					{
						if (config.is_nocheckboxnotify)
							return FALSE;

						_app_listviewsort_appsrules (hwnd, IDC_FILES_LV);
					}

					break;
				}

				case LVN_GETINFOTIP:
				{
					LPNMLVGETINFOTIP lpnmlv = (LPNMLVGETINFOTIP)lparam;

					const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, (UINT)lpnmlv->hdr.idFrom, lpnmlv->iItem);

					if (hash)
						StringCchCopy (lpnmlv->pszText, lpnmlv->cchTextMax, _app_gettooltip (hash));

					break;
				}

				case LVN_GETEMPTYMARKUP:
				{
					NMLVEMPTYMARKUP* lpnmlv = (NMLVEMPTYMARKUP*)lparam;

					lpnmlv->dwFlags = EMF_CENTERED;
					StringCchCopy (lpnmlv->szMarkup, _countof (lpnmlv->szMarkup), app.LocaleString (IDS_STATUS_EMPTY, nullptr));

					SetWindowLongPtr (hwnd, DWLP_MSGRESULT, TRUE);
					return TRUE;
				}
			}

			break;
		}

		case WM_COMMAND:
		{
			if (HIWORD (wparam) == EN_CHANGE)
			{
				_r_ctrl_enable (hwnd, IDC_SAVE, (SendDlgItemMessage (hwnd, IDC_NAME_EDIT, WM_GETTEXTLENGTH, 0, 0) > 0) && (SendDlgItemMessage (hwnd, IDC_RULES_EDIT, WM_GETTEXTLENGTH, 0, 0) > 0)); // enable apply button
				return FALSE;
			}

			switch (LOWORD (wparam))
			{
				case IDM_CHECK:
				case IDM_UNCHECK:
				{
					const bool new_val = (LOWORD (wparam) == IDM_CHECK) ? true : false;
					size_t item = LAST_VALUE;

					while ((item = (size_t)SendDlgItemMessage (hwnd, IDC_FILES_LV, LVM_GETNEXTITEM, item, LVNI_SELECTED)) != -1)
					{
						config.is_nocheckboxnotify = true;

						_r_listview_setitemcheck (hwnd, IDC_FILES_LV, item, new_val);

						config.is_nocheckboxnotify = false;
					}

					_app_listviewsort_appsrules (hwnd, IDC_FILES_LV);

					break;
				}

				case IDOK: // process Enter key
				case IDC_SAVE:
				{
					if (!SendDlgItemMessage (hwnd, IDC_NAME_EDIT, WM_GETTEXTLENGTH, 0, 0) || !SendDlgItemMessage (hwnd, IDC_RULES_EDIT, WM_GETTEXTLENGTH, 0, 0))
						return FALSE;

					// rule destination
					{
						const rstring rule = _r_ctrl_gettext (hwnd, IDC_RULES_EDIT).Trim (L"\r\n " RULE_DELIMETER);
						const size_t rule_length = min (rule.GetLength (), RULE_RULE_CCH_MAX) + 1;

						// here we parse and check rule syntax
						{
							rstring::rvector arr = rule.AsVector (RULE_DELIMETER);

							for (size_t i = 0; i < arr.size (); i++)
							{
								rstring rule_single = arr.at (i).Trim (L"\r\n ");

								if (rule_single.IsEmpty () || rule_single.At (0) == L'*')
									continue;

								if (!_app_parserulestring (rule_single, nullptr, nullptr))
								{
									_r_ctrl_showtip (hwnd, IDC_RULES_EDIT, TTI_ERROR, APP_NAME, _r_fmt (app.LocaleString (IDS_STATUS_SYNTAX_ERROR, nullptr), rule_single.GetString ()));
									_r_ctrl_enable (hwnd, IDC_SAVE, false);

									return FALSE;
								}
							}
						}

						_r_fastlock_acquireexclusive (&lock_access);

						// save rule destination
						if (ptr_rule->prule)
						{
							delete[] ptr_rule->prule;
							ptr_rule->prule = nullptr;
						}

						ptr_rule->prule = new WCHAR[rule_length];

						if (ptr_rule->prule)
							StringCchCopy (ptr_rule->prule, rule_length, rule);
					}

					// save rule name
					{
						rstring name = _r_ctrl_gettext (hwnd, IDC_NAME_EDIT).Trim (L"\r\n " RULE_DELIMETER);

						if (!name.IsEmpty ())
						{
							const size_t name_length = min (name.GetLength (), RULE_NAME_CCH_MAX) + 1;

							if (ptr_rule->pname)
							{
								delete[] ptr_rule->pname;
								ptr_rule->pname = nullptr;
							}

							ptr_rule->pname = new WCHAR[name_length];

							if (ptr_rule->pname)
								StringCchCopy (ptr_rule->pname, name_length, name);
						}
					}

					// save rule apps
					ptr_rule->apps.clear ();

					for (size_t i = 0; i < _r_listview_getitemcount (hwnd, IDC_FILES_LV); i++)
					{
						const size_t hash = _r_listview_getitemlparam (hwnd, IDC_FILES_LV, i);

						if (hash)
						{
							const bool is_apply = _r_listview_isitemchecked (hwnd, IDC_FILES_LV, i);

							if (is_apply)
								ptr_rule->apps[hash] = true;
						}
					}

					ptr_rule->protocol = (UINT8)SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_GETITEMDATA, SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_GETCURSEL, 0, 0), 0);
					ptr_rule->version = (ADDRESS_FAMILY)SendDlgItemMessage (hwnd, IDC_PORTVERSION_EDIT, CB_GETITEMDATA, SendDlgItemMessage (hwnd, IDC_PORTVERSION_EDIT, CB_GETCURSEL, 0, 0), 0);

					ptr_rule->dir = (FWP_DIRECTION)SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_GETCURSEL, 0, 0);
					ptr_rule->is_block = SendDlgItemMessage (hwnd, IDC_ACTION_EDIT, CB_GETCURSEL, 0, 0) ? true : false;
					ptr_rule->is_enabled = (IsDlgButtonChecked (hwnd, IDC_ENABLED_CHK) == BST_CHECKED) ? true : false;

					_r_fastlock_releaseexclusive (&lock_access);

					EndDialog (hwnd, 1);

					break;
				}

				case IDCANCEL: // process Esc key
				case IDC_CLOSE:
				{
					EndDialog (hwnd, 0);
					break;
				}
			}

			break;
		}
	}

	return FALSE;
}

BOOL settings_callback (HWND hwnd, DWORD msg, LPVOID lpdata1, LPVOID lpdata2)
{
	PAPP_SETTINGS_PAGE const page = (PAPP_SETTINGS_PAGE)lpdata2;

	switch (msg)
	{
		case _RM_INITIALIZE:
		{
			switch (page->dlg_id)
			{
				case IDD_SETTINGS_GENERAL:
				{
					CheckDlgButton (hwnd, IDC_ALWAYSONTOP_CHK, app.ConfigGet (L"AlwaysOnTop", false).AsBool () ? BST_CHECKED : BST_UNCHECKED);

#ifdef _APP_HAVE_AUTORUN
					CheckDlgButton (hwnd, IDC_LOADONSTARTUP_CHK, app.AutorunIsEnabled () ? BST_CHECKED : BST_UNCHECKED);
#endif // _APP_HAVE_AUTORUN

#ifdef _APP_HAVE_SKIPUAC
					CheckDlgButton (hwnd, IDC_SKIPUACWARNING_CHK, app.SkipUacIsEnabled () ? BST_CHECKED : BST_UNCHECKED);
#endif // _APP_HAVE_SKIPUAC

					CheckDlgButton (hwnd, IDC_CHECKUPDATES_CHK, app.ConfigGet (L"CheckUpdates", true).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					if (!_app_canihaveaccess ())
						_r_ctrl_enable (hwnd, IDC_CHECKUPDATES_CHK, false);

					app.LocaleEnum (hwnd, IDC_LANGUAGE, false, 0);

					break;
				}

				case IDD_SETTINGS_RULES:
				{
					CheckDlgButton (hwnd, IDC_RULE_ALLOWINBOUND, app.ConfigGet (L"AllowInboundConnections", false).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_RULE_ALLOWLISTEN, app.ConfigGet (L"AllowListenConnections2", true).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_RULE_ALLOWLOOPBACK, app.ConfigGet (L"AllowLoopbackConnections", false).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					CheckDlgButton (hwnd, IDC_USEFULLBLOCKLIST_CHK, app.ConfigGet (L"IsExtraRulesEnabled", false).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_USESTEALTHMODE_CHK, app.ConfigGet (L"UseStealthMode", false).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_INSTALLBOOTTIMEFILTERS_CHK, app.ConfigGet (L"InstallBoottimeFilters", false).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_PROXYSUPPORT_CHK, app.ConfigGet (L"EnableProxySupport", false).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					CheckDlgButton (hwnd, IDC_USEHOSTS_CHK, app.ConfigGet (L"IsHostsEnabled", true).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_USECERTIFICATES_CHK, app.ConfigGet (L"IsCerificatesEnabled", false).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_USENETWORKRESOLUTION_CHK, app.ConfigGet (L"IsNetworkResolutionsEnabled", false).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					_r_ctrl_settip (hwnd, IDC_USEFULLBLOCKLIST_CHK, LPSTR_TEXTCALLBACK);
					_r_ctrl_settip (hwnd, IDC_USESTEALTHMODE_CHK, LPSTR_TEXTCALLBACK);
					_r_ctrl_settip (hwnd, IDC_INSTALLBOOTTIMEFILTERS_CHK, LPSTR_TEXTCALLBACK);
					_r_ctrl_settip (hwnd, IDC_PROXYSUPPORT_CHK, LPSTR_TEXTCALLBACK);

					if (!_app_canihaveaccess ())
					{
						_r_ctrl_enable (hwnd, IDC_USEHOSTS_CHK, false);
						_r_ctrl_enable (hwnd, IDC_USENETWORKRESOLUTION_CHK, false);
					}

					if (!_r_sys_validversion (6, 2))
						_r_ctrl_enable (hwnd, IDC_PROXYSUPPORT_CHK, false);

					break;
				}

				case IDD_SETTINGS_HIGHLIGHTING:
				{
					// configure listview
					_r_listview_setstyle (hwnd, IDC_COLORS, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);

					_app_listviewsetimagelist (hwnd, IDC_COLORS);

					_r_listview_deleteallitems (hwnd, IDC_COLORS);
					_r_listview_deleteallcolumns (hwnd, IDC_COLORS);

					_r_listview_addcolumn (hwnd, IDC_COLORS, 0, app.LocaleString (IDS_NAME, nullptr), 100, LVCFMT_LEFT);

					{
						for (size_t i = 0; i < colors.size (); i++)
						{
							PITEM_COLOR ptr_clr = &colors.at (i);

							if (ptr_clr)
							{
								ptr_clr->clr = app.ConfigGet (ptr_clr->config_value, ptr_clr->default_clr).AsUlong ();

								config.is_nocheckboxnotify = true;

								_r_listview_additem (hwnd, IDC_COLORS, i, 0, app.LocaleString (ptr_clr->locale_id, nullptr), config.icon_id, LAST_VALUE, i);
								_r_listview_setitemcheck (hwnd, IDC_COLORS, i, app.ConfigGet (ptr_clr->config_name, ptr_clr->is_enabled).AsBool ());

								config.is_nocheckboxnotify = false;
							}
						}
					}

					break;
				}

				case IDD_SETTINGS_RULES_BLOCKLIST:
				case IDD_SETTINGS_RULES_SYSTEM:
				case IDD_SETTINGS_RULES_CUSTOM:
				{
					// configure listview
					_r_listview_setstyle (hwnd, IDC_EDITOR, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);

					SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)config.himg);
					SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_SETIMAGELIST, LVSIL_NORMAL, (LPARAM)config.himg);

					_r_listview_deleteallitems (hwnd, IDC_EDITOR);
					_r_listview_deleteallgroups (hwnd, IDC_EDITOR);
					_r_listview_deleteallcolumns (hwnd, IDC_EDITOR);

					_r_listview_addcolumn (hwnd, IDC_EDITOR, 0, app.LocaleString (IDS_NAME, nullptr), 49, LVCFMT_LEFT);
					_r_listview_addcolumn (hwnd, IDC_EDITOR, 1, app.LocaleString (IDS_DIRECTION, nullptr), 26, LVCFMT_LEFT);
					_r_listview_addcolumn (hwnd, IDC_EDITOR, 2, app.LocaleString (IDS_PROTOCOL, nullptr), 20, LVCFMT_LEFT);

					_r_listview_addgroup (hwnd, IDC_EDITOR, 0, nullptr, 0, LVGS_COLLAPSIBLE);
					_r_listview_addgroup (hwnd, IDC_EDITOR, 1, nullptr, 0, LVGS_COLLAPSIBLE);
					_r_listview_addgroup (hwnd, IDC_EDITOR, 2, nullptr, 0, LVGS_COLLAPSIBLE);

					_app_listviewsetfont (hwnd, IDC_EDITOR);

					std::vector<PITEM_RULE> const* ptr_rules = nullptr;

					if (page->dlg_id == IDD_SETTINGS_RULES_BLOCKLIST)
						ptr_rules = &rules_blocklist;

					else if (page->dlg_id == IDD_SETTINGS_RULES_SYSTEM)
						ptr_rules = &rules_system;

					else if (page->dlg_id == IDD_SETTINGS_RULES_CUSTOM)
						ptr_rules = &rules_custom;

					if (ptr_rules && !ptr_rules->empty ())
					{
						for (size_t i = 0, item = 0; i < ptr_rules->size (); i++)
						{
							PITEM_RULE const ptr_rule = ptr_rules->at (i);

							if (!ptr_rule)
								continue;

							size_t group_id = 2;

							if (ptr_rule->is_enabled)
								group_id = ptr_rule->apps.empty () ? 0 : 1;

							config.is_nocheckboxnotify = true;

							_r_listview_additem (hwnd, IDC_EDITOR, item, 0, ptr_rule->pname, ptr_rule->is_block ? 1 : 0, group_id, i);
							_r_listview_setitemcheck (hwnd, IDC_EDITOR, item, ptr_rule->is_enabled);

							item += 1;

							config.is_nocheckboxnotify = false;
						}
					}

					break;
				}

				case IDD_SETTINGS_LOG:
				{
					CheckDlgButton (hwnd, IDC_ENABLELOG_CHK, app.ConfigGet (L"IsLogEnabled", false).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					SetDlgItemText (hwnd, IDC_LOGPATH, app.ConfigGet (L"LogPath", PATH_LOG));

					UDACCEL ud = {0};
					ud.nInc = 64; // set step to 64kb

					SendDlgItemMessage (hwnd, IDC_LOGSIZELIMIT, UDM_SETACCEL, 1, (LPARAM)&ud);
					SendDlgItemMessage (hwnd, IDC_LOGSIZELIMIT, UDM_SETRANGE32, 64, 2048);
					SendDlgItemMessage (hwnd, IDC_LOGSIZELIMIT, UDM_SETPOS32, 0, app.ConfigGet (L"LogSizeLimitKb", 256).AsUint ());

					CheckDlgButton (hwnd, IDC_ENABLENOTIFICATIONS_CHK, app.ConfigGet (L"IsNotificationsEnabled", true).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_NOTIFICATIONSOUND_CHK, app.ConfigGet (L"IsNotificationsSound", true).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_NOTIFICATIONNOBLOCKLIST_CHK, app.ConfigGet (L"IsNotificationsExcludeBlocklist", true).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					SendDlgItemMessage (hwnd, IDC_NOTIFICATIONDISPLAYTIMEOUT, UDM_SETRANGE32, 0, _R_SECONDSCLOCK_HOUR (1));
					SendDlgItemMessage (hwnd, IDC_NOTIFICATIONDISPLAYTIMEOUT, UDM_SETPOS32, 0, app.ConfigGet (L"NotificationsDisplayTimeout", NOTIFY_TIMER_DEFAULT).AsUint ());

					SendDlgItemMessage (hwnd, IDC_NOTIFICATIONTIMEOUT, UDM_SETRANGE32, NOTIFY_TIMEOUT_MINIMUM, _R_SECONDSCLOCK_HOUR (1));
					SendDlgItemMessage (hwnd, IDC_NOTIFICATIONTIMEOUT, UDM_SETPOS32, 0, app.ConfigGet (L"NotificationsTimeout", NOTIFY_TIMEOUT).AsUint ());

					// dropped packets logging (win7+)
					if (!_r_sys_validversion (6, 1))
					{
						_r_ctrl_enable (hwnd, IDC_ENABLELOG_CHK, false);
						_r_ctrl_enable (hwnd, IDC_ENABLENOTIFICATIONS_CHK, false);
					}
					else
					{
						PostMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDC_ENABLELOG_CHK, 0), 0);
						PostMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDC_ENABLENOTIFICATIONS_CHK, 0), 0);
					}

					break;
				}

				case IDD_SETTINGS_SYSTEM:
				{
					//CheckDlgButton (hwnd, IDC_SYSTEMDISABLENCSI_CHK, app.ConfigGet (L"IsLogEnabled", false).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					HKEY hkey = nullptr;

					if (RegOpenKeyEx (HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\services\\NlaSvc\\Parameters\\Internet", 0, KEY_READ, &hkey) == ERROR_SUCCESS)
					{
						DWORD value = 0;
						DWORD size = sizeof (DWORD);

						if (RegQueryValueEx (hkey, L"EnableActiveProbing", nullptr, nullptr, (LPBYTE)&value, &size) == ERROR_SUCCESS)
							CheckDlgButton (hwnd, IDC_SYSTEMDISABLENCSI_CHK, !value ? BST_CHECKED : BST_UNCHECKED);

						RegCloseKey (hkey);
					}

					_r_ctrl_settip (hwnd, IDC_SYSTEMDISABLENCSI_CHK, LPSTR_TEXTCALLBACK);

					break;
				}
			}

			break;
		}

		case _RM_LOCALIZE:
		{
			// localize titles
			SetDlgItemText (hwnd, IDC_TITLE_GENERAL, app.LocaleString (IDS_TITLE_GENERAL, L":"));
			SetDlgItemText (hwnd, IDC_TITLE_LANGUAGE, app.LocaleString (IDS_TITLE_LANGUAGE, L": (Language)"));
			SetDlgItemText (hwnd, IDC_TITLE_CONFIRMATIONS, app.LocaleString (IDS_TITLE_CONFIRMATIONS, L":"));
			SetDlgItemText (hwnd, IDC_TITLE_HIGHLIGHTING, app.LocaleString (IDS_TITLE_HIGHLIGHTING, L":"));
			SetDlgItemText (hwnd, IDC_TITLE_EXPERTS, app.LocaleString (IDS_TITLE_EXPERTS, L":"));
			SetDlgItemText (hwnd, IDC_TITLE_LOGGING, app.LocaleString (IDS_TITLE_LOGGING, L": (win7+)"));
			SetDlgItemText (hwnd, IDC_TITLE_NOTIFICATIONS, app.LocaleString (IDS_TITLE_NOTIFICATIONS, L": (win7+)"));
			SetDlgItemText (hwnd, IDC_TITLE_ADVANCED, app.LocaleString (IDS_TITLE_ADVANCED, L":"));

			switch (page->dlg_id)
			{
				case IDD_SETTINGS_GENERAL:
				{
					SetDlgItemText (hwnd, IDC_ALWAYSONTOP_CHK, app.LocaleString (IDS_ALWAYSONTOP_CHK, nullptr));
					SetDlgItemText (hwnd, IDC_LOADONSTARTUP_CHK, app.LocaleString (IDS_LOADONSTARTUP_CHK, nullptr));
					SetDlgItemText (hwnd, IDC_SKIPUACWARNING_CHK, app.LocaleString (IDS_SKIPUACWARNING_CHK, nullptr));
					SetDlgItemText (hwnd, IDC_CHECKUPDATES_CHK, app.LocaleString (IDS_CHECKUPDATES_CHK, nullptr));

					SetDlgItemText (hwnd, IDC_LANGUAGE_HINT, app.LocaleString (IDS_LANGUAGE_HINT, nullptr));

					break;
				}

				case IDD_SETTINGS_HIGHLIGHTING:
				{
					SetDlgItemText (hwnd, IDC_COLORS_HINT, app.LocaleString (IDS_COLORS_HINT, nullptr));

					for (size_t i = 0; i < _r_listview_getitemcount (hwnd, IDC_COLORS); i++)
					{
						const size_t idx = _r_listview_getitemlparam (hwnd, IDC_COLORS, i);

						PITEM_COLOR const ptr_clr = &colors.at (idx);

						if (ptr_clr)
							_r_listview_setitem (hwnd, IDC_COLORS, i, 0, app.LocaleString (ptr_clr->locale_id, nullptr));
					}

					_app_listviewsetfont (hwnd, IDC_COLORS);

					break;
				}

				case IDD_SETTINGS_RULES:
				{
					SetDlgItemText (hwnd, IDC_RULE_ALLOWINBOUND, app.LocaleString (IDS_RULE_ALLOWINBOUND, nullptr));
					SetDlgItemText (hwnd, IDC_RULE_ALLOWLISTEN, app.LocaleString (IDS_RULE_ALLOWLISTEN, nullptr));
					SetDlgItemText (hwnd, IDC_RULE_ALLOWLOOPBACK, app.LocaleString (IDS_RULE_ALLOWLOOPBACK, nullptr));

					SetDlgItemText (hwnd, IDC_USEFULLBLOCKLIST_CHK, app.LocaleString (IDS_USEFULLBLOCKLIST_CHK, L"*"));
					SetDlgItemText (hwnd, IDC_USESTEALTHMODE_CHK, app.LocaleString (IDS_USESTEALTHMODE_CHK, L"*"));
					SetDlgItemText (hwnd, IDC_INSTALLBOOTTIMEFILTERS_CHK, app.LocaleString (IDS_INSTALLBOOTTIMEFILTERS_CHK, L"*"));
					SetDlgItemText (hwnd, IDC_PROXYSUPPORT_CHK, app.LocaleString (IDS_PROXYSUPPORT_CHK, L"* (win8+) [BETA]"));

					SetDlgItemText (hwnd, IDC_USEHOSTS_CHK, app.LocaleString (IDS_USEHOSTS_CHK, nullptr));
					SetDlgItemText (hwnd, IDC_USECERTIFICATES_CHK, app.LocaleString (IDS_USECERTIFICATES_CHK, 0));
					SetDlgItemText (hwnd, IDC_USENETWORKRESOLUTION_CHK, app.LocaleString (IDS_USENETWORKRESOLUTION_CHK, 0));

					break;
				}

				case IDD_SETTINGS_RULES_BLOCKLIST:
				case IDD_SETTINGS_RULES_SYSTEM:
				case IDD_SETTINGS_RULES_CUSTOM:
				{
					_r_listview_setcolumn (hwnd, IDC_EDITOR, 0, app.LocaleString (IDS_NAME, nullptr), 0);
					_r_listview_setcolumn (hwnd, IDC_EDITOR, 1, app.LocaleString (IDS_DIRECTION, nullptr), 0);
					_r_listview_setcolumn (hwnd, IDC_EDITOR, 2, app.LocaleString (IDS_PROTOCOL, nullptr), 0);

					std::vector<PITEM_RULE> const* ptr_rules = nullptr;

					_r_fastlock_acquireshared (&lock_access);

					if (page->dlg_id == IDD_SETTINGS_RULES_BLOCKLIST)
						ptr_rules = &rules_blocklist;

					else if (page->dlg_id == IDD_SETTINGS_RULES_SYSTEM)
						ptr_rules = &rules_system;

					else if (page->dlg_id == IDD_SETTINGS_RULES_CUSTOM)
						ptr_rules = &rules_custom;

					if (ptr_rules && !ptr_rules->empty ())
					{
						const size_t total_count = _r_listview_getitemcount (hwnd, IDC_EDITOR);
						size_t group1_count = 0;
						size_t group2_count = 0;
						size_t group3_count = 0;

						for (size_t i = 0; i < total_count; i++)
						{
							const size_t idx = _r_listview_getitemlparam (hwnd, IDC_EDITOR, i);
							PITEM_RULE const ptr_rule = ptr_rules->at (idx);

							if (!ptr_rule)
								continue;

							rstring protocol = app.LocaleString (IDS_ALL, nullptr);

							// protocol
							if (ptr_rule->protocol)
							{
								for (size_t j = 0; j < protocols.size (); j++)
								{
									if (ptr_rule->protocol == protocols.at (j).id)
										protocol = protocols.at (j).name;
								}
							}

							size_t group_id = 2;

							if (ptr_rule->is_enabled && ptr_rule->apps.empty ())
							{
								group_id = 0;
								group1_count += 1;
							}
							else if (ptr_rule->is_enabled && !ptr_rule->apps.empty ())
							{
								group_id = 1;
								group2_count += 1;
							}
							else
							{
								group3_count += 1;
							}

							config.is_nocheckboxnotify = true;

							_r_listview_setitem (hwnd, IDC_EDITOR, i, 0, ptr_rule->pname, ptr_rule->is_block ? 1 : 0, group_id);
							_r_listview_setitem (hwnd, IDC_EDITOR, i, 1, app.LocaleString (IDS_DIRECTION_1 + ptr_rule->dir, nullptr));
							_r_listview_setitem (hwnd, IDC_EDITOR, i, 2, protocol);

							_r_listview_setitemcheck (hwnd, IDC_EDITOR, i, ptr_rule->is_enabled);

							config.is_nocheckboxnotify = false;
						}

						_r_listview_setgroup (hwnd, IDC_EDITOR, 0, app.LocaleString (IDS_GROUP_ENABLED, _r_fmt (L" (%d/%d)", group1_count, total_count)), 0, 0);
						_r_listview_setgroup (hwnd, IDC_EDITOR, 1, app.LocaleString (IDS_GROUP_SPECIAL, _r_fmt (L" (%d/%d)", group2_count, total_count)), 0, 0);
						_r_listview_setgroup (hwnd, IDC_EDITOR, 2, app.LocaleString (IDS_GROUP_DISABLED, _r_fmt (L" (%d/%d)", group3_count, total_count)), 0, 0);
					}

					_r_fastlock_releaseshared (&lock_access);

					_app_listviewsort_rules (hwnd, IDC_EDITOR);
					_r_listview_redraw (hwnd, IDC_EDITOR);

					SetDlgItemText (hwnd, IDC_RULES_BLOCKLIST_HINT, app.LocaleString (IDS_RULES_BLOCKLIST_HINT, nullptr));
					SetDlgItemText (hwnd, IDC_RULES_SYSTEM_HINT, app.LocaleString (IDS_RULES_SYSTEM_HINT, nullptr));
					_r_ctrl_settext (hwnd, IDC_RULES_CUSTOM_HINT, app.LocaleString (IDS_RULES_CUSTOM_HINT, nullptr), WIKI_URL);

					SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_RESETEMPTYTEXT, 0, 0);

					break;
				}

				case IDD_SETTINGS_LOG:
				{
					SetDlgItemText (hwnd, IDC_ENABLELOG_CHK, app.LocaleString (IDS_ENABLELOG_CHK, nullptr));

					SetDlgItemText (hwnd, IDC_LOGSIZELIMIT_HINT, app.LocaleString (IDS_LOGSIZELIMIT_HINT, nullptr));

					SetDlgItemText (hwnd, IDC_ENABLENOTIFICATIONS_CHK, app.LocaleString (IDS_ENABLENOTIFICATIONS_CHK, nullptr));
					SetDlgItemText (hwnd, IDC_NOTIFICATIONSOUND_CHK, app.LocaleString (IDS_NOTIFICATIONSOUND_CHK, nullptr));
					SetDlgItemText (hwnd, IDC_NOTIFICATIONNOBLOCKLIST_CHK, app.LocaleString (IDS_NOTIFICATIONNOBLOCKLIST_CHK, nullptr));

					SetDlgItemText (hwnd, IDC_NOTIFICATIONDISPLAYTIMEOUT_HINT, app.LocaleString (IDS_NOTIFICATIONDISPLAYTIMEOUT_HINT, nullptr));
					SetDlgItemText (hwnd, IDC_NOTIFICATIONTIMEOUT_HINT, app.LocaleString (IDS_NOTIFICATIONTIMEOUT_HINT, nullptr));

					_r_wnd_addstyle (hwnd, IDC_LOGPATH_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);

					break;
				}

				case IDD_SETTINGS_SYSTEM:
				{
					SetDlgItemText (hwnd, IDC_SYSTEMDISABLENCSI_CHK, app.LocaleString (IDS_SYSTEMDISABLENCSI_CHK, L"*"));

					break;
				}
			}

			break;
		}

		case _RM_MESSAGE:
		{
			LPMSG pmsg = (LPMSG)lpdata1;

			switch (pmsg->message)
			{
				case WM_VSCROLL:
				case WM_HSCROLL:
				{
					if (page->dlg_id == IDD_SETTINGS_LOG)
					{
						const UINT ctrl_id = GetDlgCtrlID ((HWND)pmsg->lParam);

						if (ctrl_id == IDC_LOGSIZELIMIT)
							app.ConfigSet (L"LogSizeLimitKb", (DWORD)SendDlgItemMessage (hwnd, ctrl_id, UDM_GETPOS32, 0, 0));

						else if (ctrl_id == IDC_NOTIFICATIONDISPLAYTIMEOUT)
							app.ConfigSet (L"NotificationsDisplayTimeout", (DWORD)SendDlgItemMessage (hwnd, ctrl_id, UDM_GETPOS32, 0, 0));

						else if (ctrl_id == IDC_NOTIFICATIONTIMEOUT)
							app.ConfigSet (L"NotificationsTimeout", (DWORD)SendDlgItemMessage (hwnd, ctrl_id, UDM_GETPOS32, 0, 0));
					}

					break;
				}

				case WM_NOTIFY:
				{
					LPNMHDR nmlp = (LPNMHDR)pmsg->lParam;

					switch (nmlp->code)
					{
						case TTN_GETDISPINFO:
						{
							LPNMTTDISPINFO lpnmdi = (LPNMTTDISPINFO)pmsg->lParam;

							if ((lpnmdi->uFlags & TTF_IDISHWND) != 0)
							{
								WCHAR buffer[1024] = {0};
								const UINT ctrl_id = GetDlgCtrlID ((HWND)lpnmdi->hdr.idFrom);

								if (ctrl_id == IDC_USEFULLBLOCKLIST_CHK)
									StringCchCopy (buffer, _countof (buffer), app.LocaleString (IDS_USEFULLBLOCKLIST_HINT, nullptr));

								else if (ctrl_id == IDC_USESTEALTHMODE_CHK)
									StringCchCopy (buffer, _countof (buffer), app.LocaleString (IDS_USESTEALTHMODE_HINT, nullptr));

								else if (ctrl_id == IDC_INSTALLBOOTTIMEFILTERS_CHK)
									StringCchCopy (buffer, _countof (buffer), app.LocaleString (IDS_INSTALLBOOTTIMEFILTERS_HINT, nullptr));

								else if (ctrl_id == IDC_PROXYSUPPORT_CHK)
									StringCchCopy (buffer, _countof (buffer), app.LocaleString (IDS_PROXYSUPPORT_HINT, nullptr));

								else if (ctrl_id == IDC_SYSTEMDISABLENCSI_CHK)
									StringCchCopy (buffer, _countof (buffer), app.LocaleString (IDS_SYSTEMDISABLENCSI_HINT_CHK, nullptr));

								if (buffer[0])
									lpnmdi->lpszText = buffer;
							}

							break;
						}

						case LVN_ITEMCHANGED:
						{
							if (config.is_nocheckboxnotify)
								break;

							LPNMLISTVIEW lpnmlv = (LPNMLISTVIEW)pmsg->lParam;

							if (lpnmlv->uNewState == 8192 || lpnmlv->uNewState == 4096)
							{
								const bool new_val = (lpnmlv->uNewState == 8192) ? true : false;
								const size_t idx = lpnmlv->lParam;

								if (nmlp->idFrom == IDC_COLORS)
								{
									PITEM_COLOR ptr_clr = &colors.at (idx);

									if (ptr_clr)
									{
										app.ConfigSet (ptr_clr->config_name, new_val);

										_r_listview_redraw (app.GetHWND (), IDC_LISTVIEW);
									}
								}
								else if (nmlp->idFrom == IDC_EDITOR)
								{
									std::vector<PITEM_RULE> const* ptr_rules = nullptr;

									if (page->dlg_id == IDD_SETTINGS_RULES_BLOCKLIST)
										ptr_rules = &rules_blocklist;

									else if (page->dlg_id == IDD_SETTINGS_RULES_SYSTEM)
										ptr_rules = &rules_system;

									else if (page->dlg_id == IDD_SETTINGS_RULES_CUSTOM)
										ptr_rules = &rules_custom;

									if (ptr_rules && !ptr_rules->empty ())
									{
										PITEM_RULE ptr_rule = ptr_rules->at (idx);

										if (ptr_rule)
										{
											ptr_rule->is_enabled = new_val;

											if (
												page->dlg_id == IDD_SETTINGS_RULES_BLOCKLIST ||
												page->dlg_id == IDD_SETTINGS_RULES_SYSTEM
												)
											{
												if (ptr_rule->pname)
													rules_config[ptr_rule->pname] = new_val;
											}

											_app_profilesave (app.GetHWND ());

											settings_callback (hwnd, _RM_LOCALIZE, nullptr, page);

											_app_installfilters (false);
										}
									}
								}
							}

							break;
						}

						case LVN_GETINFOTIP:
						{
							const UINT ctrl_id = (UINT)nmlp->idFrom;

							if (ctrl_id != IDC_EDITOR)
								break;

							LPNMLVGETINFOTIP lpnmlv = (LPNMLVGETINFOTIP)pmsg->lParam;

							PITEM_RULE ptr_rule = nullptr;

							_r_fastlock_acquireshared (&lock_access);

							const size_t idx = _r_listview_getitemlparam (hwnd, ctrl_id, lpnmlv->iItem);

							if (page->dlg_id == IDD_SETTINGS_RULES_BLOCKLIST)
								ptr_rule = rules_blocklist.at (idx);

							else if (page->dlg_id == IDD_SETTINGS_RULES_SYSTEM)
								ptr_rule = rules_system.at (idx);

							else if (page->dlg_id == IDD_SETTINGS_RULES_CUSTOM)
								ptr_rule = rules_custom.at (idx);

							if (ptr_rule)
							{
								rstring rule = ptr_rule->prule;

								if (rule.IsEmpty ())
									rule = app.LocaleString (IDS_STATUS_EMPTY, nullptr);

								else
									rule.Replace (RULE_DELIMETER, L"\r\n" TAB_SPACE);

								StringCchPrintf (lpnmlv->pszText, lpnmlv->cchTextMax, L"%s (#%zu)\r\n%s:\r\n%s%s", ptr_rule->pname, idx, app.LocaleString (IDS_RULE, nullptr).GetString (), TAB_SPACE, rule.GetString ());

								if (!ptr_rule->apps.empty ())
									StringCchCat (lpnmlv->pszText, lpnmlv->cchTextMax, _r_fmt (L"\r\n%s:\r\n%s%s", app.LocaleString (IDS_FILEPATH, nullptr).GetString (), TAB_SPACE, _app_rulesexpand (ptr_rule).GetString ()));
							}

							_r_fastlock_releaseshared (&lock_access);

							break;
						}

						case NM_CUSTOMDRAW:
						{
							if (nmlp->idFrom == IDC_COLORS || nmlp->idFrom == IDC_EDITOR)
							{
								LPNMLVCUSTOMDRAW lpnmlv = (LPNMLVCUSTOMDRAW)pmsg->lParam;
								LPARAM lparam = 0;

								if (lpnmlv->nmcd.hdr.idFrom == IDC_EDITOR && lpnmlv->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
								{
									const size_t idx = lpnmlv->nmcd.lItemlParam;
									PITEM_RULE ptr_rule = nullptr;

									_r_fastlock_acquireshared (&lock_access);

									if (page->dlg_id == IDD_SETTINGS_RULES_BLOCKLIST)
										ptr_rule = rules_blocklist.at (idx);

									else if (page->dlg_id == IDD_SETTINGS_RULES_SYSTEM)
										ptr_rule = rules_system.at (idx);

									else if (page->dlg_id == IDD_SETTINGS_RULES_CUSTOM)
										ptr_rule = rules_custom.at (idx);

									if (ptr_rule)
									{
										if (ptr_rule->is_haveerrors)
											lparam = (LPARAM)_app_getcolorvalue (_r_str_hash (L"ColorInvalid"), false);

										else if (!ptr_rule->apps.empty ())
											lparam = (LPARAM)_app_getcolorvalue (_r_str_hash (L"ColorSpecial"), false);
									}

									_r_fastlock_releaseshared (&lock_access);
								}

								SetWindowLongPtr (hwnd, DWLP_MSGRESULT, _app_wmcustdraw (lpnmlv, lparam));
								return TRUE;
							}

							break;
						}

						case LVN_GETEMPTYMARKUP:
						{
							NMLVEMPTYMARKUP* lpnmlv = (NMLVEMPTYMARKUP*)pmsg->lParam;

							lpnmlv->dwFlags = EMF_CENTERED;
							StringCchCopy (lpnmlv->szMarkup, _countof (lpnmlv->szMarkup), app.LocaleString (IDS_STATUS_EMPTY, nullptr));

							SetWindowLongPtr (hwnd, DWLP_MSGRESULT, TRUE);
							return TRUE;
						}

						case NM_DBLCLK:
						{
							LPNMITEMACTIVATE lpnmlv = (LPNMITEMACTIVATE)pmsg->lParam;

							if (lpnmlv->iItem == -1)
								break;

							if (nmlp->idFrom == IDC_COLORS)
							{
								const size_t idx = _r_listview_getitemlparam (hwnd, IDC_COLORS, lpnmlv->iItem);

								CHOOSECOLOR cc = {0};
								COLORREF cust[16] = {0};

								for (size_t i = 0; i < min (_countof (cust), colors.size ()); i++)
									cust[i] = colors.at (i).default_clr;

								cc.lStructSize = sizeof (cc);
								cc.Flags = CC_RGBINIT | CC_FULLOPEN;
								cc.hwndOwner = hwnd;
								cc.lpCustColors = cust;
								cc.rgbResult = colors.at (idx).clr;

								if (ChooseColor (&cc))
								{
									PITEM_COLOR ptr_clr = &colors.at (idx);

									if (ptr_clr)
									{
										if (ptr_clr->hbrush)
										{
											DeleteObject (ptr_clr->hbrush);
											ptr_clr->hbrush = nullptr;
										}

										ptr_clr->clr = cc.rgbResult;
										ptr_clr->hbrush = CreateSolidBrush (cc.rgbResult);

										app.ConfigSet (ptr_clr->config_value, cc.rgbResult);

										_r_listview_redraw (hwnd, IDC_COLORS);
										_r_listview_redraw (app.GetHWND (), IDC_LISTVIEW);
									}
								}
							}
							else if (nmlp->idFrom == IDC_EDITOR)
							{
								PostMessage (hwnd, WM_COMMAND, MAKELPARAM (IDM_EDIT, 0), 0);
							}

							break;
						}

						case NM_CLICK:
						case NM_RETURN:
						{
							if (nmlp->idFrom == IDC_RULES_BLOCKLIST_HINT || nmlp->idFrom == IDC_RULES_CUSTOM_HINT)
							{
								PNMLINK nmlink = (PNMLINK)pmsg->lParam;

								if (nmlink->item.szUrl)
									ShellExecute (hwnd, nullptr, nmlink->item.szUrl, nullptr, nullptr, SW_SHOWDEFAULT);
							}

							break;
						}
					}

					break;
				}

				case WM_CONTEXTMENU:
				{
					const UINT ctrl_id = GetDlgCtrlID ((HWND)pmsg->wParam);

					if (ctrl_id != IDC_EDITOR)
						break;

					const HMENU menu = LoadMenu (nullptr, MAKEINTRESOURCE (IDM_EDITOR));
					const HMENU submenu = GetSubMenu (menu, 0);

					// localize
					app.LocaleMenu (submenu, IDS_CHECK, IDM_CHECK, false, nullptr);
					app.LocaleMenu (submenu, IDS_UNCHECK, IDM_UNCHECK, false, nullptr);

					if (!SendDlgItemMessage (hwnd, ctrl_id, LVM_GETSELECTEDCOUNT, 0, 0))
					{
						EnableMenuItem (submenu, IDM_EDIT, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
						EnableMenuItem (submenu, IDM_DELETE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
						EnableMenuItem (submenu, IDM_CHECK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
						EnableMenuItem (submenu, IDM_UNCHECK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					}

					if (page->dlg_id == IDD_SETTINGS_RULES_CUSTOM)
					{
						app.LocaleMenu (submenu, IDS_ADD, IDM_ADD, false, L"...");
						app.LocaleMenu (submenu, IDS_EDIT2, IDM_EDIT, false, L"...");
						app.LocaleMenu (submenu, IDS_DELETE, IDM_DELETE, false, nullptr);
					}
					else
					{
						DeleteMenu (submenu, IDM_ADD, MF_BYCOMMAND);
						DeleteMenu (submenu, IDM_EDIT, MF_BYCOMMAND);
						DeleteMenu (submenu, IDM_DELETE, MF_BYCOMMAND);
						DeleteMenu (submenu, 0, MF_BYPOSITION);
					}

					POINT pt = {0};
					GetCursorPos (&pt);

					TrackPopupMenuEx (submenu, TPM_RIGHTBUTTON | TPM_LEFTBUTTON, pt.x, pt.y, hwnd, nullptr);

					DestroyMenu (menu);

					break;
				}

				case WM_COMMAND:
				{
					switch (LOWORD (pmsg->wParam))
					{
						case IDC_ALWAYSONTOP_CHK:
						case IDC_LOADONSTARTUP_CHK:
						case IDC_SKIPUACWARNING_CHK:
						case IDC_CHECKUPDATES_CHK:
						case IDC_LANGUAGE:
						case IDC_USEFULLBLOCKLIST_CHK:
						case IDC_USESTEALTHMODE_CHK:
						case IDC_INSTALLBOOTTIMEFILTERS_CHK:
						case IDC_PROXYSUPPORT_CHK:
						case IDC_USEHOSTS_CHK:
						case IDC_USECERTIFICATES_CHK:
						case IDC_USENETWORKRESOLUTION_CHK:
						case IDC_RULE_ALLOWINBOUND:
						case IDC_RULE_ALLOWLISTEN:
						case IDC_RULE_ALLOWLOOPBACK:
						case IDC_ENABLELOG_CHK:
						case IDC_LOGPATH:
						case IDC_LOGPATH_BTN:
						case IDC_LOGSIZELIMIT_CTRL:
						case IDC_ENABLENOTIFICATIONS_CHK:
						case IDC_NOTIFICATIONSOUND_CHK:
						case IDC_NOTIFICATIONNOBLOCKLIST_CHK:
						case IDC_NOTIFICATIONDISPLAYTIMEOUT_CTRL:
						case IDC_NOTIFICATIONTIMEOUT_CTRL:
						case IDC_SYSTEMDISABLENCSI_CHK:
						{
							const UINT ctrl_id = LOWORD (pmsg->wParam);
							const UINT notify_code = HIWORD (pmsg->wParam);

							if (ctrl_id == IDC_ALWAYSONTOP_CHK)
							{
								app.ConfigSet (L"AlwaysOnTop", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED) ? true : false);
								CheckMenuItem (GetMenu (app.GetHWND ()), IDM_ALWAYSONTOP_CHK, MF_BYCOMMAND | ((IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED) ? MF_CHECKED : MF_UNCHECKED));
							}
							else if (ctrl_id == IDC_LOADONSTARTUP_CHK)
							{
								app.AutorunEnable (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED);
							}
							else if (ctrl_id == IDC_SKIPUACWARNING_CHK)
							{
								app.SkipUacEnable (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED);
							}
							else if (ctrl_id == IDC_CHECKUPDATES_CHK)
							{
								app.ConfigSet (L"CheckUpdates", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED) ? true : false);
							}
							else if (ctrl_id == IDC_LANGUAGE && notify_code == CBN_SELCHANGE)
							{
								app.LocaleApplyFromControl (hwnd, ctrl_id);
							}
							else if (ctrl_id == IDC_USEFULLBLOCKLIST_CHK)
							{
								if (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED && !app.ConfirmMessage (hwnd, nullptr, app.LocaleString (IDS_QUESTION_EXPERT, nullptr), L"ConfirmExpert"))
								{
									CheckDlgButton (hwnd, ctrl_id, BST_UNCHECKED);
									return TRUE;
								}

								app.ConfigSet (L"IsExtraRulesEnabled", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED) ? true : false);

								_app_loadrules (hwnd, _r_fmt (L"%s\\" XML_BLOCKLIST, app.GetProfileDirectory ()), MAKEINTRESOURCE (IDR_RULES_BLOCKLIST), true, &rules_blocklist);

								app.SettingsPageInitialize (IDD_SETTINGS_RULES_BLOCKLIST, true, false); // re-inititalize page

								_app_installfilters (false);
							}
							else if (ctrl_id == IDC_USESTEALTHMODE_CHK)
							{
								if (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED && !app.ConfirmMessage (hwnd, nullptr, app.LocaleString (IDS_QUESTION_EXPERT, nullptr), L"ConfirmExpert"))
								{
									CheckDlgButton (hwnd, ctrl_id, BST_UNCHECKED);
									return TRUE;
								}

								app.ConfigSet (L"UseStealthMode", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED) ? true : false);

								_app_installfilters (false);
							}
							else if (ctrl_id == IDC_INSTALLBOOTTIMEFILTERS_CHK)
							{
								if (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED && !app.ConfirmMessage (hwnd, nullptr, app.LocaleString (IDS_QUESTION_EXPERT, nullptr), L"ConfirmExpert"))
								{
									CheckDlgButton (hwnd, ctrl_id, BST_UNCHECKED);
									return TRUE;
								}

								app.ConfigSet (L"InstallBoottimeFilters", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED) ? true : false);

								_app_installfilters (false);
							}
							else if (ctrl_id == IDC_PROXYSUPPORT_CHK)
							{
								if (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED && !app.ConfirmMessage (hwnd, nullptr, app.LocaleString (IDS_QUESTION_EXPERT, nullptr), L"ConfirmExpert"))
								{
									CheckDlgButton (hwnd, ctrl_id, BST_UNCHECKED);
									return TRUE;
								}

								app.ConfigSet (L"EnableProxySupport", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED) ? true : false);

								_app_installfilters (false);
							}
							else if (ctrl_id == IDC_USEHOSTS_CHK)
							{
								app.ConfigSet (L"IsHostsEnabled", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED) ? true : false);

								_app_installfilters (false);
							}
							else if (ctrl_id == IDC_USECERTIFICATES_CHK)
							{
								app.ConfigSet (L"IsCerificatesEnabled", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED) ? true : false);

								if (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED)
								{
									_r_fastlock_acquireexclusive (&lock_access);

									for (auto &p : apps)
									{
										PITEM_APP ptr_app = &p.second;

										if (ptr_app->type == AppRegular)
											ptr_app->is_signed = _app_verifysignature (p.first, ptr_app->real_path, &ptr_app->signer);
									}

									_r_fastlock_releaseexclusive (&lock_access);
								}

								_r_listview_redraw (app.GetHWND (), IDC_LISTVIEW);
							}
							else if (ctrl_id == IDC_USENETWORKRESOLUTION_CHK)
							{
								app.ConfigSet (L"IsNetworkResolutionsEnabled", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED) ? true : false);
							}
							else if (ctrl_id == IDC_RULE_ALLOWINBOUND)
							{
								app.ConfigSet (L"AllowInboundConnections", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED) ? true : false);

								_app_installfilters (false);
							}
							else if (ctrl_id == IDC_RULE_ALLOWLISTEN)
							{
								if (IsDlgButtonChecked (hwnd, ctrl_id) == BST_UNCHECKED && !app.ConfirmMessage (hwnd, nullptr, _r_fmt (app.LocaleString (IDS_QUESTION_LISTEN, nullptr), LISTENS_ISSUE_URL), L"ConfirmListen"))
								{
									CheckDlgButton (hwnd, ctrl_id, BST_CHECKED);
									return TRUE;
								}

								app.ConfigSet (L"AllowListenConnections2", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED) ? true : false);

								_app_installfilters (false);
							}
							else if (ctrl_id == IDC_RULE_ALLOWLOOPBACK)
							{
								app.ConfigSet (L"AllowLoopbackConnections", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED) ? true : false);

								_app_installfilters (false);
							}
							else if (ctrl_id == IDC_ENABLELOG_CHK)
							{
								const bool is_enabled = (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED);

								app.ConfigSet (L"IsLogEnabled", is_enabled);
								CheckMenuItem (GetMenu (app.GetHWND ()), IDM_ENABLELOG_CHK, MF_BYCOMMAND | (is_enabled ? MF_CHECKED : MF_UNCHECKED));

								_r_ctrl_enable (hwnd, IDC_LOGPATH, is_enabled); // input
								_r_ctrl_enable (hwnd, IDC_LOGPATH_BTN, is_enabled); // button

								EnableWindow ((HWND)SendDlgItemMessage (hwnd, IDC_LOGSIZELIMIT, UDM_GETBUDDY, 0, 0), is_enabled);

								_app_loginit (is_enabled);
							}
							else if (ctrl_id == IDC_LOGPATH && notify_code == EN_KILLFOCUS)
							{
								app.ConfigSet (L"LogPath", _r_ctrl_gettext (hwnd, ctrl_id));

								_app_loginit (app.ConfigGet (L"IsLogEnabled", false));
							}
							else if (ctrl_id == IDC_LOGPATH_BTN)
							{
								OPENFILENAME ofn = {0};

								WCHAR path[MAX_PATH] = {0};
								GetDlgItemText (hwnd, IDC_LOGPATH, path, _countof (path));
								StringCchCopy (path, _countof (path), _r_path_expand (path));

								ofn.lStructSize = sizeof (ofn);
								ofn.hwndOwner = hwnd;
								ofn.lpstrFile = path;
								ofn.nMaxFile = _countof (path);
								ofn.lpstrFileTitle = APP_NAME_SHORT;
								ofn.nMaxFile = _countof (path);
								ofn.lpstrFilter = L"*.log\0*.log\0\0";
								ofn.lpstrDefExt = L"log";
								ofn.Flags = OFN_EXPLORER | OFN_ENABLESIZING | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;

								if (GetSaveFileName (&ofn))
								{
									StringCchCopy (path, _countof (path), _r_path_unexpand (path));

									app.ConfigSet (L"LogPath", path);
									SetDlgItemText (hwnd, IDC_LOGPATH, path);

									_app_loginit (app.ConfigGet (L"IsLogEnabled", false));

								}
							}
							else if (ctrl_id == IDC_LOGSIZELIMIT_CTRL && notify_code == EN_KILLFOCUS)
							{
								app.ConfigSet (L"LogSizeLimitKb", (DWORD)SendDlgItemMessage (hwnd, IDC_LOGSIZELIMIT, UDM_GETPOS32, 0, 0));
							}
							else if (ctrl_id == IDC_ENABLENOTIFICATIONS_CHK)
							{
								const bool is_enabled = (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED);

								app.ConfigSet (L"IsNotificationsEnabled", is_enabled);
								CheckMenuItem (GetMenu (app.GetHWND ()), IDM_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | (is_enabled ? MF_CHECKED : MF_UNCHECKED));

								_r_ctrl_enable (hwnd, IDC_NOTIFICATIONSOUND_CHK, is_enabled);
								_r_ctrl_enable (hwnd, IDC_NOTIFICATIONNOBLOCKLIST_CHK, is_enabled);

								EnableWindow ((HWND)SendDlgItemMessage (hwnd, IDC_NOTIFICATIONTIMEOUT, UDM_GETBUDDY, 0, 0), is_enabled);
								EnableWindow ((HWND)SendDlgItemMessage (hwnd, IDC_NOTIFICATIONDISPLAYTIMEOUT, UDM_GETBUDDY, 0, 0), is_enabled);

								_app_notifyrefresh ();
							}
							else if (ctrl_id == IDC_NOTIFICATIONSOUND_CHK)
							{
								app.ConfigSet (L"IsNotificationsSound", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED));
							}
							else if (ctrl_id == IDC_NOTIFICATIONNOBLOCKLIST_CHK)
							{
								app.ConfigSet (L"IsNotificationsExcludeBlocklist", (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED));
							}
							else if (ctrl_id == IDC_NOTIFICATIONDISPLAYTIMEOUT_CTRL && notify_code == EN_KILLFOCUS)
							{
								app.ConfigSet (L"NotificationsDisplayTimeout", (DWORD)SendDlgItemMessage (hwnd, IDC_NOTIFICATIONDISPLAYTIMEOUT, UDM_GETPOS32, 0, 0));
							}
							else if (ctrl_id == IDC_NOTIFICATIONTIMEOUT_CTRL && notify_code == EN_KILLFOCUS)
							{
								app.ConfigSet (L"NotificationsTimeout", (DWORD)SendDlgItemMessage (hwnd, IDC_NOTIFICATIONTIMEOUT, UDM_GETPOS32, 0, 0));
							}
							else if (ctrl_id == IDC_SYSTEMDISABLENCSI_CHK)
							{
								HKEY hkey = nullptr;
								const bool is_disable = (IsDlgButtonChecked (hwnd, ctrl_id) == BST_CHECKED);

								if (RegOpenKeyEx (HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\services\\NlaSvc\\Parameters\\Internet", 0, KEY_READ | KEY_WRITE, &hkey) == ERROR_SUCCESS)
								{
									DWORD EnableActiveProbing = !is_disable;

									RegSetValueEx (hkey, L"EnableActiveProbing", 0, REG_DWORD, (LPBYTE)&EnableActiveProbing, sizeof (EnableActiveProbing));

									RegCloseKey (hkey);
								}
							}

							break;
						}

						case IDM_ADD:
						{
							if (page->dlg_id != IDD_SETTINGS_RULES_CUSTOM)
								break;

							PITEM_RULE ptr_rule = new ITEM_RULE;

							if (ptr_rule)
							{
								ptr_rule->is_block = true; // block by default

								SetWindowLongPtr (app.GetHWND (), GWLP_USERDATA, LAST_VALUE);
								if (DialogBoxParam (nullptr, MAKEINTRESOURCE (IDD_EDITOR), hwnd, &EditorProc, (LPARAM)ptr_rule))
								{
									_r_fastlock_acquireexclusive (&lock_access);

									rules_custom.push_back (ptr_rule);

									_r_fastlock_releaseexclusive (&lock_access);

									const size_t item = _r_listview_getitemcount (hwnd, IDC_EDITOR);

									config.is_nocheckboxnotify = true;

									_r_listview_additem (hwnd, IDC_EDITOR, item, 0, ptr_rule->pname, ptr_rule->is_block ? 1 : 0, 0, rules_custom.size () - 1);
									_r_listview_setitemcheck (hwnd, IDC_EDITOR, item, ptr_rule->is_enabled);

									config.is_nocheckboxnotify = false;

									_app_profilesave (app.GetHWND ());

									app.SettingsPageInitialize (page->dlg_id, false, true); // re-inititalize page

									_app_installfilters (false);
								}
								else
								{
									_app_freerule (&ptr_rule);
								}
							}

							break;
						}

						case IDM_EDIT:
						{
							if (page->dlg_id != IDD_SETTINGS_RULES_CUSTOM)
								break;

							const size_t item = (size_t)SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

							if (item == LAST_VALUE)
								break;

							const size_t idx = _r_listview_getitemlparam (hwnd, IDC_EDITOR, item);

							PITEM_RULE ptr_rule = rules_custom.at (idx);

							if (ptr_rule)
							{
								SetWindowLongPtr (app.GetHWND (), GWLP_USERDATA, idx);
								if (DialogBoxParam (nullptr, MAKEINTRESOURCE (IDD_EDITOR), hwnd, &EditorProc, (LPARAM)ptr_rule))
								{
									_app_profilesave (app.GetHWND ());

									app.SettingsPageInitialize (page->dlg_id, false, true); // re-inititalize page

									_app_installfilters (false);
								}
							}

							break;
						}

						case IDM_DELETE:
						{
							if (page->dlg_id != IDD_SETTINGS_RULES_CUSTOM)
								break;

							const size_t total_count = (size_t)SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_GETSELECTEDCOUNT, 0, 0);

							if (!total_count)
								break;

							if (_r_msg (hwnd, MB_YESNO | MB_ICONEXCLAMATION | MB_TOPMOST, APP_NAME, nullptr, app.LocaleString (IDS_QUESTION_DELETE, nullptr), total_count) != IDYES)
								break;

							const size_t count = _r_listview_getitemcount (hwnd, IDC_EDITOR) - 1;
							bool is_enabled = false;

							_r_fastlock_acquireexclusive (&lock_access);

							for (size_t i = count; i != LAST_VALUE; i--)
							{
								if (ListView_GetItemState (GetDlgItem (hwnd, IDC_EDITOR), i, LVNI_SELECTED))
								{
									const size_t idx = _r_listview_getitemlparam (hwnd, IDC_EDITOR, i);

									if (!is_enabled && rules_custom.at (idx) && rules_custom.at (idx)->is_enabled)
										is_enabled = true;

									SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_DELETEITEM, i, 0);

									_app_freerule (&rules_custom.at (idx));
								}
							}

							_r_fastlock_releaseexclusive (&lock_access);

							_app_profilesave (app.GetHWND ());

							app.SettingsPageInitialize (page->dlg_id, false, true); // re-inititalize page

							if (is_enabled)
								_app_installfilters (false);

							_r_listview_redraw (hwnd, IDC_EDITOR);

							break;
						}

						case IDM_CHECK:
						case IDM_UNCHECK:
						{
							std::vector<PITEM_RULE> const* ptr_rules = nullptr;

							if (page->dlg_id == IDD_SETTINGS_RULES_BLOCKLIST)
								ptr_rules = &rules_blocklist;

							else if (page->dlg_id == IDD_SETTINGS_RULES_SYSTEM)
								ptr_rules = &rules_system;

							else if (page->dlg_id == IDD_SETTINGS_RULES_CUSTOM)
								ptr_rules = &rules_custom;

							if (ptr_rules)
							{
								const bool new_val = (LOWORD (pmsg->wParam) == IDM_CHECK) ? true : false;

								size_t item = LAST_VALUE;

								_r_fastlock_acquireexclusive (&lock_access);

								while ((item = (size_t)SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_GETNEXTITEM, item, LVNI_SELECTED)) != -1)
								{
									const size_t idx = _r_listview_getitemlparam (hwnd, IDC_EDITOR, item);

									PITEM_RULE ptr_rule = ptr_rules->at (idx);

									if (ptr_rule)
									{
										ptr_rule->is_enabled = new_val;

										config.is_nocheckboxnotify = true;

										_r_listview_setitemcheck (hwnd, IDC_EDITOR, item, new_val);

										if (page->dlg_id == IDD_SETTINGS_RULES_BLOCKLIST || page->dlg_id == IDD_SETTINGS_RULES_SYSTEM)
											rules_config[ptr_rule->pname] = new_val;

										config.is_nocheckboxnotify = false;
									}
								}

								_r_fastlock_releaseexclusive (&lock_access);

								_app_profilesave (app.GetHWND ());
								_app_installfilters (false);

								settings_callback (hwnd, _RM_LOCALIZE, nullptr, page); // re-inititalize page
							}

							break;
						}
					}

					break;
				}
			}

			break;
		}
	}

	return FALSE;
}

void ResizeWindow (HWND hwnd, INT width, INT height)
{
	RECT rc = {0};

	GetClientRect (GetDlgItem (hwnd, IDC_EXIT_BTN), &rc);
	const INT button_width = rc.right;

	GetClientRect (GetDlgItem (hwnd, IDC_STATUSBAR), &rc);
	const INT statusbar_height = rc.bottom;

	const INT button_top = height - statusbar_height - app.GetDPI (1 + 34);

	SetWindowPos (GetDlgItem (hwnd, IDC_LISTVIEW), nullptr, 0, 0, width, height - statusbar_height - app.GetDPI (1 + 46), SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSENDCHANGING | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);

	SetWindowPos (GetDlgItem (hwnd, IDC_START_BTN), nullptr, app.GetDPI (10), button_top, 0, 0, SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSENDCHANGING | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
	SetWindowPos (GetDlgItem (hwnd, IDC_SETTINGS_BTN), nullptr, width - app.GetDPI (10) - button_width - button_width - app.GetDPI (6), button_top, 0, 0, SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSENDCHANGING | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
	SetWindowPos (GetDlgItem (hwnd, IDC_EXIT_BTN), nullptr, width - app.GetDPI (10) - button_width, button_top, 0, 0, SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSENDCHANGING | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSIZE);

	// resize statusbar
	SendDlgItemMessage (hwnd, IDC_STATUSBAR, WM_SIZE, 0, 0);
}

bool _wfp_logunsubscribe ()
{
	bool result = false;

	_app_loginit (false); // destroy log file handle if present

	if (config.hevent)
	{
		HMODULE hlib = LoadLibrary (L"fwpuclnt.dll");

		if (hlib)
		{
			const FWPMNEU _FwpmNetEventUnsubscribe = (FWPMNEU)GetProcAddress (hlib, "FwpmNetEventUnsubscribe0");

			if (_FwpmNetEventUnsubscribe)
			{
				DWORD rc = _FwpmNetEventUnsubscribe (config.hengine, config.hevent);

				if (rc == ERROR_SUCCESS)
				{
					config.hevent = nullptr;
					result = true;
				}
			}

			FreeLibrary (hlib);
		}
	}

	return result;
}

bool _wfp_logsubscribe ()
{
	if (!config.hengine)
		return false;

	bool result = false;

	if (config.hevent)
	{
		result = true;
	}
	else
	{
		const HMODULE hlib = LoadLibrary (L"fwpuclnt.dll");

		if (!hlib)
		{
			_app_logerror (L"LoadLibrary", GetLastError (), L"fwpuclnt.dll");
		}
		else
		{
			FWPMNES2 _FwpmNetEventSubscribe2 = nullptr;
			FWPMNES1 _FwpmNetEventSubscribe1 = nullptr;
			FWPMNES0 _FwpmNetEventSubscribe0 = nullptr;

			_FwpmNetEventSubscribe2 = (FWPMNES2)GetProcAddress (hlib, "FwpmNetEventSubscribe2"); // win10+

			if (!_FwpmNetEventSubscribe2)
			{
				_FwpmNetEventSubscribe1 = (FWPMNES1)GetProcAddress (hlib, "FwpmNetEventSubscribe1"); // win8+

				if (!_FwpmNetEventSubscribe1)
					_FwpmNetEventSubscribe0 = (FWPMNES0)GetProcAddress (hlib, "FwpmNetEventSubscribe0"); // win7+
			}

			if (!_FwpmNetEventSubscribe2 && !_FwpmNetEventSubscribe1 && !_FwpmNetEventSubscribe0)
			{
				_app_logerror (L"GetProcAddress", GetLastError (), L"FwpmNetEventSubscribe");
			}
			else
			{
				FWPM_NET_EVENT_SUBSCRIPTION subscription;
				FWPM_NET_EVENT_ENUM_TEMPLATE enum_template;

				SecureZeroMemory (&subscription, sizeof (subscription));
				SecureZeroMemory (&enum_template, sizeof (enum_template));

				if (config.psession)
					memcpy (&subscription.sessionKey, config.psession, sizeof (GUID));

				subscription.enumTemplate = &enum_template;

				DWORD rc = 0;

				if (_FwpmNetEventSubscribe2)
					rc = _FwpmNetEventSubscribe2 (config.hengine, &subscription, &_app_logcallback2, nullptr, &config.hevent); // win10+

				else if (_FwpmNetEventSubscribe1)
					rc = _FwpmNetEventSubscribe1 (config.hengine, &subscription, &_app_logcallback1, nullptr, &config.hevent); // win8+

				else if (_FwpmNetEventSubscribe0)
					rc = _FwpmNetEventSubscribe0 (config.hengine, &subscription, &_app_logcallback0, nullptr, &config.hevent); // win7+

				if (rc != ERROR_SUCCESS)
				{
					_app_logerror (L"FwpmNetEventSubscribe", rc, nullptr);
				}
				else
				{
					_app_loginit (true); // create log file
					result = true;
				}
			}

			FreeLibrary (hlib);
		}
	}

	return result;
}

bool _wfp_initialize (bool is_full)
{
	bool result = true;
	DWORD rc = 0;

	if (!config.hengine)
	{
		// generate unique session key
		if (!config.psession)
		{
			config.psession = new GUID;

			if (config.psession)
			{
				if (CoCreateGuid (config.psession) != S_OK)
				{
					delete config.psession;
					config.psession = nullptr;
				}
			}
		}

		SecureZeroMemory (&session, sizeof (session));

		session.displayData.name = APP_NAME;
		session.displayData.description = APP_NAME;

		if (config.psession)
			memcpy (&session.sessionKey, config.psession, sizeof (GUID));

		rc = FwpmEngineOpen (nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &config.hengine);

		if (rc != ERROR_SUCCESS)
		{
			_app_logerror (L"FwpmEngineOpen", rc, nullptr);
			config.hengine = nullptr;
			result = false;
		}
	}

	// set security info
	if (is_full && result && config.hengine && !config.is_securityinfoset)
	{
		if (config.hengine && config.psid)
		{
			// Add DACL for given user
			PACL pDacl = nullptr;
			PSECURITY_DESCRIPTOR securityDescriptor = nullptr;

			rc = FwpmEngineGetSecurityInfo (config.hengine, DACL_SECURITY_INFORMATION, nullptr, nullptr, &pDacl, nullptr, &securityDescriptor);

			if (rc != ERROR_SUCCESS)
			{
				_app_logerror (L"FwpmEngineGetSecurityInfo", rc, nullptr);
			}
			else
			{
				bool bExists = false;

				// Loop through the ACEs and search for user SID.
				for (WORD cAce = 0; !bExists && cAce < pDacl->AceCount; cAce++)
				{
					ACCESS_ALLOWED_ACE* pAce = nullptr;

					// Get ACE info
					if (!GetAce (pDacl, cAce, (LPVOID*)&pAce))
					{
						_app_logerror (L"GetAce", GetLastError (), nullptr);
						continue;
					}

					if (pAce->Header.AceType != ACCESS_ALLOWED_ACE_TYPE)
						continue;

					if (EqualSid (&pAce->SidStart, config.psid))
						bExists = true;
				}

				if (!bExists)
				{
					PACL pNewDacl = nullptr;
					EXPLICIT_ACCESS ea = {0};

					// Initialize an EXPLICIT_ACCESS structure for the new ACE.
					SecureZeroMemory (&ea, sizeof (ea));

					ea.grfAccessPermissions = FWPM_GENERIC_ALL | DELETE | WRITE_DAC | WRITE_OWNER;
					ea.grfAccessMode = GRANT_ACCESS;
					ea.grfInheritance = NO_INHERITANCE;
					ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
					ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
					ea.Trustee.ptstrName = (LPTSTR)config.psid;
					//ea.Trustee.ptstrName = user;

					// Create a new ACL that merges the new ACE
					// into the existing DACL.
					rc = SetEntriesInAcl (1, &ea, pDacl, &pNewDacl);

					if (rc != ERROR_SUCCESS)
					{
						_app_logerror (L"SetEntriesInAcl", rc, nullptr);
					}
					else
					{
						rc = FwpmEngineSetSecurityInfo (config.hengine, DACL_SECURITY_INFORMATION, nullptr, nullptr, pNewDacl, nullptr);

						if (rc != ERROR_SUCCESS)
						{
							_app_logerror (L"FwpmEngineSetSecurityInfo", rc, nullptr);
						}
						else
						{
							config.is_securityinfoset = true;
						}
					}

					if (pNewDacl)
						LocalFree (pNewDacl);
				}
			}
		}
	}

	// dropped packets logging (win7+)
	if (is_full && result && config.hengine && !config.hevent && _r_sys_validversion (6, 1))
	{
		FWP_VALUE val;

		val.type = FWP_UINT32;
		val.uint32 = 1;

		rc = FwpmEngineSetOption (config.hengine, FWPM_ENGINE_COLLECT_NET_EVENTS, &val);

		if (rc != ERROR_SUCCESS)
		{
			_app_logerror (L"FwpmEngineSetOption", rc, L"FWPM_ENGINE_COLLECT_NET_EVENTS");
		}
		else
		{
			// configure dropped packets logging (win8+)
			if (_r_sys_validversion (6, 2))
			{
				// the filter engine will collect wfp network events that match any supplied key words
				val.type = FWP_UINT32;
				val.uint32 = FWPM_NET_EVENT_KEYWORD_CAPABILITY_DROP;

				FwpmEngineSetOption (config.hengine, FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS, &val);

				// enables the connection monitoring feature and starts logging creation and deletion events (and notifying any subscribers)
				val.type = FWP_UINT32;
				val.uint32 = 1;

				FwpmEngineSetOption (config.hengine, FWPM_ENGINE_MONITOR_IPSEC_CONNECTIONS, &val);
			}
		}
	}

	if (is_full && config.hengine && !_wfp_isfiltersinstalled ())
	{
		rc = FwpmTransactionBegin (config.hengine, 0);

		if (rc != ERROR_SUCCESS)
		{
			_app_logerror (L"FwpmTransactionBegin", rc, nullptr);
			result = false;
		}
		else
		{
			// create provider
			FWPM_PROVIDER provider = {0};

			provider.displayData.name = APP_NAME;
			provider.displayData.description = APP_NAME;

			provider.providerKey = GUID_WfpProvider;
			provider.flags = FWPM_PROVIDER_FLAG_PERSISTENT;

			rc = FwpmProviderAdd (config.hengine, &provider, nullptr);

			if (rc != ERROR_SUCCESS && rc != FWP_E_ALREADY_EXISTS)
			{
				_app_logerror (L"FwpmProviderAdd", rc, nullptr);
				FwpmTransactionAbort (config.hengine);
				result = false;
			}
			else
			{
				FWPM_SUBLAYER sublayer = {0};

				sublayer.displayData.name = APP_NAME;
				sublayer.displayData.description = APP_NAME;

				sublayer.providerKey = (LPGUID)&GUID_WfpProvider;
				sublayer.subLayerKey = GUID_WfpSublayer;
				sublayer.flags = FWPM_SUBLAYER_FLAG_PERSISTENT;
				sublayer.weight = (UINT16)app.ConfigGet (L"SublayerWeight", 0x0000ffff).AsUint (); // highest weight "65535"

				rc = FwpmSubLayerAdd (config.hengine, &sublayer, nullptr);

				if (rc != ERROR_SUCCESS && rc != FWP_E_ALREADY_EXISTS)
				{
					_app_logerror (L"FwpmSubLayerAdd", rc, nullptr);
					FwpmTransactionAbort (config.hengine);
					result = false;
				}
				else
				{
					FwpmTransactionCommit (config.hengine);
					result = true;
				}
			}
		}
	}

	return result;
}

void _wfp_uninitialize (bool is_force)
{
	DWORD result = 0;

	if (config.hengine)
	{
		if (is_force)
		{
			result = FwpmTransactionBegin (config.hengine, 0);

			if (result != ERROR_SUCCESS)
			{
				_app_logerror (L"FwpmTransactionBegin", result, nullptr);
			}
			else
			{
				// destroy callouts (deprecated)
				{
					const GUID callouts[] = {
						GUID_WfpOutboundCallout4_DEPRECATED,
						GUID_WfpOutboundCallout6_DEPRECATED,
						GUID_WfpInboundCallout4_DEPRECATED,
						GUID_WfpInboundCallout6_DEPRECATED,
						GUID_WfpListenCallout4_DEPRECATED,
						GUID_WfpListenCallout6_DEPRECATED
					};

					for (UINT i = 0; i < _countof (callouts); i++)
						FwpmCalloutDeleteByKey (config.hengine, &callouts[i]);
				}

				// destroy sublayer
				result = FwpmSubLayerDeleteByKey (config.hengine, &GUID_WfpSublayer);

				if (result != ERROR_SUCCESS && result != FWP_E_SUBLAYER_NOT_FOUND)
					_app_logerror (L"FwpmSubLayerDeleteByKey", result, nullptr);

				// destroy provider
				result = FwpmProviderDeleteByKey (config.hengine, &GUID_WfpProvider);

				if (result != ERROR_SUCCESS && result != FWP_E_PROVIDER_NOT_FOUND)
					_app_logerror (L"FwpmProviderDeleteByKey", result, nullptr);

				FwpmTransactionCommit (config.hengine);
			}
		}

		// dropped packets logging (win7+)
		if (_r_sys_validversion (6, 1))
		{
			_wfp_logunsubscribe ();

			FWP_VALUE val;

			val.type = FWP_UINT32;
			val.uint32 = 0;

			result = FwpmEngineSetOption (config.hengine, FWPM_ENGINE_COLLECT_NET_EVENTS, &val);

			if (result != ERROR_SUCCESS)
				_app_logerror (L"FwpmEngineSetOption", result, L"FWPM_ENGINE_COLLECT_NET_EVENTS");

			if (_r_sys_validversion (6, 2))
			{
				val.type = FWP_UINT32;
				val.uint32 = 0;

				FwpmEngineSetOption (config.hengine, FWPM_ENGINE_MONITOR_IPSEC_CONNECTIONS, &val);
			}
		}

		FwpmEngineClose (config.hengine);
		config.hengine = nullptr;
	}

	if (config.psession)
	{
		delete config.psession;
		config.psession = nullptr;
	}

	config.is_securityinfoset = false;
}

void DrawFrameBorder (HDC dc, HWND hwnd, COLORREF clr)
{
	RECT rc = {0};
	GetWindowRect (hwnd, &rc);

	HPEN hpen = CreatePen (PS_INSIDEFRAME, GetSystemMetrics (SM_CXBORDER), clr);

	HPEN old_pen = (HPEN)SelectObject (dc, hpen);
	HBRUSH old_brush = (HBRUSH)SelectObject (dc, GetStockObject (NULL_BRUSH));

	Rectangle (dc, 0, 0, (rc.right - rc.left), (rc.bottom - rc.top));

	SelectObject (dc, old_pen);
	SelectObject (dc, old_brush);

	DeleteObject (hpen);
}

LRESULT CALLBACK NotificationProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
		case WM_CLOSE:
		{
			_app_notifyhide (hwnd);

			SetWindowLongPtr (hwnd, DWLP_MSGRESULT, TRUE);
			return TRUE;
		}

		case WM_MOUSEMOVE:
		{
			if (!config.is_notifytimeout)
				_app_notifysettimeout (hwnd, 0, false, 0);

			break;
		}

		//case WM_MOUSELEAVE:
		//{
		//	if (!config.is_notifymouse || config.is_notifytimeout)
		//		break;

		//	config.is_notifymouse = false;

		//	_app_notifysettimeout (hwnd, 0, false, 0);
		//	_app_notifysettimeout (hwnd, NOTIFY_TIMER_DISPLAY_ID, true, NOTIFY_TIMER_MOUSE);

		//	break;
		//}

		case WM_ACTIVATE:
		{
			switch (LOWORD (wparam))
			{
				//case WA_ACTIVE:
				//case WA_CLICKACTIVE:
				//{
				//	_app_notifysettimeout (hwnd, 0, false, 0);
				//	break;
				//}

				case WA_INACTIVE:
				{
					if (!config.is_notifytimeout)
						_app_notifyhide (hwnd);

					break;
				}
			}

			break;
		}

		case WM_TIMER:
		{
			if (config.is_notifytimeout && wparam != NOTIFY_TIMER_TIMEOUT_ID)
				return FALSE;

			//case NOTIFY_TIMER_FOREGROUND_ID:
			//{
			//	if (GetForegroundWindow () != hwnd)
			//	{
			//		//_app_notifysettimeout (hwnd, NOTIFY_TIMER_POPUP_ID, true, NOTIFY_TIMER_POPUP);
			//		_app_notifyhide (hwnd);
			//	}

			//	return FALSE;
			//}

			if (wparam == NOTIFY_TIMER_TIMEOUT_ID)
			{
				if (_r_wnd_undercursor (hwnd))
				{
					_app_notifysettimeout (hwnd, wparam, false, 0);
					return FALSE;
				}
			}

			if (wparam == NOTIFY_TIMER_POPUP_ID || wparam == NOTIFY_TIMER_TIMEOUT_ID)
				_app_notifyhide (hwnd);

			break;
		}

		case WM_KEYDOWN:
		{
			switch (wparam)
			{
				case VK_ESCAPE:
				{
					_app_notifyhide (hwnd);
					break;
				}
			}

			break;
		}

		case WM_PAINT:
		{
			PAINTSTRUCT ps = {0};
			HDC hdc = BeginPaint (hwnd, &ps);

			RECT rc = {0};
			GetClientRect (hwnd, &rc);

			static const INT bottom = app.GetDPI (48);

			rc.left = GetSystemMetrics (SM_CXBORDER);
			rc.right -= (rc.left * 2);
			rc.top = rc.bottom - bottom;
			rc.bottom = rc.top + bottom;

			_r_dc_fillrect (hdc, &rc, GetSysColor (COLOR_BTNFACE));

			for (INT i = 0; i < rc.right; i++)
				SetPixel (hdc, i, rc.top, GetSysColor (COLOR_APPWORKSPACE));

			DrawFrameBorder (hdc, hwnd, NOTIFY_BORDER_COLOR);

			EndPaint (hwnd, &ps);

			break;
		}

		case WM_CTLCOLORBTN:
		case WM_CTLCOLOREDIT:
		case WM_CTLCOLORSTATIC:
		{
			const UINT ctrl_id = GetDlgCtrlID ((HWND)lparam);

			if (
				ctrl_id == IDC_ICON_ID ||
				ctrl_id == IDC_TITLE_ID ||
				ctrl_id == IDC_MUTE_BTN ||
				ctrl_id == IDC_CLOSE_BTN ||
				ctrl_id == IDC_FILE_ID ||
				ctrl_id == IDC_ADDRESS_REMOTE_ID ||
				ctrl_id == IDC_ADDRESS_LOCAL_ID ||
				ctrl_id == IDC_FILTER_ID ||
				ctrl_id == IDC_DATE_ID ||
				ctrl_id == IDC_CREATERULE_ADDR_ID ||
				ctrl_id == IDC_CREATERULE_PORT_ID
				)
			{
				SetBkMode ((HDC)wparam, TRANSPARENT); // background-hack
				SetTextColor ((HDC)wparam, _r_dc_getcolorbrightness (GetSysColor (COLOR_WINDOW)));

				return (INT_PTR)GetSysColorBrush (COLOR_WINDOW);
			}

			break;
		}

		case WM_SETCURSOR:
		{
			const UINT ctrl_id = GetDlgCtrlID ((HWND)wparam);

			if (ctrl_id == IDC_MUTE_BTN || ctrl_id == IDC_CLOSE_BTN)
			{
				SetCursor (LoadCursor (nullptr, IDC_HAND));

				SetWindowLongPtr (hwnd, DWLP_MSGRESULT, TRUE);
				return TRUE;
			}

			break;
		}

		case WM_NOTIFY:
		{
			LPNMHDR nmlp = (LPNMHDR)lparam;

			switch (nmlp->code)
			{
				case TTN_GETDISPINFO:
				{
					LPNMTTDISPINFO lpnmdi = (LPNMTTDISPINFO)lparam;

					if ((lpnmdi->uFlags & TTF_IDISHWND) != 0)
					{
						WCHAR buffer[1024] = {0};
						const UINT ctrl_id = GetDlgCtrlID ((HWND)lpnmdi->hdr.idFrom);

						if (
							ctrl_id == IDC_MUTE_BTN ||
							ctrl_id == IDC_CLOSE_BTN ||
							ctrl_id == IDC_FILE_ID ||
							ctrl_id == IDC_ADDRESS_REMOTE_ID ||
							ctrl_id == IDC_ADDRESS_LOCAL_ID ||
							ctrl_id == IDC_FILTER_ID ||
							ctrl_id == IDC_DATE_ID
							)
						{
							_r_fastlock_acquireshared (&lock_notification);

							const size_t idx = _app_notifygetcurrent ();

							if (idx != LAST_VALUE)
							{
								PITEM_LOG const ptr_log = notifications.at (idx);

								if (ptr_log)
								{
									if (ctrl_id == IDC_MUTE_BTN)
									{
										StringCchCopy (buffer, _countof (buffer), app.LocaleString (IDS_NOTIFY_DISABLENOTIFICATIONS, nullptr));
									}
									else if (ctrl_id == IDC_CLOSE_BTN)
									{
										StringCchCopy (buffer, _countof (buffer), app.LocaleString (IDS_CLOSE, nullptr));
									}
									else if (ctrl_id == IDC_FILE_ID)
									{
										StringCchCopy (buffer, _countof (buffer), _app_gettooltip (ptr_log->hash));
									}
									else
									{
										StringCchCopy (buffer, _countof (buffer), _r_ctrl_gettext (hwnd, ctrl_id));
									}

									lpnmdi->lpszText = buffer;
								}
							}

							_r_fastlock_releaseshared (&lock_notification);
						}
						else if (ctrl_id == IDC_CREATERULE_ADDR_ID || ctrl_id == IDC_CREATERULE_PORT_ID)
						{
							StringCchCopy (buffer, _countof (buffer), app.LocaleString (IDS_NOTIFY_TOOLTIP, nullptr));

							lpnmdi->lpszText = buffer;
						}
					}

					break;
				}

				case NM_CLICK:
				case NM_RETURN:
				{
					PNMLINK nmlink = (PNMLINK)lparam;

					if (nmlink->item.szUrl[0])
					{
						if (nmlp->idFrom == IDC_FILE_ID)
						{
							_r_fastlock_acquireshared (&lock_notification);

							const size_t idx = _app_notifygetcurrent ();

							if (idx != LAST_VALUE)
							{
								ShowItem (app.GetHWND (), IDC_LISTVIEW, _app_getposition (app.GetHWND (), notifications.at (idx)->hash), -1);

								_r_wnd_toggle (app.GetHWND (), true);
							}

							_r_fastlock_releaseshared (&lock_notification);

							_app_notifyhide (hwnd);
						}
						else if (nmlp->idFrom == IDC_ADDRESS_REMOTE_ID)
						{
						}
						else if (nmlp->idFrom == IDC_ADDRESS_LOCAL_ID)
						{
						}
					}

					break;
				}
			}

			break;
		}

		case WM_COMMAND:
		{
			switch (LOWORD (wparam))
			{
				case IDC_CREATERULE_ADDR_ID:
				case IDC_CREATERULE_PORT_ID:
				{
					const bool is_enabled = (IsDlgButtonChecked (hwnd, IDC_CREATERULE_ADDR_ID) == BST_CHECKED) || (IsDlgButtonChecked (hwnd, IDC_CREATERULE_PORT_ID) == BST_CHECKED);

					SetDlgItemText (hwnd, IDC_ALLOW_BTN, app.LocaleString (is_enabled ? IDS_ACTION_4 : IDS_ACTION_1, nullptr));

					break;
				}

				case IDC_ALLOW_BTN:
				{
					_app_notifycommand (hwnd, CmdAllow);
					break;
				}

				//case IDC_BLOCK_BTN:
				//{
				//	_app_notifycommand (hwnd, CmdBlock);
				//	break;
				//}

				case IDC_IGNORE_BTN:
				{
					_app_notifycommand (hwnd, CmdIgnore);
					break;
				}

				case IDC_MUTE_BTN:
				{
					_app_notifycommand (hwnd, CmdMute);
					break;
				}

				case IDC_CLOSE_BTN:
				{
					_app_notifyhide (hwnd);
					break;
				}
			}

			break;
		}
	}

	return DefWindowProc (hwnd, msg, wparam, lparam);
}

void _app_generate_addmenu (HMENU submenu)
{
	const UINT uproc_id = 2;
	const UINT upckg_id = 3;
	const UINT usvc_id = 4;

	const HMENU submenu_process = GetSubMenu (submenu, uproc_id);
	const HMENU submenu_package = GetSubMenu (submenu, upckg_id);
	const HMENU submenu_service = GetSubMenu (submenu, usvc_id);

	_app_generate_processes ();

	app.LocaleMenu (submenu, IDS_ADD_FILE, IDM_ADD_FILE, false, L"...");
	app.LocaleMenu (submenu, IDS_ADD_PROCESS, uproc_id, true, nullptr);
	app.LocaleMenu (submenu, IDS_ADD_PACKAGE, upckg_id, true, nullptr);
	app.LocaleMenu (submenu, IDS_ADD_SERVICE, usvc_id, true, nullptr);
	app.LocaleMenu (submenu, IDS_ALL, IDM_ALL_PROCESSES, false, _r_fmt (L" (%d)", processes.size ()));
	app.LocaleMenu (submenu, IDS_ALL, IDM_ALL_PACKAGES, false, _r_fmt (L" (%d)", packages.size ()));
	app.LocaleMenu (submenu, IDS_ALL, IDM_ALL_SERVICES, false, _r_fmt (L" (%d)", services.size ()));

	// generate processes popup menu
	{
		if (processes.empty ())
		{
			MENUITEMINFO mii = {0};

			WCHAR buffer[128] = {0};
			StringCchCopy (buffer, _countof (buffer), app.LocaleString (IDS_STATUS_EMPTY, nullptr));

			mii.cbSize = sizeof (mii);
			mii.fMask = MIIM_STATE | MIIM_STRING;
			mii.dwTypeData = buffer;
			mii.fState = MF_DISABLED | MF_GRAYED;

			SetMenuItemInfo (submenu_process, IDM_ALL_PROCESSES, FALSE, &mii);
		}
		else
		{
			AppendMenu (submenu_process, MF_SEPARATOR, 0, nullptr);

			for (size_t i = 0; i < processes.size (); i++)
			{
				MENUITEMINFO mii = {0};

				mii.cbSize = sizeof (mii);
				mii.fMask = MIIM_ID | MIIM_CHECKMARKS | MIIM_STRING;
				mii.fType = MFT_STRING;
				mii.dwTypeData = processes.at (i).display_name;
				mii.wID = IDX_PROCESS + UINT (i);
				mii.hbmpChecked = processes.at (i).hbmp;
				mii.hbmpUnchecked = processes.at (i).hbmp;

				InsertMenuItem (submenu_process, IDX_PROCESS + UINT (i), FALSE, &mii);
			}
		}
	}

	// generate packages popup menu (win8+)
	if (_r_sys_validversion (6, 2))
	{
		size_t total_added = 0;

		if (!packages.empty ())
		{
			for (size_t i = 0; i < packages.size (); i++)
			{
				if (apps.find (packages.at (i).hash) != apps.end ())
					continue;

				if (!total_added)
					AppendMenu (submenu_package, MF_SEPARATOR, 1, nullptr);

				MENUITEMINFO mii = {0};

				mii.cbSize = sizeof (mii);
				mii.fMask = MIIM_ID | MIIM_CHECKMARKS | MIIM_STRING;
				mii.fType = MFT_STRING;
				mii.dwTypeData = packages.at (i).display_name;
				mii.wID = IDX_PACKAGE + UINT (i);
				mii.hbmpChecked = config.hbitmap_package_small;
				mii.hbmpUnchecked = config.hbitmap_package_small;

				InsertMenuItem (submenu_package, IDX_PACKAGE + UINT (i), FALSE, &mii);
				total_added += 1;
			}
		}

		if (!total_added)
		{
			MENUITEMINFO mii = {0};

			WCHAR buffer[128] = {0};
			StringCchCopy (buffer, _countof (buffer), app.LocaleString (IDS_STATUS_EMPTY, nullptr));

			mii.cbSize = sizeof (mii);
			mii.fMask = MIIM_STATE | MIIM_STRING;
			mii.dwTypeData = buffer;
			mii.fState = MF_DISABLED | MF_GRAYED;

			SetMenuItemInfo (submenu_package, IDM_ALL_PACKAGES, FALSE, &mii);
		}
	}
	else
	{
		EnableMenuItem (submenu, upckg_id, MF_BYPOSITION | MF_DISABLED | MF_GRAYED);
	}

	{
		size_t total_added = 0;

		if (!services.empty ())
		{
			for (size_t i = 0; i < services.size (); i++)
			{
				if (apps.find (services.at (i).hash) != apps.end ())
					continue;

				if (!total_added)
					AppendMenu (submenu_service, MF_SEPARATOR, 1, nullptr);

				MENUITEMINFO mii = {0};

				mii.cbSize = sizeof (mii);
				mii.fMask = MIIM_ID | MIIM_CHECKMARKS | MIIM_STRING;
				mii.fType = MFT_STRING;
				mii.dwTypeData = services.at (i).display_name;
				mii.wID = IDX_SERVICE + UINT (i);
				mii.hbmpChecked = config.hbitmap_service_small;
				mii.hbmpUnchecked = config.hbitmap_service_small;

				InsertMenuItem (submenu_service, IDX_SERVICE + UINT (i), FALSE, &mii);
				total_added += 1;
			}
		}

		if (!total_added)
		{
			MENUITEMINFO mii = {0};

			WCHAR buffer[128] = {0};
			StringCchCopy (buffer, _countof (buffer), app.LocaleString (IDS_STATUS_EMPTY, nullptr));

			mii.cbSize = sizeof (mii);
			mii.fMask = MIIM_STATE | MIIM_STRING;
			mii.dwTypeData = buffer;
			mii.fState = MF_DISABLED | MF_GRAYED;

			SetMenuItemInfo (submenu_service, IDM_ALL_SERVICES, FALSE, &mii);
		}
	}
}

INT_PTR CALLBACK DlgProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	if (msg == WM_FINDMSGSTRING)
	{
		LPFINDREPLACE const lpfr = (LPFINDREPLACE)lparam;

		if ((lpfr->Flags & FR_DIALOGTERM) != 0)
		{
			config.hfind = nullptr;
		}
		else if ((lpfr->Flags & FR_FINDNEXT) != 0)
		{
			const size_t total = _r_listview_getitemcount (hwnd, IDC_LISTVIEW);
			const INT start = (INT)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, (WPARAM)total - 1, LVNI_SELECTED | LVNI_DIRECTIONMASK | LVNI_BELOW) + 1;

			for (size_t i = start; i < total; i++)
			{
				const rstring text = _r_listview_getitemtext (hwnd, IDC_LISTVIEW, i, 0);

				if (StrStrI (text, lpfr->lpstrFindWhat) != nullptr)
				{
					ShowItem (hwnd, IDC_LISTVIEW, i, 0);
					SetFocus (hwnd);
					break;
				}
			}
		}

		return FALSE;
	}

	switch (msg)
	{
		case WM_INITDIALOG:
		{
			// static initializer
			config.wd_length = GetWindowsDirectory (config.windows_dir, _countof (config.windows_dir));
			config.tmp1_length = GetTempPath (_countof (config.tmp1_dir), config.tmp1_dir);
			GetLongPathName (rstring (config.tmp1_dir), config.tmp1_dir, _countof (config.tmp1_dir));

			StringCchPrintf (config.apps_path, _countof (config.apps_path), L"%s\\" XML_APPS, app.GetProfileDirectory ());
			StringCchPrintf (config.rules_config_path, _countof (config.rules_config_path), L"%s\\" XML_RULES_CONFIG, app.GetProfileDirectory ());
			StringCchPrintf (config.rules_custom_path, _countof (config.rules_custom_path), L"%s\\" XML_RULES_CUSTOM, app.GetProfileDirectory ());

			config.ntoskrnl_hash = _r_str_hash (PROC_SYSTEM_NAME);
			config.myhash = _r_str_hash (app.GetBinaryPath ());
			apps_undelete[config.myhash] = true; // disable deletion for me ;)

			// set privileges
			{
				LPCWSTR privileges[] = {
					SE_BACKUP_NAME,
					SE_DEBUG_NAME,
					SE_SECURITY_NAME,
					SE_TAKE_OWNERSHIP_NAME,
				};

				_r_sys_setprivilege (privileges, _countof (privileges), true);
			}

			// set process priority
			SetPriorityClass (GetCurrentProcess (), HIGH_PRIORITY_CLASS);

			// initialize spinlocks
			_r_fastlock_initialize (&lock_apply);
			_r_fastlock_initialize (&lock_access);
			_r_fastlock_initialize (&lock_writelog);
			_r_fastlock_initialize (&lock_notification);

			// get current user security identifier
			if (!config.psid)
			{
				// get user sid
				HANDLE token = nullptr;
				DWORD token_length = 0;
				PTOKEN_USER token_user = nullptr;

				if (OpenProcessToken (GetCurrentProcess (), TOKEN_QUERY, &token))
				{
					GetTokenInformation (token, TokenUser, nullptr, token_length, &token_length);

					if (GetLastError () == ERROR_INSUFFICIENT_BUFFER)
					{
						token_user = new TOKEN_USER[token_length];

						if (token_user)
						{
							if (GetTokenInformation (token, TokenUser, token_user, token_length, &token_length))
							{
								SID_NAME_USE sid_type;

								WCHAR username[MAX_PATH] = {0};
								WCHAR domain[MAX_PATH] = {0};

								DWORD length1 = _countof (username);
								DWORD length2 = _countof (domain);

								if (LookupAccountSid (nullptr, token_user->User.Sid, username, &length1, domain, &length2, &sid_type) && length1 && length2)
									StringCchPrintf (config.title, _countof (config.title), L"%s [%s\\%s]", APP_NAME, domain, username);

								config.psid = new BYTE[SECURITY_MAX_SID_SIZE];

								if (config.psid)
									memcpy (config.psid, token_user->User.Sid, SECURITY_MAX_SID_SIZE);
							}
						}

						delete[] token_user;
					}

					CloseHandle (token);
				}

				if (!config.title[0])
					StringCchCopy (config.title, _countof (config.title), APP_NAME); // fallback
			}

			// configure listview
			_r_listview_setstyle (hwnd, IDC_LISTVIEW, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);

			_r_listview_addcolumn (hwnd, IDC_LISTVIEW, 0, app.LocaleString (IDS_FILEPATH, nullptr), 70, LVCFMT_LEFT);
			_r_listview_addcolumn (hwnd, IDC_LISTVIEW, 1, app.LocaleString (IDS_ADDED, nullptr), 26, LVCFMT_LEFT);

			_r_listview_addgroup (hwnd, IDC_LISTVIEW, 0, app.LocaleString (IDS_GROUP_ALLOWED, nullptr), 0, LVGS_COLLAPSIBLE | (app.ConfigGet (L"Group1IsCollaped", false).AsBool () ? LVGS_COLLAPSED : LVGS_NORMAL));
			_r_listview_addgroup (hwnd, IDC_LISTVIEW, 1, app.LocaleString (IDS_GROUP_BLOCKED, nullptr), 0, LVGS_COLLAPSIBLE | (app.ConfigGet (L"Group2IsCollaped", false).AsBool () ? LVGS_COLLAPSED : LVGS_NORMAL));

			_app_listviewsetimagelist (hwnd, IDC_LISTVIEW);
			_app_listviewsetfont (hwnd, IDC_LISTVIEW);

			// load settings imagelist
			{
				const INT cx = GetSystemMetrics (SM_CXSMICON);

				config.himg = ImageList_Create (cx, cx, ILC_COLOR32 | ILC_MASK, 0, 5);

				HICON hico = _r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_ALLOW), cx);
				ImageList_ReplaceIcon (config.himg, -1, hico);
				DestroyIcon (hico);

				hico = _r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_BLOCK), cx);
				ImageList_ReplaceIcon (config.himg, -1, hico);
				DestroyIcon (hico);
			}

			// get default icon for executable
			_app_getfileicon (_r_path_expand (PATH_NTOSKRNL), false, &config.icon_id, &config.hicon_large);
			_app_getfileicon (_r_path_expand (PATH_NTOSKRNL), true, &config.icon_id, &config.hicon_small);

			// get default icon for services
			if (_app_getfileicon (_r_path_expand (PATH_SERVICES), true, &config.icon_service_id, &config.hicon_service_small))
			{
				config.hbitmap_service_small = _app_ico2bmp (config.hicon_service_small);
			}
			else
			{
				config.hicon_service_small = config.hicon_small;
				config.hbitmap_service_small = _app_ico2bmp (config.hicon_small);
			}

			// get default icon for windows store package (win8+)
			if (_r_sys_validversion (6, 2))
			{
				if (_app_getfileicon (_r_path_expand (PATH_STORE), true, &config.icon_package_id, &config.hicon_package))
				{
					config.hbitmap_package_small = _app_ico2bmp (config.hicon_package);
				}
				else
				{
					config.icon_package_id = config.icon_id;
					config.hicon_package = config.hicon_small;
					config.hbitmap_package_small = _app_ico2bmp (config.hicon_small);
				}
			}

			// allow drag&drop support
			DragAcceptFiles (hwnd, TRUE);

			// initialize settings
			app.AddSettingsPage (IDD_SETTINGS_GENERAL, IDS_SETTINGS_1, &settings_callback);
			app.AddSettingsPage (IDD_SETTINGS_HIGHLIGHTING, IDS_TITLE_HIGHLIGHTING, &settings_callback);

			{
				const size_t page_id = app.AddSettingsPage (IDD_SETTINGS_RULES, IDS_TRAY_RULES, &settings_callback);

				app.AddSettingsPage (IDD_SETTINGS_RULES_BLOCKLIST, IDS_TRAY_BLOCKLIST_RULES, &settings_callback, page_id);
				app.AddSettingsPage (IDD_SETTINGS_RULES_SYSTEM, IDS_TRAY_SYSTEM_RULES, &settings_callback, page_id);
				app.AddSettingsPage (IDD_SETTINGS_RULES_CUSTOM, IDS_TRAY_CUSTOM_RULES, &settings_callback, page_id);
			}

			// dropped packets logging (win7+)
			if (_r_sys_validversion (6, 1))
				app.AddSettingsPage (IDD_SETTINGS_LOG, IDS_TRAY_LOG, &settings_callback);

			app.AddSettingsPage (IDD_SETTINGS_SYSTEM, IDS_TRAY_SYSTEM, &settings_callback);

			// initialize timers
			{
				timers.push_back (15);
				timers.push_back (_R_SECONDSCLOCK_MIN (10));
				timers.push_back (_R_SECONDSCLOCK_MIN (20));
				timers.push_back (_R_SECONDSCLOCK_MIN (30));
				timers.push_back (_R_SECONDSCLOCK_HOUR (1));
				timers.push_back (_R_SECONDSCLOCK_HOUR (2));
				timers.push_back (_R_SECONDSCLOCK_HOUR (4));
				timers.push_back (_R_SECONDSCLOCK_HOUR (6));
			}

			// initialize colors
			{
				addcolor (IDS_HIGHLIGHT_SIGNED, L"IsHighlightSigned", true, L"ColorSigned", LISTVIEW_COLOR_SIGNED);
				addcolor (IDS_HIGHLIGHT_SPECIAL, L"IsHighlightSpecial", true, L"ColorSpecial", LISTVIEW_COLOR_SPECIAL);
				addcolor (IDS_HIGHLIGHT_SYSTEM, L"IsHighlightSystem", true, L"ColorSystem", LISTVIEW_COLOR_SYSTEM);
				addcolor (IDS_HIGHLIGHT_TIMER, L"IsHighlightTimer", true, L"ColorTimer", LISTVIEW_COLOR_TIMER);
				addcolor (IDS_HIGHLIGHT_PACKAGE, L"IsHighlightPackage", true, L"ColorPackage", LISTVIEW_COLOR_PACKAGE);
				addcolor (IDS_HIGHLIGHT_SERVICE, L"IsHighlightService", true, L"ColorService", LISTVIEW_COLOR_SERVICE);
				addcolor (IDS_HIGHLIGHT_PICO, L"IsHighlightPico", true, L"ColorPico", LISTVIEW_COLOR_PICO);
				addcolor (IDS_HIGHLIGHT_SILENT, L"IsHighlightSilent", true, L"ColorSilent", LISTVIEW_COLOR_SILENT);
				addcolor (IDS_HIGHLIGHT_INVALID, L"IsHighlightInvalid", true, L"ColorInvalid", LISTVIEW_COLOR_INVALID);
			}

			// initialize protocols
			{
				addprotocol (L"ICMP", IPPROTO_ICMP);
				addprotocol (L"ICMPv6", IPPROTO_ICMPV6);
				addprotocol (L"IGMP", IPPROTO_IGMP);
				addprotocol (L"IPv4", IPPROTO_IPV4);
				addprotocol (L"IPv6", IPPROTO_IPV6);
				addprotocol (L"L2TP", IPPROTO_L2TP);
				addprotocol (L"RAW", IPPROTO_RAW);
				addprotocol (L"RDP", IPPROTO_RDP);
				addprotocol (L"SCTP", IPPROTO_SCTP);
				addprotocol (L"TCP", IPPROTO_TCP);
				addprotocol (L"UDP", IPPROTO_UDP);
			}

			// initialize thread objects
			config.stop_evt = CreateEvent (nullptr, FALSE, FALSE, nullptr);
			config.finish_evt = CreateEvent (nullptr, FALSE, FALSE, nullptr);

			// initialize dropped packets log callback thread (win7+)
			if (_r_sys_validversion (6, 1))
			{
				config.log_evt = CreateEvent (nullptr, FALSE, FALSE, nullptr);

				log_stack = (PSLIST_HEADER)_aligned_malloc (sizeof (SLIST_HEADER), MEMORY_ALLOCATION_ALIGNMENT);

				if (log_stack)
					InitializeSListHead (log_stack);

				_beginthreadex (nullptr, 0, &LogThread, hwnd, 0, nullptr);
			}

			// create notification window (win7+)
			if (_r_sys_validversion (6, 1))
				_app_notifycreatewindow ();

			// load profile
			_app_profileload (hwnd);
			_app_listviewsort (hwnd, IDC_LISTVIEW, -1, false);

			// initialize winsock (required by getnameinfo)
			{
				WSADATA wsaData = {0};

				if (WSAStartup (WINSOCK_VERSION, &wsaData) == ERROR_SUCCESS)
					config.is_wsainit = true;
			}

			// install filters
			if (_wfp_isfiltersinstalled ())
				_app_installfilters (true);

			break;
		}

		case WM_DROPFILES:
		{
			UINT numfiles = DragQueryFile ((HDROP)wparam, 0xFFFFFFFF, nullptr, 0);
			size_t item = 0;

			_r_fastlock_acquireexclusive (&lock_access);

			for (UINT i = 0; i < numfiles; i++)
			{
				const UINT length = DragQueryFile ((HDROP)wparam, i, nullptr, 0) + 1;

				LPWSTR file = new WCHAR[length];

				if (file)
				{
					DragQueryFile ((HDROP)wparam, i, file, length);

					item = _app_addapplication (hwnd, file, 0, false, false, false);

					delete[] file;
				}
			}

			_r_fastlock_releaseexclusive (&lock_access);

			_app_listviewsort (hwnd, IDC_LISTVIEW, -1, false);
			_app_profilesave (hwnd);

			ShowItem (hwnd, IDC_LISTVIEW, _app_getposition (hwnd, item), -1);

			DragFinish ((HDROP)wparam);

			break;
		}

		case WM_CLOSE:
		{
			if (_app_istimersactive ())
			{
				if (!app.ConfirmMessage (hwnd, nullptr, app.LocaleString (IDS_QUESTION_TIMER, nullptr), L"ConfirmExitTimer"))
				{
					SetWindowLongPtr (hwnd, DWLP_MSGRESULT, TRUE);
					return true;
				}
			}
			else
			{
				if (!app.ConfirmMessage (hwnd, nullptr, app.LocaleString (IDS_QUESTION_EXIT, nullptr), L"ConfirmExit2"))
					return true;
			}

			DestroyWindow (hwnd);

			break;
		}

		case WM_DESTROY:
		{
			app.ConfigSet (L"Group1IsCollaped", ((SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETGROUPSTATE, 0, LVGS_COLLAPSED) & LVGS_COLLAPSED) != 0) ? true : false);
			app.ConfigSet (L"Group2IsCollaped", ((SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETGROUPSTATE, 1, LVGS_COLLAPSED) & LVGS_COLLAPSED) != 0) ? true : false);

			_app_timer_apply (hwnd, true);
			app.TrayDestroy (UID);

			DestroyWindow (config.hnotification);
			UnregisterClass (NOTIFY_CLASS_DLG, app.GetHINSTANCE ());

			ImageList_Destroy (config.himg);

			_app_profilesave (hwnd);

			if (_r_fastlock_islocked (&lock_apply))
				WaitForSingleObjectEx (config.finish_evt, 8000, FALSE);

			_wfp_uninitialize (false);

			if (_r_sys_validversion (6, 1) && log_stack)
				_aligned_free (log_stack);

			PostQuitMessage (0);

			break;
		}

		case WM_PAINT:
		{
			PAINTSTRUCT ps = {0};
			HDC hdc = BeginPaint (hwnd, &ps);

			RECT rc = {0};
			GetWindowRect (GetDlgItem (hwnd, IDC_LISTVIEW), &rc);

			for (INT i = 0; i < rc.right; i++)
				SetPixel (hdc, i, rc.bottom - rc.top, GetSysColor (COLOR_APPWORKSPACE));

			EndPaint (hwnd, &ps);

			break;
		}

		case WM_SETTINGCHANGE:
		{
			_app_notifyrefresh ();
			break;
		}

		case WM_NOTIFY:
		{
			LPNMHDR nmlp = (LPNMHDR)lparam;

			switch (nmlp->code)
			{
				case NM_CUSTOMDRAW:
				{
					if (nmlp->idFrom != IDC_LISTVIEW)
						break;

					SetWindowLongPtr (hwnd, DWLP_MSGRESULT, _app_wmcustdraw ((LPNMLVCUSTOMDRAW)lparam, 0));
					return TRUE;
				}

				case LVN_COLUMNCLICK:
				{
					LPNMLISTVIEW pnmv = (LPNMLISTVIEW)lparam;

					_app_listviewsort (hwnd, IDC_LISTVIEW, pnmv->iSubItem, true);

					break;
				}

				case LVN_GETINFOTIP:
				{
					LPNMLVGETINFOTIP lpnmlv = (LPNMLVGETINFOTIP)lparam;

					const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, (UINT)lpnmlv->hdr.idFrom, lpnmlv->iItem);

					if (hash)
					{
						StringCchCopy (lpnmlv->pszText, lpnmlv->cchTextMax, _app_gettooltip (hash));
					}

					break;
				}

				case LVN_ITEMCHANGED:
				{
					LPNMLISTVIEW lpnmlv = (LPNMLISTVIEW)lparam;

					if (lpnmlv->uNewState == 8192 || lpnmlv->uNewState == 4096)
					{
						if (config.is_nocheckboxnotify)
							return FALSE;

						const size_t hash = lpnmlv->lParam;
						PITEM_APP ptr_app = _app_getapplication (hash);

						if (ptr_app)
						{
							ptr_app->is_enabled = (lpnmlv->uNewState == 8192) ? true : false;

							_r_listview_setitem (hwnd, IDC_LISTVIEW, lpnmlv->iItem, 0, nullptr, LAST_VALUE, ptr_app->is_enabled ? 0 : 1);

							_r_fastlock_acquireexclusive (&lock_notification);
							_app_freenotify (LAST_VALUE, hash);
							_r_fastlock_releaseexclusive (&lock_notification);

							_app_notifyrefresh ();

							_app_installfilters (false);
						}
					}
					else if (((lpnmlv->uNewState ^ lpnmlv->uOldState) & LVIS_SELECTED) != 0)
					{
						_app_refreshstatus (hwnd, true, false);
					}

					break;
				}

				case LVN_GETEMPTYMARKUP:
				{
					NMLVEMPTYMARKUP* lpnmlv = (NMLVEMPTYMARKUP*)lparam;

					lpnmlv->dwFlags = EMF_CENTERED;
					StringCchCopy (lpnmlv->szMarkup, _countof (lpnmlv->szMarkup), app.LocaleString (IDS_STATUS_EMPTY, nullptr));

					SetWindowLongPtr (hwnd, DWLP_MSGRESULT, TRUE);
					return TRUE;
				}

				case NM_DBLCLK:
				{
					LPNMITEMACTIVATE lpnmlv = (LPNMITEMACTIVATE)lparam;

					if (lpnmlv->iItem != -1)
						PostMessage (hwnd, WM_COMMAND, MAKELPARAM (IDM_EXPLORE, 0), 0);

					break;
				}
			}

			break;
		}

		case WM_MENUSELECT:
		{
			if (!IsWindowVisible (hwnd) || IsIconic (hwnd))
				break;

			std::vector<ITEM_ADD>* vc_ptr = nullptr;
			size_t idx = LAST_VALUE;

			// show process information in statusbar
			if (GetMenuState ((HMENU)lparam, LOWORD (wparam), MF_BYCOMMAND) != 0xFFFFFFFF)
			{
				if ((LOWORD (wparam) >= IDX_PROCESS) && LOWORD (wparam) <= (IDX_PROCESS + processes.size ()))
				{
					vc_ptr = &processes;
					idx = LOWORD (wparam) - IDX_PROCESS;
				}
				else if ((LOWORD (wparam) >= IDX_PACKAGE) && LOWORD (wparam) <= (IDX_PACKAGE + packages.size ()))
				{
					vc_ptr = &packages;
					idx = LOWORD (wparam) - IDX_PACKAGE;
				}
				else if ((LOWORD (wparam) >= IDX_SERVICE) && LOWORD (wparam) <= (IDX_SERVICE + services.size ()))
				{
					vc_ptr = &services;
					idx = LOWORD (wparam) - IDX_SERVICE;
				}
			}

			if (vc_ptr && idx != LAST_VALUE)
			{
				if (((HIWORD (wparam) & MF_HILITE) != 0) || ((HIWORD (wparam) & MF_MOUSESELECT) != 0))
				{
					PITEM_ADD const ptr_app = &vc_ptr->at (idx);

					if (ptr_app)
						_r_status_settext (hwnd, IDC_STATUSBAR, 0, ptr_app->real_path[0] ? ptr_app->real_path : ptr_app->display_name);
				}
			}
			else
			{
				_app_refreshstatus (hwnd, true, false);
			}

			break;
		}

		case WM_CONTEXTMENU:
		{
			if (GetDlgCtrlID ((HWND)wparam) == IDC_LISTVIEW)
			{
				const UINT usettings_id = 2;
				const UINT utimer_id = 3;
				const UINT selected_count = (UINT)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETSELECTEDCOUNT, 0, 0);

				const HMENU menu = LoadMenu (nullptr, MAKEINTRESOURCE (IDM_LISTVIEW));
				const HMENU submenu = GetSubMenu (menu, 0);
				const HMENU submenu_add = GetSubMenu (submenu, 0);
				const HMENU submenu_settings = GetSubMenu (submenu, usettings_id);
				const HMENU submenu_timer = GetSubMenu (submenu, utimer_id);

				// localize
				app.LocaleMenu (submenu, IDS_ADD, 0, true, nullptr);
				app.LocaleMenu (submenu, IDS_TRAY_RULES, usettings_id, true, nullptr);
				app.LocaleMenu (submenu, IDS_DISABLENOTIFICATIONS, IDM_DISABLENOTIFICATIONS, false, nullptr);
				app.LocaleMenu (submenu, IDS_TIMER, utimer_id, true, nullptr);
				app.LocaleMenu (submenu, IDS_NOTIMER, IDM_DISABLETIMER, false, nullptr);
				app.LocaleMenu (submenu, IDS_REFRESH, IDM_REFRESH2, false, L"\tF5");
				app.LocaleMenu (submenu, IDS_EXPLORE, IDM_EXPLORE, false, nullptr);
				app.LocaleMenu (submenu, IDS_COPY, IDM_COPY, false, L"\tCtrl+C");
				app.LocaleMenu (submenu, IDS_DELETE, IDM_DELETE, false, L"\tDel");
				app.LocaleMenu (submenu, IDS_CHECK, IDM_CHECK, false, nullptr);
				app.LocaleMenu (submenu, IDS_UNCHECK, IDM_UNCHECK, false, nullptr);

				if (!selected_count)
				{
					EnableMenuItem (submenu, usettings_id, MF_BYPOSITION | MF_DISABLED | MF_GRAYED);
					EnableMenuItem (submenu, utimer_id, MF_BYPOSITION | MF_DISABLED | MF_GRAYED);
					EnableMenuItem (submenu, IDM_EXPLORE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					EnableMenuItem (submenu, IDM_COPY, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					EnableMenuItem (submenu, IDM_DELETE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					EnableMenuItem (submenu, IDM_CHECK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					EnableMenuItem (submenu, IDM_UNCHECK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
				}

				_app_generate_addmenu (submenu_add);

				// show configuration
				{
					const size_t item = (size_t)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED); // get first item
					const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, item);

					_r_fastlock_acquireshared (&lock_access);

					PITEM_APP const ptr_app = _app_getapplication (hash);

					if (ptr_app)
					{
						CheckMenuItem (submenu, IDM_DISABLENOTIFICATIONS, MF_BYCOMMAND | (ptr_app->is_silent ? MF_CHECKED : MF_UNCHECKED));

						AppendMenu (submenu_settings, MF_SEPARATOR, 0, nullptr);

						if (rules_custom.empty ())
						{
							AppendMenu (submenu_settings, MF_STRING, IDX_RULES_SPECIAL, app.LocaleString (IDS_STATUS_EMPTY, nullptr));
							EnableMenuItem (submenu_settings, IDX_RULES_SPECIAL, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
						}
						else
						{
							for (size_t i = 0; i < rules_custom.size (); i++)
							{
								PITEM_RULE const ptr_rule = rules_custom.at (i);

								if (ptr_rule)
								{
									MENUITEMINFO mii = {0};

									bool is_checked = (ptr_rule->is_enabled && !ptr_rule->apps.empty () && (ptr_rule->apps.find (hash) != ptr_rule->apps.end ()));

									WCHAR buffer[128] = {0};
									StringCchPrintf (buffer, _countof (buffer), app.LocaleString (IDS_RULE_APPLY, nullptr), app.LocaleString (ptr_rule->is_block ? IDS_ACTION_2 : IDS_ACTION_1, nullptr).GetString (), ptr_rule->pname);

									if (selected_count > 1)
										StringCchCat (buffer, _countof (buffer), _r_fmt (L" (%d)", selected_count));

									mii.cbSize = sizeof (mii);
									mii.fMask = MIIM_ID | MIIM_STATE | MIIM_STRING;
									mii.fType = MFT_STRING;
									mii.dwTypeData = buffer;
									mii.fState = (is_checked ? MF_CHECKED : MF_UNCHECKED);
									mii.wID = IDX_RULES_SPECIAL + UINT (i);

									InsertMenuItem (submenu_settings, mii.wID, FALSE, &mii);
								}
							}
						}

						AppendMenu (submenu_settings, MF_SEPARATOR, 0, nullptr);
						AppendMenu (submenu_settings, MF_STRING, IDM_OPENRULESEDITOR, app.LocaleString (IDS_OPENRULESEDITOR, L"..."));
					}

					// show timers
					{
						bool is_checked = false;
						const time_t current_time = _r_unixtime_now ();

						for (size_t i = 0; i < timers.size (); i++)
						{
							MENUITEMINFO mii = {0};

							WCHAR buffer[128] = {0};
							StringCchCopy (buffer, _countof (buffer), _r_fmt_interval (timers.at (i)));

							mii.cbSize = sizeof (mii);
							mii.fMask = MIIM_ID | MIIM_STRING;
							mii.fType = MFT_STRING;
							mii.dwTypeData = buffer;
							mii.wID = IDX_TIMER + UINT (i);

							InsertMenuItem (submenu_timer, mii.wID, FALSE, &mii);

							if (!is_checked && apps_timer.find (hash) != apps_timer.end () && apps_timer[hash] > current_time && apps_timer[hash] <= (current_time + timers.at (i)))
							{
								CheckMenuRadioItem (submenu_timer, IDX_TIMER, IDX_TIMER + UINT (timers.size ()), mii.wID, MF_BYCOMMAND);
								is_checked = true;
							}
						}

						if (!is_checked)
							CheckMenuRadioItem (submenu_timer, IDM_DISABLETIMER, IDM_DISABLETIMER, IDM_DISABLETIMER, MF_BYCOMMAND);
					}

					_r_fastlock_releaseshared (&lock_access);
				}

				POINT pt = {0};
				GetCursorPos (&pt);

				TrackPopupMenuEx (submenu, TPM_RIGHTBUTTON | TPM_LEFTBUTTON, pt.x, pt.y, hwnd, nullptr);

				DestroyMenu (menu);
			}

			break;
		}

		case WM_SIZE:
		{
			ResizeWindow (hwnd, LOWORD (lparam), HIWORD (lparam));
			RedrawWindow (hwnd, nullptr, nullptr, RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE);

			_app_listviewresize (hwnd, IDC_LISTVIEW);

			break;
		}

		case WM_TRAYICON:
		{
			switch (LOWORD (lparam))
			{
				case NIN_POPUPOPEN:
				{
					if (_app_notifyshow (_app_notifygetcurrent (), true))
						_app_notifysettimeout (config.hnotification, NOTIFY_TIMER_POPUP_ID, false, 0);

					break;
				}

				case NIN_POPUPCLOSE:
				{
					_app_notifysettimeout (config.hnotification, NOTIFY_TIMER_POPUP_ID, true, NOTIFY_TIMER_POPUP);
					break;
				}

				case NIN_BALLOONUSERCLICK:
				{
					if (config.is_popuperrors)
					{
						PostMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDM_TRAY_LOGSHOW_ERR, 0), 0);
						config.is_popuperrors = false;
					}

					break;
				}

				case NIN_BALLOONHIDE:
				case NIN_BALLOONTIMEOUT:
				{
					config.is_popuperrors = false;
					break;
				}

				case WM_MBUTTONDOWN:
				{
					PostMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDM_LOGSHOW, 0), 0);
					break;
				}

				case WM_LBUTTONUP:
				{
					SetForegroundWindow (hwnd);
					break;
				}

				case WM_LBUTTONDBLCLK:
				{
					_r_wnd_toggle (hwnd, false);
					break;
				}

				case WM_RBUTTONUP:
				{
					SetForegroundWindow (hwnd); // don't touch

					const UINT mode_id = 3;
					const UINT add_id = 4;
					const UINT notifications_id = 6;
					const UINT errlog_id = 7;

					const HMENU menu = LoadMenu (nullptr, MAKEINTRESOURCE (IDM_TRAY));
					const HMENU submenu = GetSubMenu (menu, 0);

					// localize
					app.LocaleMenu (submenu, IDS_TRAY_SHOW, IDM_TRAY_SHOW, false, nullptr);
					app.LocaleMenu (submenu, _wfp_isfiltersinstalled () ? IDS_TRAY_STOP : IDS_TRAY_START, IDM_TRAY_START, false, L"...");
					app.LocaleMenu (submenu, IDS_TRAY_MODE, mode_id, true, nullptr);
					app.LocaleMenu (submenu, IDS_MODE_WHITELIST, IDM_TRAY_MODEWHITELIST, false, nullptr);
					app.LocaleMenu (submenu, IDS_MODE_BLACKLIST, IDM_TRAY_MODEBLACKLIST, false, nullptr);

					app.LocaleMenu (submenu, IDS_ADD, add_id, true, nullptr);
					_app_generate_addmenu (GetSubMenu (submenu, add_id));

					app.LocaleMenu (submenu, IDS_TRAY_LOG, notifications_id, true, nullptr);
					app.LocaleMenu (submenu, IDS_ENABLELOG_CHK, IDM_TRAY_ENABLELOG_CHK, false, nullptr);
					app.LocaleMenu (submenu, IDS_ENABLENOTIFICATIONS_CHK, IDM_TRAY_ENABLENOTIFICATIONS_CHK, false, nullptr);
					app.LocaleMenu (submenu, IDS_LOGSHOW, IDM_TRAY_LOGSHOW, false, nullptr);
					app.LocaleMenu (submenu, IDS_LOGCLEAR, IDM_TRAY_LOGCLEAR, false, nullptr);

					app.LocaleMenu (submenu, IDS_TRAY_LOGERR, errlog_id, true, nullptr);

					if (_r_fs_exists (_r_dbg_getpath (APP_NAME_SHORT)))
					{
						app.LocaleMenu (submenu, IDS_LOGSHOW, IDM_TRAY_LOGSHOW_ERR, false, nullptr);
						app.LocaleMenu (submenu, IDS_LOGCLEAR, IDM_TRAY_LOGCLEAR_ERR, false, nullptr);
					}
					else
					{
						EnableMenuItem (submenu, errlog_id, MF_BYPOSITION | MF_DISABLED | MF_GRAYED);
					}

					app.LocaleMenu (submenu, IDS_SETTINGS, IDM_TRAY_SETTINGS, false, L"...");
					app.LocaleMenu (submenu, IDS_WEBSITE, IDM_TRAY_WEBSITE, false, nullptr);
					app.LocaleMenu (submenu, IDS_ABOUT, IDM_TRAY_ABOUT, false, nullptr);
					app.LocaleMenu (submenu, IDS_EXIT, IDM_TRAY_EXIT, false, nullptr);

					CheckMenuItem (submenu, IDM_TRAY_ENABLELOG_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsLogEnabled", false).AsBool () ? MF_CHECKED : MF_UNCHECKED));
					CheckMenuItem (submenu, IDM_TRAY_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsNotificationsEnabled", true).AsBool () ? MF_CHECKED : MF_UNCHECKED));

					if (_r_fastlock_islocked (&lock_apply))
						EnableMenuItem (submenu, IDM_TRAY_START, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);

					// dropped packets logging (win7+)
					if (!_r_sys_validversion (6, 1))
					{
						EnableMenuItem (submenu, IDM_TRAY_ENABLELOG_CHK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
						EnableMenuItem (submenu, IDM_TRAY_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					}

					CheckMenuRadioItem (submenu, IDM_TRAY_MODEWHITELIST, IDM_TRAY_MODEBLACKLIST, IDM_TRAY_MODEWHITELIST + app.ConfigGet (L"Mode", ModeWhitelist).AsUint (), MF_BYCOMMAND);

					POINT pt = {0};
					GetCursorPos (&pt);

					TrackPopupMenuEx (submenu, TPM_RIGHTBUTTON | TPM_LEFTBUTTON, pt.x, pt.y, hwnd, nullptr);

					DestroyMenu (menu);

					break;
				}
			}

			break;
		}

		case WM_POWERBROADCAST:
		{
			switch (wparam)
			{
				case PBT_APMRESUMESUSPEND:
				{
					app.ConfigInit ();

					_app_profileload (hwnd);

					if (_wfp_isfiltersinstalled ())
					{
						if (_wfp_initialize (true))
							_app_installfilters (true);
					}
					else
					{
						_r_listview_redraw (hwnd, IDC_LISTVIEW);
					}

					break;
				}

				case PBT_APMSUSPEND:
				{
					// clear notifications
					_r_fastlock_acquireexclusive (&lock_notification);

					if (!notifications.empty ())
					{
						_app_notifyhide (config.hnotification);

						for (size_t i = notifications.size () - 1; i != LAST_VALUE; i--)
						{
							PITEM_LOG ptr_log = notifications.at (i);

							if (ptr_log)
							{
								delete ptr_log;
								ptr_log = nullptr;
							}
						}

						notifications.clear ();
					}

					_r_fastlock_releaseexclusive (&lock_notification);

					_app_profilesave (hwnd);
					_wfp_uninitialize (false);

					break;
				}
			}

			break;
		}

		case WM_DEVICECHANGE:
		{
			if (wparam == DBT_DEVICEARRIVAL)
			{
				const PDEV_BROADCAST_HDR lbhdr = (PDEV_BROADCAST_HDR)lparam;

				if (lbhdr->dbch_devicetype == DBT_DEVTYP_VOLUME)
					_app_installfilters (false);
			}

			break;
		}

		case WM_COMMAND:
		{
			if (HIWORD (wparam) == 0 && LOWORD (wparam) >= IDX_LANGUAGE && LOWORD (wparam) <= IDX_LANGUAGE + app.LocaleGetCount ())
			{
				app.LocaleApplyFromMenu (GetSubMenu (GetSubMenu (GetMenu (hwnd), 2), LANG_MENU), LOWORD (wparam), IDX_LANGUAGE);
				return FALSE;
			}
			else if ((LOWORD (wparam) >= IDX_PROCESS && LOWORD (wparam) <= IDX_PROCESS + processes.size ()))
			{
				PITEM_ADD const ptr_proc = &processes.at (LOWORD (wparam) - IDX_PROCESS);

				const size_t hash = _app_addapplication (hwnd, ptr_proc->real_path, 0, false, false, true);

				_app_listviewsort (hwnd, IDC_LISTVIEW, -1, false);
				_app_profilesave (hwnd);

				ShowItem (hwnd, IDC_LISTVIEW, _app_getposition (hwnd, hash), -1);

				return FALSE;
			}
			else if ((LOWORD (wparam) >= IDX_PACKAGE && LOWORD (wparam) <= IDX_PACKAGE + packages.size ()))
			{
				PITEM_ADD const ptr_package = &packages.at (LOWORD (wparam) - IDX_PACKAGE);

				const size_t hash = _app_addapplication (hwnd, ptr_package->sid, 0, false, false, true);

				_app_listviewsort (hwnd, IDC_LISTVIEW, -1, false);
				_app_profilesave (hwnd);

				ShowItem (hwnd, IDC_LISTVIEW, _app_getposition (hwnd, hash), -1);

				return FALSE;
			}
			else if ((LOWORD (wparam) >= IDX_SERVICE && LOWORD (wparam) <= IDX_SERVICE + services.size ()))
			{
				PITEM_ADD const ptr_svc = &services.at (LOWORD (wparam) - IDX_SERVICE);

				const size_t hash = _app_addapplication (hwnd, ptr_svc->service_name, 0, false, false, true);

				_app_listviewsort (hwnd, IDC_LISTVIEW, -1, false);
				_app_profilesave (hwnd);

				ShowItem (hwnd, IDC_LISTVIEW, _app_getposition (hwnd, hash), -1);

				return FALSE;
			}
			else if ((LOWORD (wparam) >= IDX_RULES_SPECIAL && LOWORD (wparam) <= IDX_RULES_SPECIAL + rules_custom.size ()))
			{
				const size_t idx = (LOWORD (wparam) - IDX_RULES_SPECIAL);

				INT item = -1;
				BOOL is_remove = (BOOL)-1;

				_r_fastlock_acquireexclusive (&lock_access);

				PITEM_RULE ptr_rule = rules_custom.at (idx);

				if (!ptr_rule)
					return FALSE;

				while ((item = (INT)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, item, LVNI_SELECTED)) != -1)
				{
					const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, item);

					PITEM_APP ptr_app = _app_getapplication (hash);

					if (!ptr_app)
						continue;

					if (is_remove == (BOOL)-1)
						is_remove = (ptr_rule->is_enabled && !ptr_rule->apps.empty () && (ptr_rule->apps.find (hash) != ptr_rule->apps.end ()));

					if (is_remove)
					{
						ptr_rule->apps.erase (hash);

						if (ptr_rule->apps.empty ())
							ptr_rule->is_enabled = false;
					}
					else
					{
						ptr_rule->apps[hash] = true;
						ptr_rule->is_enabled = true;
					}
				}

				_r_fastlock_releaseexclusive (&lock_access);

				_app_profilesave (hwnd);

				_app_installfilters (false);

				return FALSE;
			}
			else if ((LOWORD (wparam) >= IDX_TIMER && LOWORD (wparam) <= IDX_TIMER + timers.size ()))
			{
				if (!SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETSELECTEDCOUNT, 0, 0))
					break;

				const size_t idx = (LOWORD (wparam) - IDX_TIMER);

				size_t item = LAST_VALUE;
				const time_t current_time = _r_unixtime_now ();

				_r_fastlock_acquireexclusive (&lock_access);

				while ((item = (INT)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, item, LVNI_SELECTED)) != -1)
				{
					const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, item);

					if (hash)
						apps_timer[hash] = current_time + timers.at (idx);
				}

				_r_fastlock_releaseexclusive (&lock_access);

				if (_app_timer_apply (hwnd, false))
				{
					_app_profilesave (hwnd);
					_app_installfilters (false);
				}

				return FALSE;
			}

			switch (LOWORD (wparam))
			{
				case IDCANCEL: // process Esc key
				case IDM_TRAY_SHOW:
				{
					_r_wnd_toggle (hwnd, false);
					break;
				}

				case IDM_SETTINGS:
				case IDM_TRAY_SETTINGS:
				case IDC_SETTINGS_BTN:
				{
					app.CreateSettingsWindow ();
					break;
				}

				case IDM_EXIT:
				case IDM_TRAY_EXIT:
				case IDC_EXIT_BTN:
				{
					SendMessage (hwnd, WM_CLOSE, 0, 0);
					break;
				}

				case IDM_WEBSITE:
				case IDM_TRAY_WEBSITE:
				{
					ShellExecute (hwnd, nullptr, _APP_WEBSITE_URL, nullptr, nullptr, SW_SHOWDEFAULT);
					break;
				}

				case IDM_CHECKUPDATES:
				{
					app.CheckForUpdates (false);
					break;
				}

				case IDM_ABOUT:
				case IDM_TRAY_ABOUT:
				{
					app.CreateAboutWindow (hwnd, app.LocaleString (IDS_DONATE, nullptr));
					break;
				}

				case IDM_EXPORT_APPS:
				case IDM_EXPORT_RULES:
				{
					WCHAR path[MAX_PATH] = {0};
					StringCchCopy (path, _countof (path), ((LOWORD (wparam) == IDM_EXPORT_APPS) ? XML_APPS : XML_RULES_CUSTOM));

					OPENFILENAME ofn = {0};

					ofn.lStructSize = sizeof (ofn);
					ofn.hwndOwner = hwnd;
					ofn.lpstrFile = path;
					ofn.nMaxFile = _countof (path);
					ofn.lpstrFilter = L"*.xml\0*.xml\0\0";
					ofn.lpstrDefExt = L"xml";
					ofn.Flags = OFN_EXPLORER | OFN_ENABLESIZING | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;

					if (GetSaveFileName (&ofn))
					{
						_app_profilesave (hwnd, ((LOWORD (wparam) == IDM_EXPORT_APPS) ? path : nullptr), ((LOWORD (wparam) == IDM_EXPORT_RULES) ? path : nullptr));
					}

					break;
				}

				case IDM_IMPORT_APPS:
				case IDM_IMPORT_RULES:
				{
					WCHAR path[MAX_PATH] = {0};
					StringCchCopy (path, _countof (path), ((LOWORD (wparam) == IDM_IMPORT_APPS) ? XML_APPS : XML_RULES_CUSTOM));

					OPENFILENAME ofn = {0};

					ofn.lStructSize = sizeof (ofn);
					ofn.hwndOwner = hwnd;
					ofn.lpstrFile = path;
					ofn.nMaxFile = _countof (path);
					ofn.lpstrFilter = L"*.xml\0*.xml\0\0";
					ofn.lpstrDefExt = L"xml";
					ofn.Flags = OFN_EXPLORER | OFN_ENABLESIZING | OFN_PATHMUSTEXIST | OFN_FORCESHOWHIDDEN;

					if (GetOpenFileName (&ofn))
					{
						// make backup
						if (LOWORD (wparam) == IDM_IMPORT_APPS)
							_r_fs_copy (config.apps_path, _r_fmt (L"%s.bak", config.apps_path));

						else if (LOWORD (wparam) == IDM_IMPORT_RULES)
							_r_fs_copy (config.rules_custom_path, _r_fmt (L"%s.bak", config.rules_custom_path));

						_app_profileload (hwnd, ((LOWORD (wparam) == IDM_IMPORT_APPS) ? path : nullptr), ((LOWORD (wparam) == IDM_IMPORT_RULES) ? path : nullptr));
						_app_profilesave (hwnd);

						_app_installfilters (false);
					}

					break;
				}

				case IDM_ALWAYSONTOP_CHK:
				{
					const bool new_val = !app.ConfigGet (L"AlwaysOnTop", false).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_ALWAYSONTOP_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"AlwaysOnTop", new_val);

					_r_wnd_top (hwnd, new_val);

					break;
				}

				case IDM_SHOWFILENAMESONLY_CHK:
				{
					const bool new_val = !app.ConfigGet (L"ShowFilenames", true).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_SHOWFILENAMESONLY_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"ShowFilenames", new_val);

					_app_profilesave (hwnd);
					_app_profileload (hwnd);

					_app_listviewsort (hwnd, IDC_LISTVIEW, -1, false);

					_r_listview_redraw (hwnd, IDC_LISTVIEW);

					break;
				}

				case IDM_AUTOSIZECOLUMNS_CHK:
				{
					const bool new_val = !app.ConfigGet (L"AutoSizeColumns", true).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_AUTOSIZECOLUMNS_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"AutoSizeColumns", new_val);

					_app_listviewresize (hwnd, IDC_LISTVIEW);

					break;
				}

				case IDM_ICONSSMALL:
				case IDM_ICONSLARGE:
				{
					app.ConfigSet (L"IsLargeIcons", (LOWORD (wparam) == IDM_ICONSLARGE) ? true : false);

					_app_listviewsetimagelist (hwnd, IDC_LISTVIEW);

					break;
				}

				case IDM_ICONSISHIDDEN:
				{
					app.ConfigSet (L"IsIconsHidden", !app.ConfigGet (L"IsIconsHidden", false).AsBool ());

					_app_listviewsetimagelist (hwnd, IDC_LISTVIEW);

					_app_profilesave (hwnd);
					_app_profileload (hwnd);

					_r_listview_redraw (hwnd, IDC_LISTVIEW);

					break;
				}

				case IDM_FONT:
				{
					CHOOSEFONT cf = {0};

					LOGFONT lf = {0};

					cf.lStructSize = sizeof (cf);
					cf.hwndOwner = hwnd;
					cf.Flags = CF_INITTOLOGFONTSTRUCT | CF_NOSCRIPTSEL | CF_LIMITSIZE | CF_NOVERTFONTS;
					cf.nSizeMax = 14;
					cf.nSizeMin = 8;
					cf.lpLogFont = &lf;

					_app_listviewinitfont (&lf);

					if (ChooseFont (&cf))
					{
						if (config.hfont)
						{
							DeleteObject (config.hfont);
							config.hfont = nullptr;
						}

						if (lf.lfFaceName[0])
						{
							app.ConfigSet (L"Font", _r_fmt (L"%s;%d;%d", lf.lfFaceName, _r_dc_fontheighttosize (lf.lfHeight), lf.lfWeight));
						}
						else
						{
							app.ConfigSet (L"Font", UI_FONT_DEFAULT);
						}

						_app_listviewsetfont (hwnd, IDC_LISTVIEW);
					}

					break;
				}

				case IDM_TRAY_MODEWHITELIST:
				case IDM_TRAY_MODEBLACKLIST:
				{
					DWORD current_mode = ModeWhitelist;

					if (LOWORD (wparam) == IDM_TRAY_MODEBLACKLIST)
						current_mode = ModeBlacklist;

					if ((app.ConfigGet (L"Mode", ModeWhitelist).AsUint () == current_mode) || _r_msg (hwnd, MB_YESNO | MB_ICONEXCLAMATION | MB_TOPMOST, APP_NAME, nullptr, app.LocaleString (IDS_QUESTION_MODE, nullptr), app.LocaleString ((current_mode == ModeWhitelist) ? IDS_MODE_WHITELIST : IDS_MODE_BLACKLIST, nullptr).GetString ()) != IDYES)
						break;

					app.ConfigSet (L"Mode", current_mode);

					CheckMenuRadioItem (GetMenu (hwnd), IDM_TRAY_MODEWHITELIST, IDM_TRAY_MODEBLACKLIST, IDM_TRAY_MODEWHITELIST + current_mode, MF_BYCOMMAND);

					_app_refreshstatus (hwnd, false, true);
					_app_installfilters (false);

					break;
				}

				case IDM_FIND:
				{
					if (!config.hfind)
					{
						static FINDREPLACE fr = {0}; // "static" is required for WM_FINDMSGSTRING

						fr.lStructSize = sizeof (fr);
						fr.hwndOwner = hwnd;
						fr.lpstrFindWhat = config.search_string;
						fr.wFindWhatLen = _countof (config.search_string) - 1;
						fr.Flags = FR_HIDEWHOLEWORD | FR_HIDEMATCHCASE | FR_HIDEUPDOWN | FR_FINDNEXT;

						config.hfind = FindText (&fr);
					}
					else
					{
						SetFocus (config.hfind);
					}

					break;
				}

				case IDM_FINDNEXT:
				{
					if (!config.search_string[0])
					{
						PostMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDM_FIND, 0), 0);
					}
					else
					{
						FINDREPLACE fr = {0};

						fr.Flags = FR_FINDNEXT;
						fr.lpstrFindWhat = config.search_string;

						SendMessage (hwnd, WM_FINDMSGSTRING, 0, (LPARAM)&fr);
					}

					break;
				}

				case IDM_REFRESH:
				case IDM_REFRESH2:
				{
					//_app_profilesave (hwnd);
					_app_profileload (hwnd);

					_app_installfilters (false);

					break;
				}

				case IDM_ENABLELOG_CHK:
				case IDM_TRAY_ENABLELOG_CHK:
				{
					const bool new_val = !app.ConfigGet (L"IsLogEnabled", false).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_ENABLELOG_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"IsLogEnabled", new_val);

					_app_loginit (new_val);

					break;
				}

				case IDM_ENABLENOTIFICATIONS_CHK:
				case IDM_TRAY_ENABLENOTIFICATIONS_CHK:
				{
					const bool new_val = !app.ConfigGet (L"IsNotificationsEnabled", true).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"IsNotificationsEnabled", new_val);

					_app_notifyrefresh ();

					break;
				}

				case IDM_LOGSHOW:
				case IDM_TRAY_LOGSHOW:
				{
					rstring path = _r_path_expand (app.ConfigGet (L"LogPath", PATH_LOG));

					if (!_r_fs_exists (path))
						return FALSE;

					_r_run (nullptr, _r_fmt (L"%s \"%s\"", app.ConfigGet (L"LogViewer", L"notepad.exe").GetString (), path.GetString ()));

					break;
				}

				case IDM_LOGCLEAR:
				case IDM_TRAY_LOGCLEAR:
				{
					rstring path = _r_path_expand (app.ConfigGet (L"LogPath", PATH_LOG));

					if ((config.hlog != nullptr && config.hlog != INVALID_HANDLE_VALUE) || _r_fs_exists (path))
					{
						if (!app.ConfirmMessage (hwnd, nullptr, app.LocaleString (IDS_QUESTION, nullptr), L"ConfirmLogClear"))
							break;

						if (config.hlog != nullptr && config.hlog != INVALID_HANDLE_VALUE)
						{
							_r_fastlock_acquireexclusive (&lock_writelog);

							SetFilePointer (config.hlog, 2, nullptr, FILE_BEGIN);
							SetEndOfFile (config.hlog);

							_r_fastlock_releaseexclusive (&lock_writelog);
						}
						else
						{
							_r_fs_delete (path);
						}

						_r_fs_delete (_r_fmt (L"%s.bak", path.GetString ()));
					}

					break;
				}

				case IDM_TRAY_LOGSHOW_ERR:
				{
					rstring path = _r_dbg_getpath (APP_NAME_SHORT);

					if (!_r_fs_exists (path))
						return FALSE;

					_r_run (nullptr, _r_fmt (L"%s \"%s\"", app.ConfigGet (L"LogViewer", L"notepad.exe").GetString (), path.GetString ()));

					break;
				}

				case IDM_TRAY_LOGCLEAR_ERR:
				{
					rstring path = _r_dbg_getpath (APP_NAME_SHORT);

					if (!_r_fs_exists (path) || !app.ConfirmMessage (hwnd, nullptr, app.LocaleString (IDS_QUESTION, nullptr), L"ConfirmLogClear"))
						break;

					_r_fs_delete (path);

					break;
				}

				case IDM_TRAY_START:
				case IDC_START_BTN:
				{
					const bool state = !_wfp_isfiltersinstalled ();

					if (_app_installmessage (hwnd, state))
					{
						if (state)
							_app_installfilters (true);
						else
							_app_uninstallfilters ();
					}

					break;
				}

				case IDM_ADD_FILE:
				{
					WCHAR files[_R_BUFFER_LENGTH] = {0};
					OPENFILENAME ofn = {0};

					size_t item = 0;

					ofn.lStructSize = sizeof (ofn);
					ofn.hwndOwner = hwnd;
					ofn.lpstrFile = files;
					ofn.nMaxFile = _countof (files);
					ofn.lpstrFilter = L"*.exe\0*.exe\0*.*\0*.*\0\0";
					ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_ENABLESIZING | OFN_PATHMUSTEXIST | OFN_FORCESHOWHIDDEN;

					if (GetOpenFileName (&ofn))
					{
						_r_fastlock_acquireexclusive (&lock_access);

						if (files[ofn.nFileOffset - 1] != 0)
						{
							item = _app_addapplication (hwnd, files, 0, false, false, false);
						}
						else
						{
							LPWSTR p = files;
							WCHAR dir[MAX_PATH] = {0};
							GetCurrentDirectory (_countof (dir), dir);

							while (*p)
							{
								p += wcslen (p) + 1;

								if (*p)
									item = _app_addapplication (hwnd, _r_fmt (L"%s\\%s", dir, p), 0, false, false, false);
							}
						}

						_r_fastlock_releaseexclusive (&lock_access);

						_app_listviewsort (hwnd, IDC_LISTVIEW, -1, false);
						_app_profilesave (hwnd);

						ShowItem (hwnd, IDC_LISTVIEW, _app_getposition (hwnd, item), -1);
					}

					break;
				}

				case IDM_ALL_PROCESSES:
				case IDM_ALL_PACKAGES:
				case IDM_ALL_SERVICES:
				{
					if (LOWORD (wparam) == IDM_ALL_PROCESSES)
					{
						_app_generate_processes ();

						for (size_t i = 0; i < processes.size (); i++)
							_app_addapplication (hwnd, processes.at (i).real_path, 0, false, false, true);
					}
					else if (LOWORD (wparam) == IDM_ALL_PACKAGES)
					{
						for (size_t i = 0; i < packages.size (); i++)
							_app_addapplication (hwnd, packages.at (i).sid, 0, false, false, true);
					}
					else if (LOWORD (wparam) == IDM_ALL_SERVICES)
					{
						for (size_t i = 0; i < services.size (); i++)
							_app_addapplication (hwnd, services.at (i).service_name, 0, false, false, true);
					}

					_app_listviewsort (hwnd, IDC_LISTVIEW, -1, false);
					_app_profilesave (hwnd);

					break;
				}

				case IDM_DISABLENOTIFICATIONS:
				case IDM_DISABLETIMER:
				case IDM_EXPLORE:
				case IDM_COPY:
				case IDM_CHECK:
				case IDM_UNCHECK:
				{
					INT item = -1;
					BOOL new_val = BOOL (-1);

					rstring buffer;

					_r_fastlock_acquireexclusive (&lock_access);

					while ((item = (INT)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, item, LVNI_SELECTED)) != -1)
					{
						const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, item);

						PITEM_APP ptr_app = _app_getapplication (hash);

						if (!ptr_app)
							continue;

						if (LOWORD (wparam) == IDM_EXPLORE)
						{
							if (ptr_app->type != AppPico)
							{
								if (_r_fs_exists (ptr_app->real_path))
									_r_run (nullptr, _r_fmt (L"\"explorer.exe\" /select,\"%s\"", ptr_app->real_path));

								else if (_r_fs_exists (_r_path_extractdir (ptr_app->real_path)))
									ShellExecute (hwnd, nullptr, _r_path_extractdir (ptr_app->real_path), nullptr, nullptr, SW_SHOWDEFAULT);
							}
						}
						else if (LOWORD (wparam) == IDM_COPY)
						{
							buffer.Append (ptr_app->display_name).Append (L"\r\n");
						}
						else if (LOWORD (wparam) == IDM_DISABLENOTIFICATIONS)
						{
							if (new_val == BOOL (-1))
								new_val = !ptr_app->is_silent;

							ptr_app->is_silent = new_val ? true : false;

							if (new_val)
							{
								_r_fastlock_acquireexclusive (&lock_notification);
								_app_freenotify (LAST_VALUE, hash);
								_r_fastlock_releaseexclusive (&lock_notification);
							}
						}
						else if (LOWORD (wparam) == IDM_DISABLETIMER)
						{
							if (apps_timer.find (hash) != apps_timer.end ())
								apps_timer[hash] = 0;
						}
						else if (LOWORD (wparam) == IDM_CHECK || LOWORD (wparam) == IDM_UNCHECK)
						{
							ptr_app->is_enabled = (LOWORD (wparam) == IDM_CHECK) ? true : false;

							config.is_nocheckboxnotify = true;

							_r_listview_setitem (hwnd, IDC_LISTVIEW, item, 0, nullptr, LAST_VALUE, ptr_app->is_enabled ? 0 : 1);
							_r_listview_setitemcheck (hwnd, IDC_LISTVIEW, item, ptr_app->is_enabled);

							config.is_nocheckboxnotify = false;

							_r_fastlock_acquireexclusive (&lock_notification);
							_app_freenotify (LAST_VALUE, hash);
							_r_fastlock_releaseexclusive (&lock_notification);
						}
					}

					_r_fastlock_releaseexclusive (&lock_access);

					if (LOWORD (wparam) == IDM_CHECK || LOWORD (wparam) == IDM_UNCHECK)
					{
						_app_notifyrefresh ();
						_app_profilesave (hwnd);

						_app_installfilters (false);
					}
					else if (LOWORD (wparam) == IDM_DISABLENOTIFICATIONS)
					{
						_app_notifyrefresh ();
						_app_profilesave (hwnd);

						_r_listview_redraw (hwnd, IDC_LISTVIEW);
					}
					else if (LOWORD (wparam) == IDM_DISABLETIMER)
					{
						if (_app_timer_apply (hwnd, false))
						{
							_app_profilesave (hwnd);
							_app_installfilters (false);
						}

						_r_listview_redraw (hwnd, IDC_LISTVIEW);
					}
					else if (LOWORD (wparam) == IDM_COPY)
					{
						buffer.Trim (L"\r\n");
						_r_clipboard_set (hwnd, buffer, buffer.GetLength ());
					}

					break;
				}

				case IDM_OPENRULESEDITOR:
				{
					PITEM_RULE ptr_rule = new ITEM_RULE;

					if (ptr_rule)
					{
						ptr_rule->is_block = true; // block by default

						INT item = -1;

						while ((item = (INT)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, item, LVNI_SELECTED)) != -1)
						{
							const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, item);

							if (hash)
								ptr_rule->apps[hash] = true;
						}

						SetWindowLongPtr (hwnd, GWLP_USERDATA, LAST_VALUE);
						if (DialogBoxParam (nullptr, MAKEINTRESOURCE (IDD_EDITOR), hwnd, &EditorProc, (LPARAM)ptr_rule))
						{
							_r_fastlock_acquireexclusive (&lock_access);

							rules_custom.push_back (ptr_rule);

							_r_fastlock_releaseexclusive (&lock_access);

							_app_profilesave (hwnd);

							_app_installfilters (false);
						}
						else
						{
							_app_freerule (&ptr_rule);
						}
					}

					break;
				}

				case IDM_DELETE:
				{
					const UINT selected = (UINT)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETSELECTEDCOUNT, 0, 0);

					if (!selected || !app.ConfirmMessage (hwnd, nullptr, _r_fmt (app.LocaleString (IDS_QUESTION_DELETE, nullptr), selected), L"ConfirmDelete"))
						break;

					bool is_checked = false;
					const size_t count = _r_listview_getitemcount (hwnd, IDC_LISTVIEW) - 1;

					size_t item = LAST_VALUE;

					_r_fastlock_acquireexclusive (&lock_access);

					for (size_t i = count; i != LAST_VALUE; i--)
					{
						if (ListView_GetItemState (GetDlgItem (hwnd, IDC_LISTVIEW), i, LVNI_SELECTED))
						{
							const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, i);

							if (hash && (apps_undelete.find (hash) == apps_undelete.end ()))
							{
								SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_DELETEITEM, i, 0);

								if (_app_freeapplication (hash) && !is_checked)
									is_checked = true;

								item = i;
							}
						}
					}

					_r_fastlock_releaseexclusive (&lock_access);

					if (item != LAST_VALUE)
						ShowItem (hwnd, IDC_LISTVIEW, min (item, _r_listview_getitemcount (hwnd, IDC_LISTVIEW) - 1), -1);

					_app_profilesave (hwnd);

					if (is_checked)
						_app_installfilters (false);

					else
					{
						_r_listview_redraw (hwnd, IDC_LISTVIEW);
						_app_refreshstatus (hwnd, true, true);
					}

					break;
				}

				case IDM_PURGE_UNUSED:
				case IDM_PURGE_ERRORS:
				{
					bool is_deleted = false;
					bool is_changed = false;

					const size_t count = _r_listview_getitemcount (hwnd, IDC_LISTVIEW) - 1;

					_r_fastlock_acquireexclusive (&lock_access);

					for (size_t i = count; i != LAST_VALUE; i--)
					{
						const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, i);

						// skip "undeletable" apps
						if (apps_undelete.find (hash) != apps_undelete.end ())
							continue;

						PITEM_APP ptr_app = _app_getapplication (hash);

						if (ptr_app)
						{
							if (!_app_isexists (ptr_app) || (LOWORD (wparam) == IDM_PURGE_UNUSED && !ptr_app->is_enabled && !ptr_app->is_silent && !_app_apphaverule (hash)))
							{
								SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_DELETEITEM, i, 0);

								is_deleted = true;

								if (_app_freeapplication (hash) && !is_changed)
									is_changed = true;
							}
						}
					}

					_r_fastlock_releaseexclusive (&lock_access);

					if (is_deleted)
					{
						_app_profilesave (hwnd);
						_app_notifyrefresh ();

						if (is_changed)
							_app_installfilters (false);

						else
						{
							_r_listview_redraw (hwnd, IDC_LISTVIEW);
							_app_refreshstatus (hwnd, true, true);
						}
					}

					break;
				}

				case IDM_PURGE_TIMERS:
				{
					if (!_app_istimersactive () || !app.ConfirmMessage (hwnd, nullptr, app.LocaleString (IDS_QUESTION_TIMERS, nullptr), L"ConfirmTimers"))
						break;

					if (_app_timer_apply (hwnd, true))
					{
						_app_profilesave (hwnd);
						_app_installfilters (false);
					}

					break;
				}

				case IDM_SELECT_ALL:
				{
					ListView_SetItemState (GetDlgItem (hwnd, IDC_LISTVIEW), -1, LVIS_SELECTED, LVIS_SELECTED);
					break;
				}

				case IDM_ZOOM:
				{
					ShowWindow (hwnd, IsZoomed (hwnd) ? SW_RESTORE : SW_MAXIMIZE);
					break;
				}

				case 999:
				{
					notifications_last.erase (config.myhash);

					ITEM_LOG log = {0};

					log.hash = config.myhash;
					log.date = _r_unixtime_now ();

					log.af = AF_INET;
					log.protocol = IPPROTO_TCP;

					InetPton (log.af, L"195.210.46.14", &log.remote_addr);
					log.remote_port = 443;

					InetPton (log.af, L"192.168.2.2", &log.local_addr);
					log.local_port = 80;

					_app_formataddress (log.remote_fmt, _countof (log.remote_fmt), &log, FWP_DIRECTION_OUTBOUND, log.remote_port, true);
					_app_formataddress (log.local_fmt, _countof (log.local_fmt), &log, FWP_DIRECTION_INBOUND, log.local_port, true);

					StringCchCopy (log.filter_name, _countof (log.filter_name), L"<test filter>");

					_app_notifyadd (&log);

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
	MSG msg = {0};

	if (app.CreateMainWindow (IDD_MAIN, IDI_MAIN, &DlgProc, &initializer_callback))
	{
		const HACCEL haccel = LoadAccelerators (app.GetHINSTANCE (), MAKEINTRESOURCE (IDA_MAIN));

		if (haccel)
		{
			while (GetMessage (&msg, nullptr, 0, 0) > 0)
			{
				TranslateAccelerator (app.GetHWND (), haccel, &msg);

				if (!IsDialogMessage (app.GetHWND (), &msg))
				{
					TranslateMessage (&msg);
					DispatchMessage (&msg);
				}
			}

			DestroyAcceleratorTable (haccel);
		}
	}

	return (INT)msg.wParam;
}
