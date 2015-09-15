//this file is part of EditorConfig plugin for Notepad++
//
//Copyright (C)2003 Don HO <donho@altern.org>
//Copyright (C)2011 EditorConfig Team <http://editorconfig.org>
//
//This program is free software; you can redistribute it and/or
//modify it under the terms of the GNU General Public License
//as published by the Free Software Foundation; either
//version 2 of the License, or (at your option) any later version.
//
//This program is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//GNU General Public License for more details.
//
//You should have received a copy of the GNU General Public License
//along with this program; if not, write to the Free Software
//Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//

#include "PluginDefinition.hpp"
#include "menuCmdID.hpp"

#include "git2.h"
#include <functional>
#include <stack>
#include <string>

//
// The plugin data that Notepad++ needs
//
FuncItem funcItem[nbFunc];

//
// The data of Notepad++ that you can use in your plugin commands
//
NppData nppData;

//
// Initialize your plugin data here
// It will be called while plugin loading   
void pluginInit(HANDLE hModule)
{
	git_libgit2_init();
}

//
// Here you can do the clean up, save the parameters (if any) for the next session
//
void pluginCleanUp()
{
	git_libgit2_shutdown();
}

//
// Initialization of your plugin commands
// You should fill your plugins commands here
void commandMenuInit()
{

    //--------------------------------------------//
    //-- STEP 3. CUSTOMIZE YOUR PLUGIN COMMANDS --//
    //--------------------------------------------//
    // with function :
    // setCommand(int index,                      // zero based number to indicate the order of command
    //            TCHAR *commandName,             // the command name that you want to see in plugin menu
    //            PFUNCPLUGINCMD functionPointer, // the symbol of function (function pointer) associated with this command. The body should be defined below. See Step 4.
    //            ShortcutKey *shortcut,          // optional. Define a shortcut to trigger this command
    //            bool check0nInit                // optional. Make this menu item be checked visually
    //            );
	setCommand(0, TEXT("GitIgnore plugin"), []() {}, NULL, false);
}

//
// Here you can do the clean up (especially for the shortcut)
//
void commandMenuCleanUp()
{
    // Don't forget to deallocate your shortcut here
}


//
// This function help you to initialize your plugin commands
//
bool setCommand(size_t index, TCHAR *cmdName, PFUNCPLUGINCMD pFunc, ShortcutKey *sk, bool check0nInit) 
{
    if (index >= nbFunc)
        return false;

    if (!pFunc)
        return false;

    lstrcpy(funcItem[index]._itemName, cmdName);
    funcItem[index]._pFunc = pFunc;
    funcItem[index]._init2Check = check0nInit;
    funcItem[index]._pShKey = sk;

    return true;
}

class Utf16ToUtf8
{
	char smallBuf[MAX_PATH * 3 + 1];
	char *ptr;
public:
	Utf16ToUtf8() : ptr(smallBuf) { smallBuf[0] = '\0'; }
	~Utf16ToUtf8()
	{
		if (ptr != smallBuf)
			delete[] ptr;
	}
	char * convert(LPCWSTR input)
	{
		if (ptr != smallBuf)
		{
			delete[] ptr;
			ptr = smallBuf;
		}
		int wlen = wcslen(input);
		int len = WideCharToMultiByte(CP_UTF8, 0, input, wlen, nullptr, 0, nullptr, nullptr);
		if (len >= sizeof(smallBuf))
			ptr = new char[len + 1];
		WideCharToMultiByte(CP_UTF8, 0, input, wlen, ptr, len + 1, nullptr, nullptr);
		ptr[len] = '\0';
		return ptr;
	}
	operator char *() { return ptr; }
};

class RelativePath
{
	char smallBuf[MAX_PATH * 3 + 1];
	char *ptr;
public:
	RelativePath() : ptr(smallBuf) { smallBuf[0] = '\0'; }
	~RelativePath()
	{
		if (ptr != smallBuf)
			delete[] ptr;
	}
	char * make(const char *baseDir, const char *fullPath)
	{
		if (strstr(fullPath, baseDir) == fullPath)
			strcpy(ptr, fullPath + strlen(baseDir));
		return ptr;
	}
	static const char * comparePath(const char *a, const char *b, int &behind, int &ahead)
	{
		behind = ahead = 0;
		const char *c = a;
		const char *d = b;
		while (true)
		{
			if ((!*a || *a == '/') && (!*b || *b == '/'))
			{
				c = a + (*a ? 1 : 0);
				d = b + (*b ? 1 : 0);
			}
			if (*a != *b)
				break;
			if (!*a || !*b)
				break;
			a++;
			b++;
		}

		b = d;
		if (*b)
		{
			for (; *b; b++)
				if (*b == '/')
					ahead++;
			if (*(b - 1) != '/')
				ahead++;
		}

		a = c;
		if (*a)
		{
			for (; *a; a++)
				if (*a == '/')
					behind++;
			if (*(a - 1) != '/')
				behind++;
		}
		return d;
	}
	operator char *() { return ptr; }
};

class AutoVar
{
	std::function<void()> fn;
public:
	AutoVar(std::function<void()> func) : fn(func) { }
	~AutoVar()
	{
		if (fn != nullptr)
			fn();
	}
};

void toForwardSlash(char *buf)
{
	while (*buf)
	{
		if (*buf == '\\')
			*buf = '/';
		buf++;
	}
}

std::stack<std::pair<std::string, git_repository *> > repoCache;

void clearRepoCache(int count = INT_MAX)
{
	int c = 0;
	git_repository *repo = nullptr;
	while (!repoCache.empty())
	{
		auto item = repoCache.top();
		if (item.second != repo)
			git_repository_free(repo);
		if (c >= count)
			return;
		repo = item.second;
		repoCache.pop();
		c++;
	}
	git_repository_free(repo);
}

void onFindFilesStarted(SCNotification *notifyCode)
{
	clearRepoCache();
}

void onFindFilesFile(SCNotification *notifyCode)
{
	auto input = (std::pair<const TCHAR *, const TCHAR *>*)notifyCode->nmhdr.hwndFrom;
	auto result = (bool*)notifyCode->nmhdr.idFrom;

	Utf16ToUtf8 discover;
	discover.convert(input->first);
	toForwardSlash(discover);
	bool found = false;
	git_repository *repo = nullptr;
	int behind = 0, ahead = 0;
	int r;
	const char *oldDir = "";
	if (!repoCache.empty())
		oldDir = repoCache.top().first.c_str();
	const char *extra = RelativePath::comparePath(oldDir, discover, behind, ahead);
	if (behind > 0)
	{
		// go to parent
		clearRepoCache(behind);
		if (!repoCache.empty())
		{
			found = true;
			repo = repoCache.top().second;
		}
	}
	while (ahead > 0)
	{
		git_buf repoPath = { 0 };
		if ((r = git_repository_discover(&repoPath, discover, 1, nullptr)) >= 0)
			if ((r = git_repository_open(&repo, repoPath.ptr)) >= 0)
			{
				
			}
		repoCache.push(std::make_pair((char *)discover, repo));
		ahead--;
	}

	if (!repo)
		return;

	//AutoVar var2([&]() { git_repository_free(repo); });

	const char *workdir = git_repository_workdir(repo);
	if (!workdir)
		return;
	Utf16ToUtf8 file;
	RelativePath relative;
	relative.make(workdir, discover);
	strcat(relative, file.convert(input->second));

	int ignored = 0;
	if ((r = git_ignore_path_is_ignored(&ignored, repo, relative)) < 0)
		return;
	if (ignored == 1)
		*result = false;
}

void onFindFilesEnded(SCNotification *notifyCode)
{
	clearRepoCache();
}