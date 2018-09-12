#pragma once

#include <streambuf>

#include <crtdbg.h>
#include <shellapi.h>
#include <tchar.h>

template <typename T>
class DebugStreamBuffer : public std::basic_streambuf<T, std::char_traits<T>>
{
	using Super = std::basic_streambuf<T, std::char_traits<T>>;
	using int_type = typename Super::int_type;
	using OutputDebugStringType = typename std::conditional<std::is_same<char, T>::value, TCHAR, WCHAR>::type;

public:
	virtual int_type overflow(int_type c = EOF)
	{
		if (c != EOF)
		{
			OutputDebugStringType buf[] = { static_cast<OutputDebugStringType>(c), '\0' };
			OutputDebugString(buf);
		}
		return c;
	}
};

static DebugStreamBuffer<char> g_debugStream;
static DebugStreamBuffer<wchar_t> g_wDebugStream;
static std::streambuf* g_defaultCoutStream = nullptr;
static std::streambuf* g_defaultCerrStream = nullptr;
static std::streambuf* g_defaultClogStream = nullptr;
static std::wstreambuf* g_defaultWCoutStream = nullptr;
static std::wstreambuf* g_defaultWCerrStream = nullptr;
static std::wstreambuf* g_defaultWClogStream = nullptr;
