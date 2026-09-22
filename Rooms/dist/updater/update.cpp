// Peppy Dolphin - the updater a tester double-clicks.
//
// ⚠️ THIS IS A PROGRAM, NOT A SCRIPT, AND THAT IS THE POINT. The first beta
// shipped with a .bat and a .ps1 and they were pulled, because a batch file
// copying things around somebody's computer reads as suspicious - fairly so.
// This does the same job as an ordinary exe sitting beside the one they already
// run, and it says out loud what it is about to do before doing it.
//
// ⚠️ It is a SEPARATE process from Dolphin on purpose. A running Windows
// program cannot overwrite its own .exe, so an in-app updater can only ever
// refresh the data files. This can replace everything, including
// "Slippi Dolphin.exe" - as long as Dolphin is closed, which it checks.
//
// No dependencies beyond Windows itself: WinHTTP for the download, CryptoAPI
// for the hashes. Nothing to install, nothing to trust but the URLs below.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>

#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")

// ⚠️ The ONLY place this program will fetch from. A manifest on the project's
// own branch, over HTTPS, naming the files and their hashes. Everything
// downloaded is checked against a hash that came from here, so a truncated or
// tampered download is discarded rather than installed.
static const wchar_t *MANIFEST_URL =
    L"https://raw.githubusercontent.com/attunegames/peppy-dolphin/rooms-wip/Rooms/dist/latest.txt";

// ---------------------------------------------------------------- utilities

static void Say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static std::wstring Widen(const std::string &s)
{
	if (s.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring out(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
	return out;
}

static std::string Trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

// The first eight characters of the MD5, which is what VERSION.txt prints and
// what a tester reads back to us. Same shortening, so the two can be compared
// by eye.
static std::string Md5Short(const std::string &path)
{
	HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return "";

	HCRYPTPROV prov = 0;
	HCRYPTHASH hash = 0;
	std::string out;

	if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) &&
	    CryptCreateHash(prov, CALG_MD5, 0, 0, &hash))
	{
		std::vector<BYTE> buf(64 * 1024);
		DWORD got = 0;
		while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got > 0)
			CryptHashData(hash, buf.data(), got, 0);

		BYTE digest[16];
		DWORD len = sizeof(digest);
		if (CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0))
		{
			char hex[33];
			for (int i = 0; i < 16; i++)
				sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
			out.assign(hex, 8);
		}
	}

	if (hash)
		CryptDestroyHash(hash);
	if (prov)
		CryptReleaseContext(prov, 0);
	CloseHandle(f);
	return out;
}

// One HTTPS GET, following redirects - which matters, because a GitHub release
// asset always redirects to its storage host.
static bool Fetch(const std::wstring &url, std::string &body)
{
	body.clear();

	URL_COMPONENTS uc = {};
	uc.dwStructSize = sizeof(uc);
	wchar_t host[256] = {0}, path[2048] = {0};
	uc.lpszHostName = host;
	uc.dwHostNameLength = _countof(host);
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = _countof(path);
	if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc))
		return false;

	HINTERNET session = WinHttpOpen(L"PeppyUpdater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
	                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
		return false;

	bool ok = false;
	HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
	if (conn)
	{
		DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
		HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
		                                   WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
		if (req)
		{
			if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
			                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
			    WinHttpReceiveResponse(req, nullptr))
			{
				DWORD status = 0, len = sizeof(status);
				WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
				                    WINHTTP_NO_HEADER_INDEX);
				if (status == 200)
				{
					DWORD avail = 0;
					do
					{
						avail = 0;
						if (!WinHttpQueryDataAvailable(req, &avail))
							break;
						if (!avail)
							break;
						std::vector<char> chunk(avail);
						DWORD read = 0;
						if (!WinHttpReadData(req, chunk.data(), avail, &read))
							break;
						body.append(chunk.data(), read);
					} while (avail > 0);
					ok = true;
				}
				else
				{
					Say("    the server answered %lu", status);
				}
			}
			WinHttpCloseHandle(req);
		}
		WinHttpCloseHandle(conn);
	}
	WinHttpCloseHandle(session);
	return ok;
}

// ⚠️ Written to a temp file and only moved into place once the hash matches.
// A half-finished download must never become the file the game loads.
static bool WriteAtomically(const std::string &dest, const std::string &data,
                            const std::string &want_hash)
{
	std::string tmp = dest + ".part";

	// Make sure the folder exists - Sys\GameFiles\GALE01 and friends.
	std::string dir = dest.substr(0, dest.find_last_of("\\/"));
	if (!dir.empty() && dir != dest)
		CreateDirectoryA(dir.c_str(), nullptr);

	HANDLE f = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
	{
		Say("    could not write %s", tmp.c_str());
		return false;
	}
	DWORD wrote = 0;
	bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &wrote, nullptr) &&
	          wrote == data.size();
	CloseHandle(f);
	if (!ok)
	{
		DeleteFileA(tmp.c_str());
		Say("    the write did not complete");
		return false;
	}

	std::string got = Md5Short(tmp);
	if (got != want_hash)
	{
		DeleteFileA(tmp.c_str());
		Say("    REFUSED: downloaded %s but the manifest says %s - nothing replaced",
		    got.empty() ? "nothing" : got.c_str(), want_hash.c_str());
		return false;
	}

	if (!MoveFileExA(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING))
	{
		DeleteFileA(tmp.c_str());
		Say("    could not replace %s (is Dolphin still open?)", dest.c_str());
		return false;
	}
	return true;
}

// Dolphin holds its own exe open, so an update while it is running would half
// apply - new data files beside the old program.
static bool DolphinIsRunning()
{
	HANDLE f = CreateFileA("Slippi Dolphin.exe", GENERIC_WRITE, 0, nullptr,
	                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_SHARING_VIOLATION;
	CloseHandle(f);
	return false;
}

static void Pause()
{
	printf("\nPress Enter to close.");
	fflush(stdout);
	getchar();
}

int main()
{
	SetConsoleTitleA("Peppy Dolphin - update");
	Say("Peppy Dolphin updater");
	Say("");
	Say("This checks one address for a newer beta and replaces up to three files");
	Say("in this folder. It touches nothing else on your computer.");
	Say("");

	if (GetFileAttributesA("Slippi Dolphin.exe") == INVALID_FILE_ATTRIBUTES)
	{
		Say("This needs to sit in the same folder as \"Slippi Dolphin.exe\".");
		Say("Move it there and run it again.");
		Pause();
		return 1;
	}

	if (DolphinIsRunning())
	{
		Say("Dolphin is open. Close it first, then run this again.");
		Pause();
		return 1;
	}

	Say("Checking for a newer build...");
	std::string manifest;
	if (!Fetch(MANIFEST_URL, manifest))
	{
		Say("Could not reach the update address. Nothing has been changed.");
		Pause();
		return 1;
	}

	// One file per line:  path|md5-8|url        (# starts a comment)
	// The path uses backslashes and may contain spaces, hence the pipes.
	std::string version;
	int changed = 0, failed = 0, already = 0;

	size_t pos = 0;
	while (pos <= manifest.size())
	{
		size_t nl = manifest.find('\n', pos);
		if (nl == std::string::npos)
			nl = manifest.size();
		std::string line = Trim(manifest.substr(pos, nl - pos));
		pos = nl + 1;

		if (line.empty() || line[0] == '#')
			continue;

		if (line.rfind("version ", 0) == 0)
		{
			version = Trim(line.substr(8));
			Say("Latest is %s", version.c_str());
			Say("");
			continue;
		}
		if (line.rfind("file ", 0) != 0)
			continue;

		std::string rest = line.substr(5);
		size_t b1 = rest.find('|');
		size_t b2 = rest.find('|', b1 == std::string::npos ? b1 : b1 + 1);
		if (b1 == std::string::npos || b2 == std::string::npos)
			continue;

		std::string path = Trim(rest.substr(0, b1));
		std::string want = Trim(rest.substr(b1 + 1, b2 - b1 - 1));
		std::string url = Trim(rest.substr(b2 + 1));

		std::string have = Md5Short(path);
		if (have == want)
		{
			Say("  up to date  %s  (%s)", path.c_str(), want.c_str());
			already++;
			continue;
		}

		Say("  updating    %s  (%s -> %s)", path.c_str(),
		    have.empty() ? "missing" : have.c_str(), want.c_str());

		std::string data;
		if (!Fetch(Widen(url), data) || data.empty())
		{
			Say("    the download failed - nothing replaced");
			failed++;
			continue;
		}
		if (WriteAtomically(path, data, want))
			changed++;
		else
			failed++;
	}

	Say("");
	if (failed > 0)
		Say("%d file(s) updated, %d FAILED. Your folder still works - run this again,",
		    changed, failed);
	else if (changed > 0)
		Say("Done - %d file(s) updated. You are on %s.", changed, version.c_str());
	else if (already > 0)
		Say("Already up to date.");
	else
		Say("The manifest listed nothing to check. Nothing has been changed.");

	if (failed > 0)
		Say("or download the zip from the releases page if it keeps failing.");

	Pause();
	return failed > 0 ? 1 : 0;
}
