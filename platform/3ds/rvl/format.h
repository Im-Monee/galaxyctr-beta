#pragma once

// printf-style formatting that follows Metrowerks MSL behaviour, shared by the narrow
// (printf.cpp) and wide (wchar.cpp) functions of the game world.
//
// Differences from newlib that the game relies on:
// - a null %s or %ls argument prints nothing (newlib prints "(null)");
// - wide strings use the game's 16-bit wchar_t.
//
// Each conversion is formatted with the C library's snprintf, so field widths, precision
// and flags behave as usual. The game's calls to sprintf and friends are redirected here
// with the linker's --wrap option (see printf.cpp); __real_snprintf is the library's own.

#include <stdarg.h>
#include <stddef.h>

extern "C" int __real_snprintf(char* pDst, size_t size, const char* pFormat, ...);

namespace rvl {

    // Writes characters into a buffer of `size` elements, always leaving room for the terminator.
    template < typename CharT >
    class FormatSink {
    public:
        FormatSink(CharT* pDst, size_t size) : mDst(pDst), mSize(size), mLength(0) {
        }

        void put(CharT c) {
            if (mLength + 1 < mSize) {
                mDst[mLength] = c;
            }
            mLength++;
        }

        // Returns the length the full output would have, like snprintf.
        size_t finish() {
            if (mSize > 0) {
                mDst[mLength < mSize ? mLength : mSize - 1] = 0;
            }
            return mLength;
        }

    private:
        CharT* mDst;
        size_t mSize;
        size_t mLength;
    };

    template < typename CharT >
    size_t formatString(CharT* pDst, size_t size, const CharT* pFormat, va_list args) {
        FormatSink< CharT > sink(pDst, size);

        while (*pFormat != 0) {
            if (*pFormat != '%') {
                sink.put(*pFormat++);
                continue;
            }

            // Collect the conversion as narrow text, resolving '*' widths and precisions.
            char spec[48];
            size_t len = 0;
            spec[len++] = '%';
            pFormat++;

            char conversion = 0;
            while (*pFormat != 0 && len < sizeof(spec) - 12) {
                char c = static_cast< char >(*pFormat++);
                if (c == '*') {
                    len += __real_snprintf(spec + len, sizeof(spec) - len, "%d", va_arg(args, int));
                    continue;
                }

                spec[len++] = c;
                if ((c >= 'a' && c <= 'z' && c != 'l' && c != 'h' && c != 'z' && c != 'j' && c != 't' && c != 'q') ||
                    (c >= 'A' && c <= 'Z' && c != 'L') || c == '%') {
                    conversion = c;
                    break;
                }
            }
            spec[len] = '\0';

            int longs = 0;
            for (size_t i = 1; i + 1 < len; i++) {
                longs += spec[i] == 'l';
            }

            if (conversion == '%' || conversion == 0) {
                sink.put('%');
                continue;
            }

            if (conversion == 's' && longs > 0) {
                // Wide string: copied as 16-bit characters.
                const wchar_t* str = va_arg(args, const wchar_t*);
                for (; str != nullptr && *str != 0; str++) {
                    sink.put(static_cast< CharT >(*str));
                }
                continue;
            }

            if (conversion == 'c' && longs > 0) {
                sink.put(static_cast< CharT >(va_arg(args, int)));
                continue;
            }

            if (conversion == 'n') {
                *va_arg(args, int*) = 0;
                continue;
            }

            char narrow[160];
            switch (conversion) {
            case 's': {
                const char* str = va_arg(args, const char*);
                __real_snprintf(narrow, sizeof(narrow), spec, str != nullptr ? str : "");
                break;
            }
            case 'c':
                __real_snprintf(narrow, sizeof(narrow), spec, va_arg(args, int));
                break;
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
                __real_snprintf(narrow, sizeof(narrow), spec, va_arg(args, double));
                break;
            case 'p':
                __real_snprintf(narrow, sizeof(narrow), spec, va_arg(args, void*));
                break;
            default:
                if (longs >= 2) {
                    __real_snprintf(narrow, sizeof(narrow), spec, va_arg(args, long long));
                } else if (longs == 1) {
                    __real_snprintf(narrow, sizeof(narrow), spec, va_arg(args, long));
                } else {
                    __real_snprintf(narrow, sizeof(narrow), spec, va_arg(args, int));
                }
                break;
            }

            for (const char* p = narrow; *p != '\0'; p++) {
                sink.put(static_cast< CharT >(static_cast< unsigned char >(*p)));
            }
        }

        return sink.finish();
    }

}  // namespace rvl
