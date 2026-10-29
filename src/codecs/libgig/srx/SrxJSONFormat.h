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

#ifndef SRX_JSON_FORMAT_H
#define SRX_JSON_FORMAT_H

#include "Serialization.h"
#include "JSONParser.h"
#include <stack>

namespace Serialization {

/** @brief Base class of "Srx-JSON" file format encoder / decoder.
 *
 * This is the 2nd file format introduced by this library for encoding /
 * decoding serialized data streams. It uses standard JSON format, which
 * provides the benefit of being user-friendly to read and to edit in encoded
 * form. Downside is that it's a somewhat more complicated and potentially
 * ambiguous file format, which for instance requires escaping / unescaping of
 * special characters and dedicated Unicode (Utf-8 that is) handling. The hereby
 * implemented encoder and parser are therefore also a bit more complex, bigger
 * and slower.
 */
class SrxJSONFormat {
public:
    static String name();
    static bool checkMagicStart(const RawData& data);
protected:
    static bool _strToBool(String value);
    static String _hexStr(void* ptr);
};

/** @brief Encodes serialized data streams in "Srx-JSON" file format.
 */
class SrxJSONEncoder : public SrxJSONFormat {
    struct Context {
        size_t depth;
        size_t index;
    };
    std::stack<Context> m_ctx;
public:
    SrxJSONEncoder();
    RawData encode(const Archive& arch);
private:
    Context& ctx();
    const Context& ctx() const;
    void clearCtx();
    void pushCtx();
    void popCtx();
    String _tab() const;
    static String _escapeStr(String data);
    static String _encodeStr(String data);
    String _encodeKeyValueStr(String key, String value);
    String _encodeKeyValueStrs(String key, std::vector<String> values, bool newLines);
    String _encodeKeyValueNum(String key, String value);
    template<class T_num>
    String _encodeKeyValueNum(String key, T_num value);
    String _encodeKeyValueBool(String key, String value);
    String _encodeKeyValueBool(String key, bool value);
    String _encodeBlockBegin(String key, char startChar, bool newLine = true);
    String _encodeBlockBegin(char startChar);
    String _encodeBlockEnd(char endChar, bool newLine = true);
    String _encode(String key, const UID& uid);
    String _encode(const UID& uid);
    String _encode(String key, const time_t& time);
    String _encode(String key, const DataType& type);
    String _encode(String key, const UIDChain& chain);
    String _encode(const Member& member);
    String _encode(const std::vector<Member>& members);
    String _encode(const Object& obj);
    String _encode(String key, const Archive::ObjectPool& objects);
    String _encodeRoot(const Archive& arch);
};

/** @brief Decodes serialized data streams being in "Srx-JSON" file format.
 */
class SrxJSONDecoder : public SrxJSONFormat {
public:
    SrxJSONDecoder();
    static void decode(Archive& arch, const RawData& data);
private:
    static void _decodeRoot(Archive& arch, JSONObjectRef jsonRootObj);
    static void _decodeAllObjects(Archive& arch, JSONArrayRef all_objects);
    static Object _decodeObject(JSONObjectRef jsonObject);
    static std::vector<Member> _decodeMembers(JSONArrayRef jsonArr);
    static Member _decodeMember(JSONObjectRef jsonObj);
    static void _decodePrimitiveObjectValue(Object& obj, JSONNodeRef json);
    static DataType _decodeType(JSONObjectRef jsonTypeObj);
    static UIDChain _decodeUIDChain(JSONArrayRef jsonArr);
    static UID _decodeUID(JSONObjectRef jsonUIDObj);
};

} // namespace Serialization

#endif // SRX_JSON_FORMAT_H
