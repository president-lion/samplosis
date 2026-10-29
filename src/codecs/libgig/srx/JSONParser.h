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

// C++ API of a general purpose JSON parser:

#ifndef LIBGIG_JSON_PARSER_H
#define LIBGIG_JSON_PARSER_H

#include "Serialization.h"
#include "helper.h"
#include <map>
#include <vector>
#include <memory>

namespace Serialization {

enum JSONType_t {
    JSON_NUMBER,
    JSON_BOOL,
    JSON_NULL,
    JSON_STRING,
    JSON_OBJECT,
    JSON_ARRAY
};

class JSONNumber;
class JSONBool;
class JSONNull;
class JSONString;
class JSONObject;
class JSONArray;

class JSONNode {
public:
    JSONNode(JSONType_t type) { m_type = type; }
    virtual ~JSONNode() {}
    JSONType_t type() const { return m_type; }
    const JSONNumber* asNum() const;
    const JSONBool* asBool() const;
    const JSONNull* asNull() const;
    const JSONString* asStr() const;
    const JSONObject* asObj() const;
    const JSONArray* asArr() const;
private:
    JSONType_t m_type;
};
typedef std::shared_ptr<JSONNode> JSONNodeRef;

class JSONNumber : public JSONNode {
    String m_str;
public:
    JSONNumber(String s) : JSONNode(JSON_NUMBER) { m_str = s; }

    template<class T_num>
    T_num value() const {
        return strTo<T_num>(m_str);
    }
};
typedef std::shared_ptr<JSONNumber> JSONNumberRef;

class JSONBool : public JSONNode {
    bool m_b;
public:
    JSONBool(bool b) : JSONNode(JSON_BOOL) { m_b = b; }
    bool value() const { return m_b; }
};
typedef std::shared_ptr<JSONBool> JSONBoolRef;

class JSONNull : public JSONNode {
public:
    JSONNull() : JSONNode(JSON_NULL) {}
};
typedef std::shared_ptr<JSONNull> JSONNullRef;

class JSONString : public JSONNode {
    String m_str;
public:
    JSONString(String s) : JSONNode(JSON_STRING) { m_str = s; }
    const String& value() const { return m_str; }
};
typedef std::shared_ptr<JSONString> JSONStringRef;

class JSONObject : public JSONNode {
    std::map<String,JSONNodeRef> m_members;
public:
    JSONObject() : JSONNode(JSON_OBJECT) {}
    void add(String name, const JSONNodeRef& value);
    JSONNodeRef member(String name) const;
    JSONNumberRef memberNum(String name) const;
    JSONBoolRef memberBool(String name) const;
    JSONNullRef memberNull(String name) const;
    JSONStringRef memberStr(String name) const;
    std::shared_ptr<JSONObject> memberObj(String name) const;
    std::shared_ptr<JSONArray> memberArr(String name) const;
};
typedef std::shared_ptr<JSONObject> JSONObjectRef;

class JSONArray : public JSONNode {
    std::vector<JSONNodeRef> m_elements;
public:
    JSONArray() : JSONNode(JSON_ARRAY) {}
    size_t count() const { return m_elements.size(); }
    void append(const JSONNodeRef& value);
    JSONNodeRef element(size_t index) const;
    JSONNumberRef elementNum(size_t index) const;
    JSONBoolRef elementBool(size_t index) const;
    JSONNullRef elementNull(size_t index) const;
    JSONStringRef elementStr(size_t index) const;
    JSONObjectRef elementObj(size_t index) const;
    std::shared_ptr<JSONArray> elementArr(size_t index) const;
};
typedef std::shared_ptr<JSONArray> JSONArrayRef;

/** @brief Parse the supplied JSON text and return structured tree.
 *
 * Parse the entire JSON document text supplied via arguments \a p and \a end
 * and return a structured tree presentation for conveniently walking JSON
 * document's tree and accessing its individual objects, arrays and
 * fundamental values.
 *
 * @param p - points to the start of the JSON document string to be parsed
 * @param end - points to the end of the JSON document string where parsing
 *              shall be stopped
 * @returns root element of supplied JSON document, which usually is a
 *          JSONObject, but may also be any other JSON type like a JSONArray,
 *          JSONString, JSONNumber, JSONBool or JSONNull.
 */
JSONNodeRef jsonParse(const char* p, const char* end);

inline JSONNumberRef jsonNumberRef(const JSONNodeRef& nodeRef) {
    return std::dynamic_pointer_cast<JSONNumber>(nodeRef);
}

inline JSONBoolRef jsonBoolRef(const JSONNodeRef& nodeRef) {
    return std::dynamic_pointer_cast<JSONBool>(nodeRef);
}

inline JSONNullRef jsonNullRef(const JSONNodeRef& nodeRef) {
    return std::dynamic_pointer_cast<JSONNull>(nodeRef);
}

inline JSONStringRef jsonStringRef(const JSONNodeRef& nodeRef) {
    return std::dynamic_pointer_cast<JSONString>(nodeRef);
}

inline JSONObjectRef jsonObjectRef(const JSONNodeRef& nodeRef) {
    return std::dynamic_pointer_cast<JSONObject>(nodeRef);
}

inline JSONArrayRef jsonArrayRef(const JSONNodeRef& nodeRef) {
    return std::dynamic_pointer_cast<JSONArray>(nodeRef);
}

} // namespace Serialization

#endif // LIBGIG_JSON_PARSER_H
