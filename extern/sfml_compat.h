/*
 * Compatibility char_traits specializations for SFML 2.5.
 *
 * SFML uses std::basic_string<sf::Uint32> (unsigned int) and
 * std::basic_string<unsigned char>. Modern libc++ removed the
 * generic char_traits template (LWG2948). Provide minimal
 * specializations so SFML compiles.
 */

#ifndef SFML_COMPAT_H
#define SFML_COMPAT_H

#include <string>
#include <cstring>
#include <algorithm>

namespace std {

template<>
struct char_traits<unsigned int> {
    typedef unsigned int char_type;
    typedef unsigned int int_type;
    typedef streamoff off_type;
    typedef streampos pos_type;
    typedef mbstate_t state_type;

    static void assign(char_type& c1, const char_type& c2) noexcept { c1 = c2; }
    static bool eq(char_type c1, char_type c2) noexcept { return c1 == c2; }
    static bool lt(char_type c1, char_type c2) noexcept { return c1 < c2; }

    static int compare(const char_type* s1, const char_type* s2, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (lt(s1[i], s2[i])) return -1;
            if (lt(s2[i], s1[i])) return 1;
        }
        return 0;
    }
    static size_t length(const char_type* s) {
        size_t len = 0;
        while (!eq(s[len], char_type())) ++len;
        return len;
    }
    static const char_type* find(const char_type* s, size_t n, const char_type& a) {
        for (size_t i = 0; i < n; ++i)
            if (eq(s[i], a)) return s + i;
        return nullptr;
    }
    static char_type* move(char_type* s1, const char_type* s2, size_t n) {
        if (n == 0) return s1;
        return static_cast<char_type*>(memmove(s1, s2, n * sizeof(char_type)));
    }
    static char_type* copy(char_type* s1, const char_type* s2, size_t n) {
        if (n == 0) return s1;
        return static_cast<char_type*>(memcpy(s1, s2, n * sizeof(char_type)));
    }
    static char_type* assign(char_type* s, size_t n, char_type a) {
        for (size_t i = 0; i < n; ++i) s[i] = a;
        return s;
    }
    static int_type not_eof(int_type c) noexcept { return eq_int_type(c, eof()) ? ~eof() : c; }
    static char_type to_char_type(int_type c) noexcept { return char_type(c); }
    static int_type to_int_type(char_type c) noexcept { return int_type(c); }
    static bool eq_int_type(int_type c1, int_type c2) noexcept { return c1 == c2; }
    static int_type eof() noexcept { return int_type(0xFFFFFFFF); }
};

template<>
struct char_traits<unsigned char> {
    typedef unsigned char char_type;
    typedef unsigned int int_type;
    typedef streamoff off_type;
    typedef streampos pos_type;
    typedef mbstate_t state_type;

    static void assign(char_type& c1, const char_type& c2) noexcept { c1 = c2; }
    static bool eq(char_type c1, char_type c2) noexcept { return c1 == c2; }
    static bool lt(char_type c1, char_type c2) noexcept { return c1 < c2; }

    static int compare(const char_type* s1, const char_type* s2, size_t n) {
        return memcmp(s1, s2, n);
    }
    static size_t length(const char_type* s) {
        return strlen(reinterpret_cast<const char*>(s));
    }
    static const char_type* find(const char_type* s, size_t n, const char_type& a) {
        return static_cast<const char_type*>(memchr(s, a, n));
    }
    static char_type* move(char_type* s1, const char_type* s2, size_t n) {
        return static_cast<char_type*>(memmove(s1, s2, n));
    }
    static char_type* copy(char_type* s1, const char_type* s2, size_t n) {
        return static_cast<char_type*>(memcpy(s1, s2, n));
    }
    static char_type* assign(char_type* s, size_t n, char_type a) {
        return static_cast<char_type*>(memset(s, a, n));
    }
    static int_type not_eof(int_type c) noexcept { return eq_int_type(c, eof()) ? ~eof() : c; }
    static char_type to_char_type(int_type c) noexcept { return char_type(c); }
    static int_type to_int_type(char_type c) noexcept { return int_type(c); }
    static bool eq_int_type(int_type c1, int_type c2) noexcept { return c1 == c2; }
    static int_type eof() noexcept { return int_type(0xFF); }
};

template<>
struct char_traits<unsigned short> {
    typedef unsigned short char_type;
    typedef unsigned int int_type;
    typedef streamoff off_type;
    typedef streampos pos_type;
    typedef mbstate_t state_type;

    static void assign(char_type& c1, const char_type& c2) noexcept { c1 = c2; }
    static bool eq(char_type c1, char_type c2) noexcept { return c1 == c2; }
    static bool lt(char_type c1, char_type c2) noexcept { return c1 < c2; }

    static int compare(const char_type* s1, const char_type* s2, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (lt(s1[i], s2[i])) return -1;
            if (lt(s2[i], s1[i])) return 1;
        }
        return 0;
    }
    static size_t length(const char_type* s) {
        size_t len = 0;
        while (!eq(s[len], char_type())) ++len;
        return len;
    }
    static const char_type* find(const char_type* s, size_t n, const char_type& a) {
        for (size_t i = 0; i < n; ++i)
            if (eq(s[i], a)) return s + i;
        return nullptr;
    }
    static char_type* move(char_type* s1, const char_type* s2, size_t n) {
        if (n == 0) return s1;
        return static_cast<char_type*>(memmove(s1, s2, n * sizeof(char_type)));
    }
    static char_type* copy(char_type* s1, const char_type* s2, size_t n) {
        if (n == 0) return s1;
        return static_cast<char_type*>(memcpy(s1, s2, n * sizeof(char_type)));
    }
    static char_type* assign(char_type* s, size_t n, char_type a) {
        for (size_t i = 0; i < n; ++i) s[i] = a;
        return s;
    }
    static int_type not_eof(int_type c) noexcept { return eq_int_type(c, eof()) ? ~eof() : c; }
    static char_type to_char_type(int_type c) noexcept { return char_type(c); }
    static int_type to_int_type(char_type c) noexcept { return int_type(c); }
    static bool eq_int_type(int_type c1, int_type c2) noexcept { return c1 == c2; }
    static int_type eof() noexcept { return int_type(0xFFFF); }
};

} // namespace std

#endif /* SFML_COMPAT_H */
