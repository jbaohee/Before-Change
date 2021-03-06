// Copyright (c) 2006-2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sandbox/src/win_utils.h"

#include <map>

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "sandbox/src/internal_types.h"
#include "sandbox/src/nt_internals.h"

namespace {

// Holds the information about a known registry key.
struct KnownReservedKey {
  const wchar_t* name;
  HKEY key;
};

// Contains all the known registry key by name and by handle.
const KnownReservedKey kKnownKey[] = {
    { L"HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT },
    { L"HKEY_CURRENT_USER", HKEY_CURRENT_USER },
    { L"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE},
    { L"HKEY_USERS", HKEY_USERS},
    { L"HKEY_PERFORMANCE_DATA", HKEY_PERFORMANCE_DATA},
    { L"HKEY_PERFORMANCE_TEXT", HKEY_PERFORMANCE_TEXT},
    { L"HKEY_PERFORMANCE_NLSTEXT", HKEY_PERFORMANCE_NLSTEXT},
    { L"HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG},
    { L"HKEY_DYN_DATA", HKEY_DYN_DATA}
};

// Returns true if the provided path points to a pipe.
bool IsPipe(const std::wstring& path) {
  size_t start = 0;
  if (0 == path.compare(0, sandbox::kNTPrefixLen, sandbox::kNTPrefix))
    start = sandbox::kNTPrefixLen;

  const wchar_t kPipe[] = L"pipe\\";
  return (0 == path.compare(start, arraysize(kPipe) - 1, kPipe));
}

}  // namespace

namespace sandbox {

HKEY GetReservedKeyFromName(const std::wstring& name) {
  for (size_t i = 0; i < arraysize(kKnownKey); ++i) {
    if (name == kKnownKey[i].name)
      return kKnownKey[i].key;
  }

  return NULL;
}

bool ResolveRegistryName(std::wstring name, std::wstring* resolved_name) {
  for (size_t i = 0; i < arraysize(kKnownKey); ++i) {
    if (name.find(kKnownKey[i].name) == 0) {
      HKEY key;
      DWORD disposition;
      if (ERROR_SUCCESS != ::RegCreateKeyEx(kKnownKey[i].key, L"", 0, NULL, 0,
                                            MAXIMUM_ALLOWED, NULL, &key,
                                            &disposition))
        return false;

      bool result = GetPathFromHandle(key, resolved_name);
      ::RegCloseKey(key);

      if (!result)
        return false;

      *resolved_name += name.substr(wcslen(kKnownKey[i].name));
      return true;
    }
  }

  return false;
}

DWORD IsReparsePoint(const std::wstring& full_path, bool* result) {
  std::wstring path = full_path;

  // Remove the nt prefix.
  if (0 == path.compare(0, kNTPrefixLen, kNTPrefix))
    path = path.substr(kNTPrefixLen);

  // Check if it's a pipe. We can't query the attributes of a pipe.
  if (IsPipe(path)) {
    *result = FALSE;
    return ERROR_SUCCESS;
  }

  std::wstring::size_type last_pos = std::wstring::npos;

  do {
    path = path.substr(0, last_pos);

    DWORD attributes = ::GetFileAttributes(path.c_str());
    if (INVALID_FILE_ATTRIBUTES == attributes) {
      DWORD error = ::GetLastError();
      if (error != ERROR_FILE_NOT_FOUND &&
          error != ERROR_PATH_NOT_FOUND &&
          error != ERROR_INVALID_NAME) {
        // Unexpected error.
        NOTREACHED();
        return error;
      }
    } else if (FILE_ATTRIBUTE_REPARSE_POINT & attributes) {
      // This is a reparse point.
      *result = true;
      return ERROR_SUCCESS;
    }

    last_pos = path.rfind(L'\\');
  } while (last_pos != std::wstring::npos);

  *result = false;
  return ERROR_SUCCESS;
}

// We get a |full_path| of the form \??\c:\some\foo\bar, and the name that
// we'll get from |handle| will be \device\harddiskvolume1\some\foo\bar.
bool SameObject(HANDLE handle, const wchar_t* full_path) {
  std::wstring path(full_path);
  DCHECK(!path.empty());

  // Check if it's a pipe.
  if (IsPipe(path))
    return true;

  std::wstring actual_path;
  if (!GetPathFromHandle(handle, &actual_path))
    return false;

  // This may end with a backslash.
  const wchar_t kBackslash = '\\';
  if (path[path.length() - 1] == kBackslash)
    path = path.substr(0, path.length() - 1);

  if (0 == actual_path.compare(full_path))
    return true;

  // Look for the drive letter.
  size_t colon_pos = path.find(L':');
  if (colon_pos == 0 || colon_pos == std::wstring::npos)
    return false;

  // Only one character for the drive.
  if (colon_pos > 1 && path[colon_pos - 2] != kBackslash)
    return false;

  // We only need 3 chars, but let's alloc a buffer for four.
  wchar_t drive[4] = {0};
  wchar_t vol_name[MAX_PATH];
  memcpy(drive, &path[colon_pos - 1], 2 * sizeof(*drive));

  // We'll get a double null terminated string.
  DWORD vol_length = ::QueryDosDeviceW(drive, vol_name, MAX_PATH);
  if (vol_length < 2 || vol_length == MAX_PATH)
    return false;

  // Ignore the nulls at the end.
  vol_length -= 2;

  // The two paths should be the same length.
  if (vol_length + path.size() - (colon_pos + 1) != actual_path.size())
    return false;

  // Check up to the drive letter.
  if (0 != actual_path.compare(0, vol_length, vol_name))
    return false;

  // Check the path after the drive letter.
  if (0 != actual_path.compare(vol_length, std::wstring::npos,
                               &path[colon_pos + 1]))
    return false;

  return true;
}

bool ConvertToLongPath(const std::wstring& short_path,
                       std::wstring* long_path) {
  // Check if the path is a NT path.
  bool is_nt_path = false;
  std::wstring path = short_path;
  if (0 == path.compare(0, kNTPrefixLen, kNTPrefix)) {
    path = path.substr(kNTPrefixLen);
    is_nt_path = true;
  }

  DWORD size = MAX_PATH;
  scoped_array<wchar_t> long_path_buf(new wchar_t[size]);

  DWORD return_value = ::GetLongPathName(path.c_str(), long_path_buf.get(),
                                         size);
  while (return_value >= size) {
    size *= 2;
    long_path_buf.reset(new wchar_t[size]);
    return_value = ::GetLongPathName(path.c_str(), long_path_buf.get(), size);
  }

  DWORD last_error = ::GetLastError();
  if (0 == return_value && (ERROR_FILE_NOT_FOUND == last_error ||
                            ERROR_PATH_NOT_FOUND == last_error ||
                            ERROR_INVALID_NAME == last_error)) {
    // The file does not exist, but maybe a sub path needs to be expanded.
    std::wstring::size_type last_slash = path.rfind(L'\\');
    if (std::wstring::npos == last_slash)
      return false;

    std::wstring begin = path.substr(0, last_slash);
    std::wstring end = path.substr(last_slash);
    if (!ConvertToLongPath(begin, &begin))
      return false;

    // Ok, it worked. Let's reset the return value.
    path = begin + end;
    return_value = 1;
  } else if (0 != return_value) {
    path = long_path_buf.get();
  }

  if (return_value != 0) {
    if (is_nt_path) {
      *long_path = kNTPrefix;
      *long_path += path;
    } else {
      *long_path = path;
    }

    return true;
  }

  return false;
}

bool GetPathFromHandle(HANDLE handle, std::wstring* path) {
  NtQueryObjectFunction NtQueryObject = NULL;
  ResolveNTFunctionPtr("NtQueryObject", &NtQueryObject);

  OBJECT_NAME_INFORMATION initial_buffer;
  OBJECT_NAME_INFORMATION* name = &initial_buffer;
  ULONG size = sizeof(initial_buffer);
  // Query the name information a first time to get the size of the name.
  NTSTATUS status = NtQueryObject(handle, ObjectNameInformation, name, size,
                                  &size);

  scoped_ptr<OBJECT_NAME_INFORMATION> name_ptr;
  if (size) {
    name = reinterpret_cast<OBJECT_NAME_INFORMATION*>(new BYTE[size]);
    name_ptr.reset(name);

    // Query the name information a second time to get the name of the
    // object referenced by the handle.
    status = NtQueryObject(handle, ObjectNameInformation, name, size, &size);
  }

  if (STATUS_SUCCESS != status)
    return false;

  path->assign(name->ObjectName.Buffer, name->ObjectName.Length /
                                        sizeof(name->ObjectName.Buffer[0]));
  return true;
}

};  // namespace sandbox

// TODO(cpu): This is not the final code we want here but we are yet
// to understand what is going on. See bug 11789.
void ResolveNTFunctionPtr(const char* name, void* ptr) {
  HMODULE ntdll = ::GetModuleHandle(sandbox::kNtdllName);
  FARPROC* function_ptr = reinterpret_cast<FARPROC*>(ptr);
  *function_ptr = ::GetProcAddress(ntdll, name);
  if (*function_ptr)
    return;
  // We have data that re-trying helps.
  *function_ptr = ::GetProcAddress(ntdll, name);
  CHECK(*function_ptr);
}
