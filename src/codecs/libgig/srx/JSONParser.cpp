/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2026 Christian Schoenebeck                              *
 *                      <cuse@users.sourceforge.net>                       *
 *                                                                         *
 *   This library is part of libgig.                                       *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this library; if not, write to the Free Software           *
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston,                 *
 *   MA  02111-1307  USA                                                   *
 ***************************************************************************/

// This file implements a general purpose JSON Parser according to RFC 8259:
// https://www.rfc-editor.org/rfc/rfc8259

#include "JSONParser.h"

#include <string.h> // for memcpy()
#include <regex>
#include <functional>

namespace Serialization {

typedef std::function<String(const std::smatch& m)> ReplaceFn;

static String replaceAll(const String& haystack, const std::regex rx,
                         ReplaceFn fn)
{
    if (haystack.empty())
        return haystack;

    struct _Replace {
        size_t pos;
        size_t len;
        String by;
    };
    std::vector<_Replace> targets;
    {
        std::sregex_iterator it(haystack.begin(), haystack.end(), rx);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            const std::smatch& m = *it;
            const String by = fn(m);
            targets.push_back({
                (size_t) m.position(),
                (size_t) m.length(),
                by
            });
        }
    }
    String s = haystack;
    for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
        const _Replace& m = *it;
        s.replace(m.pos, m.len, m.by);
    }
    return s;
}

static String utf8StrFromCodePoint(unsigned long long cp) {
    String s;
    // See https://en.wikipedia.org/wiki/UTF-8#Description for details.
    if (cp <= 0x7f) { // 1 byte gross (7 bits net) ...
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x7ff) { // 2 bytes gross (11 bits net) ...
        s.push_back(static_cast<char>(
            0b11000000 | ((cp >> 6) & 0b00011111) // upper 5 bits
        ));
        s.push_back(static_cast<char>(
            0b10000000 | (cp & 0b00111111) // lower 6 bits
        ));
    } else if (cp < 0xffff) { // 3 bytes gross (16 bits net) ...
        s.push_back(static_cast<char>(
            0b11100000 | ((cp >> 12) & 0b00001111) // highest 4 bits
        ));
        s.push_back(static_cast<char>(
            0b10000000 | ((cp >> 6) & 0b00111111) // middle 6 bits
        ));
        s.push_back(static_cast<char>(
            0b10000000 | (cp & 0b00111111) // lowest 6 bits
        ));
    } else if (cp < 0x10ffff) { // 4 bytes gross (21 bits net) ...
        s.push_back(static_cast<char>(
            0b11110000 | ((cp >> 18) & 0b00000111) // highest 3 bits
        ));
        s.push_back(static_cast<char>(
            0b10000000 | ((cp >> 12) & 0b00111111) // higher 6 bits
        ));
        s.push_back(static_cast<char>(
            0b10000000 | ((cp >> 6) & 0b00111111) // lower 6 bits
        ));
        s.push_back(static_cast<char>(
            0b10000000 | (cp & 0b00111111) // lowest 6 bits
        ));
    } else {
        assert(false /* invalid UTF-8 code point*/);
    }
    return s;
}

// e.g. "\u005c"
static const std::regex rxUnicodeEscape(R"(\\u[0-9a-fA-F]{4})");

// e.g. "\\" or "\n"
static const std::regex rxDoubleEscape(R"(\\[\\ntrbf"/])");

enum Token_t {
    T_COMMENT,
    T_STR,
    T_NUMBER,
    T_BOOL,
    T_NULL,
    T_OBJ_BEGIN,
    T_OBJ_END,
    T_ARR_BEGIN,
    T_ARR_END,
    T_COMMA,
    T_COLON,
    T_EOF
};

struct Token {
    Token_t type;
    const char* p;
    const char* end;

    String str() const {
        if (p >= end) return "";
        String s;
        const size_t n = end - p;
        s.resize(n);
        memcpy(&s[0], p, n);
        return s;
    }

    Token stripQuotes() const {
        if (p+2 >= end)
            return { type, NULL, NULL };
        return { type, p+1, end-1 };
    }

    String unescape() const {
        String s = str();
        if (s.empty()) return s;
        s = replaceAll(s, rxUnicodeEscape, [](const std::smatch& m) -> String {
            const String hex = "0x" + m.str().substr(2);
            const unsigned long long utf8CodePoint = std::stoull(hex, NULL, 16);
            return utf8StrFromCodePoint(utf8CodePoint);
        });
        s = replaceAll(s, rxDoubleEscape, [](const std::smatch& m) -> String {
            const String type = m.str().substr(1);
            if (!type.empty()) {
                switch (type[0])  {
                    case '\\':  return "\\";
                    case 'n':   return "\n";
                    case 't':   return "\t";
                    case 'r':   return "\r";
                    case 'b':   return "\b";
                    case 'f':   return "\f";
                    case '"':   return "\"";
                    case '/':   return "/";
                }
            }
            return m.str(); // don't replace
        });
        return s;
    }

    bool boolean() const {
        return str() != "false";
    }
};

enum LexState_t {
    L_START,
    L_COMMENT_LINE,
    L_COMMENT_BLOCK,
    L_STR,
};

// e.g. "1234" or "59.2" or "-4.32e+34"
static const std::regex rxNumber(R"(^-?\d+(\.\d+)?([eE][+-]?\d*)?)");

// either "true" or "false"
static const std::regex rxBool(R"(^(true|false))");

// "null"
static const std::regex rxNull(R"(^null)");

static Token lex(const char*& p, const char* end, LexState_t state = L_START) {
    for (const char* start = p; p < end; ++p) {
        const char c = *p;
        const char c2 = (p+1 < end) ? *(p+1) : char(0);
        switch (state) {
            case L_START: {
                switch (c) {
                    case ' ':
                    case '\t':
                    case '\n':
                    case '\r':
                        continue;
                    case ',':
                        return { T_COMMA, p, ++p };
                    case ':':
                        return { T_COLON, p, ++p };
                    case '{':
                        return { T_OBJ_BEGIN, p, ++p };
                    case '}':
                        return { T_OBJ_END, p, ++p };
                    case '[':
                        return { T_ARR_BEGIN, p, ++p };
                    case ']':
                        return { T_ARR_END, p, ++p };
                    case '/':
                        if (c2 == '/') {
                            state = L_COMMENT_LINE;
                            start = p++;
                            continue;
                        }
                        if (c2 == '*') {
                            state = L_COMMENT_BLOCK;
                            start = p++;
                            continue;
                        }
                    case '"':
                        state = L_STR;
                        start = p;
                        continue;
                    case '-':
                    case '0':
                    case '1':
                    case '2':
                    case '3':
                    case '4':
                    case '5':
                    case '6':
                    case '7':
                    case '8':
                    case '9': {
                        std::cmatch m;
                        std::regex_search(p, end, m, rxNumber);
                        if (m.empty())
                            throw Exception("JSON lexer: syntax error");
                        else
                            return { T_NUMBER, p, p += m.length() };
                    }
                    default: {
                        std::cmatch m;
                        std::regex_search(p, end, m, rxBool);
                        if (!m.empty())
                            return { T_BOOL, p, p += m.length() };
                        std::regex_search(p, end, m, rxNull);
                        if (!m.empty())
                            return { T_NULL, p, p += m.length() };
                        throw Exception("JSON lexer: syntax error");
                    }
                }
            }
            case L_COMMENT_LINE: {
                switch (c) {
                    case '\n':
                        return { T_COMMENT, start, p++ };
                    default:
                        continue;
                }
            }
            case L_COMMENT_BLOCK: {
                switch (c) {
                    case '*':
                        if (c2 == '/')
                            return { T_COMMENT, start, p += 2 };
                    default:
                        continue;
                }
            }
            case L_STR: {
                switch (c) {
                    case '"':
                        return { T_STR, start, ++p };
                    case '\\':
                        if (c2 == '"' || c2 == '\\') {
                            ++p;
                            continue;
                        }
                    default:
                        continue;
                }
            }
        }
    }
    return { T_EOF, NULL, NULL };
}

static JSONArrayRef parseArr(const char*& p, const char* end);
static JSONObjectRef parseObj(const char*& p, const char* end);
static JSONNodeRef parseVal(const char*& p, const char* end, bool optional);

static JSONArrayRef parseArr(const char*& p, const char* end) {
    enum _PState_t {
        P_ARR_ELEMENT_BEGIN,
        P_ARR_ELEMENT_COMPLETE
    } state = P_ARR_ELEMENT_BEGIN;
    JSONArrayRef arr = JSONArrayRef(new JSONArray);
    while (true) {
        switch (state) {
            case P_ARR_ELEMENT_BEGIN: {
                JSONNodeRef value = parseVal(p, end, true);
                if (value)
                    arr->append(value);
                state = P_ARR_ELEMENT_COMPLETE;
                continue;
            }
            case P_ARR_ELEMENT_COMPLETE: {
                Token token = lex(p, end);
                switch (token.type) {
                    case T_COMMA:
                        state = P_ARR_ELEMENT_BEGIN;
                        continue;
                    case T_ARR_END:
                        return arr;
                    case T_COMMENT:
                        continue;
                    default:
                        throw Exception("JSON parser: syntax error");
                }
            }
        }
    }
}

static JSONObjectRef parseObj(const char*& p, const char* end) {
    enum _PState_t {
        P_OBJ_MEMBER_BEGIN,
        P_OBJ_MEMBER_NAME,
        P_OBJ_MEMBER_VALUE,
        P_OBJ_MEMBER_COMPLETE
    } state = P_OBJ_MEMBER_BEGIN;
    String name;
    JSONObjectRef obj = JSONObjectRef(new JSONObject);
    while (true) {
        switch (state) {
            case P_OBJ_MEMBER_BEGIN: {
                Token token = lex(p, end);
                switch (token.type) {
                    case T_STR:
                        name = token.stripQuotes().unescape();
                        state = P_OBJ_MEMBER_NAME;
                        continue;
                    case T_OBJ_END:
                        return obj;
                    case T_COMMENT:
                        continue;
                    default:
                        throw Exception("JSON parser: syntax error");
                }
            }
            case P_OBJ_MEMBER_NAME: {
                Token token = lex(p, end);
                switch (token.type) {
                    case T_COLON:
                        state = P_OBJ_MEMBER_VALUE;
                        continue;
                    case T_COMMENT:
                        continue;
                    default:
                        throw Exception("JSON parser: syntax error");
                }
            }
            case P_OBJ_MEMBER_VALUE: {
                JSONNodeRef value = parseVal(p, end, false);
                obj->add(name, value);
                state = P_OBJ_MEMBER_COMPLETE;
                continue;
            }
            case P_OBJ_MEMBER_COMPLETE: {
                Token token = lex(p, end);
                switch (token.type) {
                    case T_COMMA:
                        state = P_OBJ_MEMBER_BEGIN;
                        continue;
                    case T_OBJ_END:
                        return obj;
                    case T_COMMENT:
                        continue;
                    default:
                        throw Exception("JSON parser: syntax error");
                }
            }
        }
    }
}

static JSONNodeRef parseVal(const char*& p, const char* end, bool optional) {
    while (true) {
        const char* pPrev = p;
        Token token = lex(p, end);
        switch (token.type) {
            case T_STR:
                return JSONNodeRef(
                    new JSONString( token.stripQuotes().unescape() )
                );
            case T_NUMBER:
                return JSONNodeRef(
                    new JSONNumber( token.str() )
                );
            case T_BOOL:
                return JSONNodeRef(
                    new JSONBool( token.boolean() )
                );
            case T_NULL:
                return JSONNodeRef(
                    new JSONNull
                );
            case T_OBJ_BEGIN:
                return parseObj(p, end);
            case T_ARR_BEGIN:
                return parseArr(p, end);
            case T_COMMENT:
                continue;
            default:
                if (optional) {
                    // restore previous read position / token
                    p = pPrev;
                    return JSONNodeRef();
                }
                throw Exception("JSON parser: syntax error");
        }
    }
}

JSONNodeRef jsonParse(const char* p, const char* end) {
    return parseVal(p, end, false);
}

const JSONNumber* JSONNode::asNum() const {
    return dynamic_cast<const JSONNumber*>(this);
}

const JSONBool* JSONNode::asBool() const {
    return dynamic_cast<const JSONBool*>(this);
}

const JSONNull* JSONNode::asNull() const {
    return dynamic_cast<const JSONNull*>(this);
}

const JSONString* JSONNode::asStr() const {
    return dynamic_cast<const JSONString*>(this);
}

const JSONObject* JSONNode::asObj() const {
    return dynamic_cast<const JSONObject*>(this);
}

const JSONArray* JSONNode::asArr() const {
    return dynamic_cast<const JSONArray*>(this);
}

void JSONObject::add(String name, const JSONNodeRef& value) {
    m_members[name] = value;
}

JSONNodeRef JSONObject::member(String name) const {
    auto it = m_members.find(name);
    return (it != m_members.end()) ? it->second : JSONNodeRef();
}

JSONNumberRef JSONObject::memberNum(String name) const {
    return jsonNumberRef(member(name));
}

JSONBoolRef JSONObject::memberBool(String name) const {
    return jsonBoolRef(member(name));
}

JSONNullRef JSONObject::memberNull(String name) const {
    return jsonNullRef(member(name));
}

JSONStringRef JSONObject::memberStr(String name) const {
    return jsonStringRef(member(name));
}

JSONObjectRef JSONObject::memberObj(String name) const {
    return jsonObjectRef(member(name));
}

JSONArrayRef JSONObject::memberArr(String name) const {
    return jsonArrayRef(member(name));
}

void JSONArray::append(const JSONNodeRef& value) {
    m_elements.push_back(value);
}

JSONNodeRef JSONArray::element(size_t index) const {
    if (index >= m_elements.size())
        return JSONNodeRef();
    return m_elements[index];
}

JSONNumberRef JSONArray::elementNum(size_t index) const {
    return jsonNumberRef(element(index));
}

JSONBoolRef JSONArray::elementBool(size_t index) const {
    return jsonBoolRef(element(index));
}

JSONNullRef JSONArray::elementNull(size_t index) const {
    return jsonNullRef(element(index));
}

JSONStringRef JSONArray::elementStr(size_t index) const {
    return jsonStringRef(element(index));
}

JSONObjectRef JSONArray::elementObj(size_t index) const {
    return jsonObjectRef(element(index));
}

JSONArrayRef JSONArray::elementArr(size_t index) const {
    return jsonArrayRef(element(index));
}

} // namespace Serialization
