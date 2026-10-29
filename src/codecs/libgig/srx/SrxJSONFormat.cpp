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

#include "SrxJSONFormat.h"

#include <string.h> // for memcpy()
#include <sstream>
#include <iomanip>
#include <locale>
#include "helper.h"

namespace Serialization {

    /*
     * Srx-JSON format history:
     * - 1.2: Initial version (for real).
     */
    #define SRX_JSON_MARKER     "Srx-JSON-v1"
    #define MAGIC_START         "// " SRX_JSON_MARKER "\n"
    #define MAGIC_START2        "// " SRX_JSON_MARKER "\r\n"
    #define ENCODING_FORMAT_MAJOR_VERSION 1
    #define ENCODING_FORMAT_MINOR_VERSION 2

    // *************** SrxJSONFormat ***************
    // *

    String SrxJSONFormat::name() {
        return SRX_JSON_MARKER;
    }

    bool SrxJSONFormat::checkMagicStart(const RawData& data) {
        if (data.empty()) return false;
        const char* p = (const char*) &data[0];
        const size_t sz = data.size();
        if (memcmp(p, MAGIC_START, std::min(strlen(MAGIC_START), sz)) &&
            memcmp(p, MAGIC_START2, std::min(strlen(MAGIC_START2), sz)))
        {
            return false;
        }
        return true;
    }

    bool SrxJSONFormat::_strToBool(String value) {
        value = toLowerCase(value);
        if (value == "true") return true;
        if (value == "false") return false;
        if (value == "yes") return true;
        if (value == "no") return false;
        if (value == "1") return true;
        if (value == "0") return false;
        if (value == "t") return true;
        if (value == "f") return false;
        if (value == "y") return true;
        if (value == "n") return false;
        assert(false /* unknown bool string */);
    }

    String SrxJSONFormat::_hexStr(void* ptr) {
        std::ostringstream oss;
        oss.imbue(std::locale::classic()); // a.k.a. "C" locale
        oss << std::hex
            << ptr;
        return oss.str();
    }

    // *************** SrxJSONEncoder ***************
    // *

    SrxJSONEncoder::SrxJSONEncoder() {
    }

    const SrxJSONEncoder::Context& SrxJSONEncoder::ctx() const {
        assert(!m_ctx.empty());
        return m_ctx.top();
    }

    SrxJSONEncoder::Context& SrxJSONEncoder::ctx() {
        assert(!m_ctx.empty());
        return m_ctx.top();
    }

    void SrxJSONEncoder::clearCtx() {
        m_ctx = std::stack<Context>();
        m_ctx.push({ 0, 0 });
    }

    void SrxJSONEncoder::pushCtx() {
        assert(!m_ctx.empty());
        const size_t depth = m_ctx.top().depth;
        m_ctx.push({ depth+1, 0 });
    }

    void SrxJSONEncoder::popCtx() {
        assert(!m_ctx.empty());
        m_ctx.pop();
    }

    String SrxJSONEncoder::_tab() const {
        String s;
        const size_t depth = ctx().depth;
        s.reserve(depth);
        for (size_t i = 0; i < depth; ++i)
            s += '\t';
        return s;
    }

    String SrxJSONEncoder::_escapeStr(String data) {
        for (size_t i = 0; i < data.length(); ++i) {
            const unsigned char c = data[i];
            switch (c) {
                case '"':
                    data.replace(i++, 1, R"(\")");
                    continue;
                case '\\':
                    data.replace(i++, 1, R"(\\)");
                    continue;
                case '\b':
                    data.replace(i++, 1, R"(\b)");
                    continue;
                case '\f':
                    data.replace(i++, 1, R"(\f)");
                    continue;
                case '\n':
                    data.replace(i++, 1, R"(\n)");
                    continue;
                case '\r':
                    data.replace(i++, 1, R"(\r)");
                    continue;
                case '\t':
                    data.replace(i++, 1, R"(\t)");
                    continue;
                default:
                    if (c <= 0x1f) {
                        std::ostringstream oss;
                        oss.imbue(std::locale::classic()); // a.k.a. "C" locale
                        oss << "\\u"
                            << std::hex
                            << std::uppercase
                            << std::setfill('0')
                            << std::setw(4)
                            << static_cast<unsigned int>(c);
                        data.replace(i, 1, oss.str());
                        i += oss.str().length() - 1;
                    }
            }
        }
        return data;
    }

    String SrxJSONEncoder::_encodeStr(String data) {
        return "\"" + _escapeStr(data) + "\"";
    }

    String SrxJSONEncoder::_encodeKeyValueStr(String key, String value) {
        String s;
        if (ctx().index)
            s += ",\n";
        s += _tab() + _encodeStr(key) + ": " + _encodeStr(value);
        ctx().index++;
        return s;
    }

    String SrxJSONEncoder::_encodeKeyValueStrs(String key, std::vector<String> values, bool newLines) {
        String s = _encodeBlockBegin(key, '[', newLines);

        if (!newLines) {
            for (const String& value : values) {
                if (ctx().index)
                    s += ", ";
                s += _encodeStr(value);
                ctx().index++;
            }
        } else {
            for (const String& value : values) {
                if (ctx().index)
                    s += ",\n";
                s += _tab() + _encodeStr(value);
                ctx().index++;
            }
        }

        return s += _encodeBlockEnd(']', newLines);
    }

    String SrxJSONEncoder::_encodeKeyValueNum(String key, String value) {
        String s;
        if (ctx().index)
            s += ",\n";
        s += _tab() + _encodeStr(key) + ": " + value;
        ctx().index++;
        return s;
    }

    template<class T_num>
    String SrxJSONEncoder::_encodeKeyValueNum(String key, T_num value) {
        return _encodeKeyValueNum(key, ToString(value));
    }

    String SrxJSONEncoder::_encodeKeyValueBool(String key, String value) {
        String s;
        if (ctx().index)
            s += ",\n";
        s += _tab() + _encodeStr(key) + ": " +
             (_strToBool(value) ? "true" : "false");
        ctx().index++;
        return s;
    }

    String SrxJSONEncoder::_encodeKeyValueBool(String key, bool value) {
        String s;
        if (ctx().index)
            s += ",\n";
        s += _tab() + _encodeStr(key) + ": " +
            (value ? "true" : "false");
        ctx().index++;
        return s;
    }

    String SrxJSONEncoder::_encodeBlockBegin(String key, char startChar, bool newLine) {
        String s;
        if (ctx().index)
            s += ",\n";
        s += _tab() + _encodeStr(key) + ": " + startChar + (newLine ? "\n" : " ");
        pushCtx();
        return s;
    }

    String SrxJSONEncoder::_encodeBlockBegin(char startChar) {
        String s;
        if (ctx().index)
            s += ",\n";
        s += _tab() + startChar + "\n";
        pushCtx();
        return s;
    }

    String SrxJSONEncoder::_encodeBlockEnd(char endChar, bool newLine) {
        const bool empty = !ctx().index;
        popCtx();
        ctx().index++;
        String s;
        if (!empty)
            s += (newLine) ? ("\n" + _tab()) : " ";
        return s + endChar;
    }

    String SrxJSONEncoder::_encode(String key, const UID& uid) {
        String s = _encodeBlockBegin(key, '{');

        s += _encodeKeyValueStr("id", _hexStr(uid.id));
        s += _encodeKeyValueNum("size", uid.size);

        return s += _encodeBlockEnd('}');
    }

    String SrxJSONEncoder::_encode(const UID& uid) {
        String s = _encodeBlockBegin('{');

        s += _encodeKeyValueStr("id", _hexStr(uid.id));
        s += _encodeKeyValueNum("size", uid.size);

        return s += _encodeBlockEnd('}');
    }

    String SrxJSONEncoder::_encode(String key, const time_t& time) {
        return _encodeKeyValueNum(key, time);
    }

    String SrxJSONEncoder::_encode(String key, const DataType& type) {
        String s = _encodeBlockBegin(key, '{');

        std::vector<String> customTypes;
        if (!type.customTypeName().empty())
            customTypes.push_back(type.customTypeName());
        if (!type.customTypeName2().empty())
            customTypes.push_back(type.customTypeName2());

        s += _encodeKeyValueStr("base_type", type.baseTypeName());
        if (!customTypes.empty())
            s += _encodeKeyValueStrs("custom_types", customTypes, false);
        s += _encodeKeyValueNum("size", type.size());
        s += _encodeKeyValueBool("is_pointer", type.isPointer());

        return s += _encodeBlockEnd('}');
    }

    String SrxJSONEncoder::_encode(String key, const UIDChain& chain) {
        String s = _encodeBlockBegin(key, '[');

        for (int i = 0; i < chain.size(); ++i)
            s += _encode(chain[i]);

        return s += _encodeBlockEnd(']');
    }

    String SrxJSONEncoder::_encode(const Member& member) {
        String s = _encodeBlockBegin('{');

        s += _encodeKeyValueStr("name", member.name());
        s += _encodeKeyValueNum("offset", member.offset());
        s += _encode("uid", member.uid());
        s += _encode("type", member.type());

        return s += _encodeBlockEnd('}');
    }

    String SrxJSONEncoder::_encode(const std::vector<Member>& members) {
        String s = _encodeBlockBegin("members", '[');

        for (int i = 0; i < members.size(); ++i)
            s += _encode(members[i]);

        return s += _encodeBlockEnd(']');
    }

    String SrxJSONEncoder::_encode(const Object& obj) {
        String s = _encodeBlockBegin('{');

        s += _encode("type", obj.type());

        if (obj.type().isPrimitive() && !obj.type().isPointer()) {
            const String primitiveValue = Archive::primitiveObjectValueToString(obj);
            if (obj.type().isNumber())
                s += _encodeKeyValueNum("value", primitiveValue);
            else if (obj.type().isBool())
                s += _encodeKeyValueBool("value", primitiveValue);
            else
                s += _encodeKeyValueStr("value", primitiveValue);
        }

        if (obj.version())
            s += _encodeKeyValueNum("version", obj.version());
        if (obj.minVersion())
            s += _encodeKeyValueNum("min_version", obj.minVersion());
        s += _encode("uid_chain", obj.uidChain());

        if (obj.parentUID() != NO_UID)
            s += _encode("parent", obj.parentUID());

        if (!obj.members().empty())
            s += _encode(obj.members());

        return s += _encodeBlockEnd('}');
    }

    String SrxJSONEncoder::_encode(String key, const Archive::ObjectPool& objects) {
        String s = _encodeBlockBegin(key, '[');

        for (Archive::ObjectPool::const_iterator itObject = objects.begin();
             itObject != objects.end(); ++itObject)
        {
            const Object& obj = itObject->second;
            s += _encode(obj);
            ctx().index++;
        }

        return s += _encodeBlockEnd(']');
    }

    String SrxJSONEncoder::_encodeRoot(const Archive& arch) {
        clearCtx();

        String s = _encodeBlockBegin('{');

        s += _encodeBlockBegin("format", '{');
        s += _encodeKeyValueStr("name", "Srx-JSON");
        s += _encodeKeyValueNum("major", ENCODING_FORMAT_MAJOR_VERSION);
        s += _encodeKeyValueNum("minor", ENCODING_FORMAT_MINOR_VERSION);
        s += _encodeBlockEnd('}');

        s += _encodeBlockBegin("meta", '{');

        if (!arch.m_name.empty())
            s += _encodeKeyValueStr("name", arch.m_name);

        if (!arch.m_comment.empty())
            s += _encodeKeyValueStr("comment", arch.m_comment);

        s += _encode("created", arch.m_timeCreated);
        s += _encode("modified", arch.m_timeModified);

        s += _encodeBlockEnd('}');

        s += _encodeBlockBegin("archive", '{');
        s += _encode("root_object", arch.m_root);
        s += _encode("all_objects", arch.m_allObjects);
        s += _encodeBlockEnd('}');

        s += _encodeBlockEnd('}');
        s += '\n';

        return s;
    }

    RawData SrxJSONEncoder::encode(const Archive& arch) {
        RawData rawData;
        String s = MAGIC_START;
        s += _encodeRoot(arch);
        rawData.resize(s.length() + 1);
        memcpy(&rawData[0], &s[0], s.length() + 1);
        return rawData;
    }

    // *************** SrxJSONDecoder ***************
    // *

    SrxJSONDecoder::SrxJSONDecoder() {
    }

    static void _decodeChar(RawData& rawData, JSONNodeRef json) {
        if (!json)
            throw Exception("JSONDecoder Error: no JSON value");
        if (json->type() != JSON_STRING)
            throw Exception("JSONDecoder Error: expected JSON string value");

        String s = json->asStr()->value();
        if (s.empty())
            throw Exception("JSONDecoder Error: unexpected empty JSON string");

        *(char*)&rawData[0] = s[0];
    }

    template<typename T_int8>
    static void _decodeInt8(RawData& rawData, JSONNodeRef json) {
        if (!json)
            throw Exception("JSONDecoder Error: no JSON value");
        if (json->type() != JSON_NUMBER)
            throw Exception("JSONDecoder Error: expected JSON number value");

        const T_int8 i = (T_int8) json->asNum()->value<int>();
        *(T_int8*)&rawData[0] = i;
    }

    template<typename T_int>
    static void _decodeInt(RawData& rawData, JSONNodeRef json) {
        if (!json)
            throw Exception("JSONDecoder Error: no JSON value");
        if (json->type() != JSON_NUMBER)
            throw Exception("JSONDecoder Error: expected JSON number value");

        const T_int i = json->asNum()->value<T_int>();
        *(T_int*)&rawData[0] = i;
    }

    template<typename T_real>
    static void _decodeReal(RawData& rawData, JSONNodeRef json) {
        if (!json)
            throw Exception("JSONDecoder Error: no JSON value");
        if (json->type() != JSON_NUMBER)
            throw Exception("JSONDecoder Error: expected JSON number value");

        const T_real r = json->asNum()->value<T_real>();
        *(T_real*)&rawData[0] = r;
    }

    template<typename T_real>
    static void _decodeBool(RawData& rawData, JSONNodeRef json) {
        if (!json)
            throw Exception("JSONDecoder Error: no JSON value");
        if (json->type() != JSON_BOOL)
            throw Exception("JSONDecoder Error: expected JSON bool value");

        const bool b = json->asBool()->value();
        *(bool*)&rawData[0] = b;
    }

    static void _decodeStr(RawData& rawData, JSONNodeRef json) {
        if (!json)
            throw Exception("JSONDecoder Error: no JSON value");
        if (json->type() != JSON_STRING)
            throw Exception("JSONDecoder Error: expected JSON string value");

        String s = json->asStr()->value();
        rawData.resize(s.length() + 1);
        strcpy((char*)&rawData[0], &s[0]);
    }

    UID SrxJSONDecoder::_decodeUID(JSONObjectRef jsonUIDObj) {
        JSONStringRef jsonID = jsonUIDObj->memberStr("id");
        if (!jsonID)
            throw Exception("JSONDecoder Error: missing UID.id");

        JSONNumberRef jsonSize = jsonUIDObj->memberNum("size");
        if (!jsonSize)
            throw Exception("JSONDecoder Error: missing UID.size");

        const ID id = (ID) std::stoull(jsonID->value(), NULL, 16);
        const size_t size = jsonSize->value<size_t>();

        const UID uid = { id, size };
        return uid;
    }

    UIDChain SrxJSONDecoder::_decodeUIDChain(JSONArrayRef jsonArr) {
        UIDChain chain;
        for (size_t i = 0; i < jsonArr->count(); ++i) {
            const UID uid = _decodeUID(jsonArr->elementObj(i));
            chain.push_back(uid);
        }
        assert(!chain.empty());
        return chain;
    }

    DataType SrxJSONDecoder::_decodeType(JSONObjectRef jsonTypeObj) {
        DataType type;

        JSONStringRef base_type = jsonTypeObj->memberStr("base_type");
        if (!base_type)
            throw Exception("JSONDecoder Error: missing Type.base_type");

        JSONArrayRef custom_types = jsonTypeObj->memberArr("custom_types");

        JSONNumberRef size = jsonTypeObj->memberNum("size");
        if (!size)
            throw Exception("JSONDecoder Error: missing Type.size");

        JSONBoolRef is_pointer = jsonTypeObj->memberBool("is_pointer");
        if (!is_pointer)
            throw Exception("JSONDecoder Error: missing Type.is_pointer");

        type.m_baseTypeName = base_type->value();
        if (custom_types) {
            JSONStringRef customTypeName1 = custom_types->elementStr(0);
            if (customTypeName1)
                type.m_customTypeName = customTypeName1->value();
            JSONStringRef customTypeName2 = custom_types->elementStr(1);
            if (customTypeName2)
                type.m_customTypeName2 = customTypeName2->value();
        }
        type.m_size = size->value<int>();
        type.m_isPointer = is_pointer->value();

        return type;
    }

    void SrxJSONDecoder::_decodePrimitiveObjectValue(Object& obj, JSONNodeRef json) {
        const DataType& type = obj.type();
        if (type.isPrimitive() && !type.isPointer()) {
            obj.m_data.resize(type.size());
            if (type.isInteger() || type.isEnum()) {
                if (type.isSigned()) {
                    if (type.size() == 1)
                        _decodeInt8<int8_t>(obj.m_data, json);
                    else if (type.size() == 2)
                        _decodeInt<int16_t>(obj.m_data, json);
                    else if (type.size() == 4)
                        _decodeInt<int32_t>(obj.m_data, json);
                    else if (type.size() == 8)
                        _decodeInt<int64_t>(obj.m_data, json);
                    else
                        assert(false /* unknown signed int type size */);
                } else {
                    if (type.size() == 1)
                        _decodeInt8<uint8_t>(obj.m_data, json);
                    else if (type.size() == 2)
                        _decodeInt<uint16_t>(obj.m_data, json);
                    else if (type.size() == 4)
                        _decodeInt<uint32_t>(obj.m_data, json);
                    else if (type.size() == 8)
                        _decodeInt<uint64_t>(obj.m_data, json);
                    else
                        assert(false /* unknown unsigned int type size */);
                }
            } else if (type.isReal()) {
                if (type.size() == sizeof(float))
                    _decodeReal<float>(obj.m_data, json);
                else if (type.size() == sizeof(double))
                    _decodeReal<double>(obj.m_data, json);
                else
                    assert(false /* unknown floating point type */);
            } else if (type.isChar()) {
                _decodeChar(obj.m_data, json);
            } else if (type.isBool()) {
                _decodeBool<uint8_t>(obj.m_data, json);
            } else if (type.isString()) {
                _decodeStr(obj.m_data, json);
            } else {
                assert(false /* unknown primitive type */);
            }
        }
    }

    Member SrxJSONDecoder::_decodeMember(JSONObjectRef jsonObj) {
        Member m;
        if (!jsonObj) return m;

        JSONStringRef name = jsonObj->memberStr("name");
        if (!name)
            throw Exception("JSONDecoder Error: missing Member.name");

        JSONNumberRef offset = jsonObj->memberNum("offset");
        if (!offset)
            throw Exception("JSONDecoder Error: missing Member.offset");

        JSONObjectRef uid = jsonObj->memberObj("uid");
        if (!uid)
            throw Exception("JSONDecoder Error: missing Member.uid");

        JSONObjectRef type = jsonObj->memberObj("type");
        if (!type)
            throw Exception("JSONDecoder Error: missing Member.type");

        m.m_uid    = _decodeUID(uid);
        m.m_offset = offset->value<ssize_t>();
        m.m_name   = name->value();
        m.m_type   = _decodeType(type);

        assert(m.type());
        assert(!m.name().empty());
        assert(m.uid().isValid());

        return m;
    }

    std::vector<Member> SrxJSONDecoder::_decodeMembers(JSONArrayRef jsonArr) {
        std::vector<Member> members;
        if (!jsonArr) return members;

        for (size_t i = 0; i < jsonArr->count(); ++i) {
            const Member member = _decodeMember(jsonArr->elementObj(i));
            if (member)
                members.push_back(member);
            else
                break;
        }

        return members;
    }

    Object SrxJSONDecoder::_decodeObject(JSONObjectRef jsonObject) {
        Object obj;
        if (!jsonObject) return obj;

        JSONObjectRef type = jsonObject->memberObj("type");
        if (!type)
            throw Exception("JSONDecoder Error: missing Object.type");

        JSONNumberRef version = jsonObject->memberNum("version");

        JSONNumberRef min_version = jsonObject->memberNum("min_version");

        JSONArrayRef uid_chain = jsonObject->memberArr("uid_chain");
        if (!uid_chain)
            throw Exception("JSONDecoder Error: missing Object.uid_chain");

        JSONObjectRef parent = jsonObject->memberObj("parent");
        JSONArrayRef members = jsonObject->memberArr("members");

        obj.m_type       = _decodeType(type);
        if (version)
            obj.m_version = version->value<Version>();
        if (min_version)
            obj.m_minVersion = min_version->value<Version>();
        obj.m_uid        = _decodeUIDChain(uid_chain);
        obj.m_parentUID  = (parent) ? _decodeUID(parent) : NO_UID;

        if (members)
            obj.m_members = _decodeMembers(members);

        assert(obj.type());

        _decodePrimitiveObjectValue(obj, jsonObject->member("value"));

        return obj;
    }

    void SrxJSONDecoder::_decodeAllObjects(Archive& arch, JSONArrayRef all_objects)
    {
        for (size_t i = 0; i < all_objects->count(); ++i) {
            const Object obj = _decodeObject(all_objects->elementObj(i));
            if (!obj) break;
            arch.m_allObjects[obj.uid()] = obj;
        }
    }

    void SrxJSONDecoder::_decodeRoot(Archive& arch, JSONObjectRef jsonRootObj) {
        {
            JSONObjectRef format = jsonRootObj->memberObj("format");
            if (!format)
                throw Exception("JSONDecoder Error: missing JSON object 'format'");

            JSONStringRef name = format->memberStr("name");
            if (!name)
                throw Exception("JSONDecoder Error: missing JSON object 'format'.'name'");
            if (name->value() != "Srx-JSON")
                throw Exception("JSONDecoder Error: wrong format name");

            JSONNumberRef major = format->memberNum("major");
            if (!major)
                throw Exception("JSONDecoder Error: missing JSON object 'format'.'major'");
            if (major->value<int>() != ENCODING_FORMAT_MAJOR_VERSION)
                throw Exception("JSONDecoder Error: wrong format major version");

            // just in case this encoding format will be extended in future
            // (currently not used)
            JSONNumberRef minor = format->memberNum("minor");
            if (!minor)
                throw Exception("JSONDecoder Error: missing JSON object 'format'.'minor'");
        }
        {
            JSONObjectRef meta = jsonRootObj->memberObj("meta");
            if (!meta)
                throw Exception("JSONDecoder Error: missing JSON object 'meta'");

            JSONStringRef name = meta->memberStr("name");
            arch.m_name = name ? name->value() : "";

            JSONStringRef comment = meta->memberStr("comment");
            arch.m_comment = comment ? comment->value() : "";

            JSONNumberRef created = meta->memberNum("created");
            arch.m_timeCreated = (time_t) (created ? created->value<uint64_t>() : 0);

            JSONNumberRef modified = meta->memberNum("modified");
            arch.m_timeModified = (time_t) (modified ? modified->value<uint64_t>() : 0);
        }
        {
            JSONObjectRef archive = jsonRootObj->memberObj("archive");
            if (!archive)
                throw Exception("JSONDecoder Error: missing JSON object 'archive'");

            JSONObjectRef root_object = archive->memberObj("root_object");
            if (!root_object)
                throw Exception("JSONDecoder Error: missing JSON object 'archive'.'root_object'");
            arch.m_root = _decodeUID(root_object);
            if (!arch.m_root)
                throw Exception("JSONDecoder Error: no root object");

            JSONArrayRef all_objects = archive->memberArr("all_objects");
            if (!all_objects)
                throw Exception("JSONDecoder Error: missing JSON object 'archive'.'all_objects'");
            _decodeAllObjects(arch, all_objects);
            if (!arch.m_allObjects[arch.m_root])
                throw Exception("JSONDecoder Error: missing declared root object");
        }
    }

    void SrxJSONDecoder::decode(Archive& arch, const RawData& data) {
        if (!checkMagicStart(data))
            throw Exception("JSONDecoder Error: Magic start missing!");
        const char* p   = (const char*) &data[0];
        const char* end = p + data.size();
        JSONNodeRef jsonRootObj = jsonParse(p, end);
        if (!jsonRootObj || jsonRootObj->type() != JSON_OBJECT)
            throw Exception("JSONDecoder Error: missing JSON root object");
        _decodeRoot(arch, jsonObjectRef(jsonRootObj));
    }

} // namespace Serialization
