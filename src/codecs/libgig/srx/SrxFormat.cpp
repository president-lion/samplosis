/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2017-2026 Christian Schoenebeck                         *
 *                           <cuse@users.sourceforge.net>                  *
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

#include "SrxFormat.h"

#include <string.h> // for memcpy()
#include "helper.h"

namespace Serialization {

    /*
     * Srx format history:
     * - 1.0: Initial version.
     * - 1.1: Adds "String", "Array", "Set" and "Map" data types and an optional
     *        2nd custom type name (e.g. "Map" types which always contain two
     *        user defined types).
     * - 1.2: Stores objects' parent UID, which projects which native data
     *        structure was contained within the boundaries of which specific
     *        other data structure. This permits to automatically restore native
     *        pointers, as structured layout info is required to identify
     *        whether a pointer was just a "weak" pointer or a "strong" pointer.
     *        For a "strong" pointer the pointed data has to be allocated
     *        automatically by this framework, whereas for "weak" pointers not.
     */
    #define MAGIC_START "Srx1v"
    #define ENCODING_FORMAT_MINOR_VERSION 2

    // *************** Encoder ***************
    // *

    String SrxFormat::_encodeBlob(String data) {
        return ToString(data.length()) + ":" + data;
    }

    String SrxFormat::_encode(const UID& uid) {
        String s;
        s += _encodeBlob(ToString(size_t(uid.id)));
        s += _encodeBlob(ToString(size_t(uid.size)));
        return _encodeBlob(s);
    }

    String SrxFormat::_encode(const time_t& time) {
        return _encodeBlob(ToString(time));
    }

    String SrxFormat::_encode(const DataType& type) {
        String s;

        // Srx v1.0 format (mandatory):
        s += _encodeBlob(type.baseTypeName());
        s += _encodeBlob(type.customTypeName());
        s += _encodeBlob(ToString(type.size()));
        s += _encodeBlob(ToString(type.isPointer()));

        // Srx v1.1 format:
        s += _encodeBlob(type.customTypeName2());

        return _encodeBlob(s);
    }

    String SrxFormat::_encode(const UIDChain& chain) {
        String s;
        for (int i = 0; i < chain.size(); ++i)
            s += _encode(chain[i]);
        return _encodeBlob(s);
    }

    String SrxFormat::_encode(const Member& member) {
        String s;
        s += _encode(member.uid());
        s += _encodeBlob(ToString(member.offset()));
        s += _encodeBlob(member.name());
        s += _encode(member.type());
        return _encodeBlob(s);
    }

    String SrxFormat::_encode(const std::vector<Member>& members) {
        String s;
        for (int i = 0; i < members.size(); ++i)
            s += _encode(members[i]);
        return _encodeBlob(s);
    }

    String SrxFormat::_encodePrimitiveValue(const Object& obj) {
        return _encodeBlob( Archive::primitiveObjectValueToString(obj) );
    }

    String SrxFormat::_encode(const Object& obj) {
        String s;
        s += _encode(obj.type());
        s += _encodeBlob(ToString(obj.version()));
        s += _encodeBlob(ToString(obj.minVersion()));
        s += _encode(obj.uidChain());
        s += _encode(obj.members());
        s += _encodePrimitiveValue(obj);
        // Srx v1.2 format:
        s += _encode(obj.parentUID());

        return _encodeBlob(s);
    }

    String SrxFormat::_encode(const Archive::ObjectPool& objects) {
        String s;
        for (Archive::ObjectPool::const_iterator itObject = objects.begin();
             itObject != objects.end(); ++itObject)
        {
            const Object& obj = itObject->second;
            s += _encode(obj);
        }
        return _encodeBlob(s);
    }

    String SrxFormat::_encodeRootBlob(const Archive& arch) {
        String s;
        s += _encodeBlob(ToString(ENCODING_FORMAT_MINOR_VERSION));
        s += _encode(arch.m_root);
        s += _encode(arch.m_allObjects);
        s += _encodeBlob(arch.m_name);
        s += _encodeBlob(arch.m_comment);
        s += _encode(arch.m_timeCreated);
        s += _encode(arch.m_timeModified);
        return _encodeBlob(s);
    }

    RawData SrxFormat::encode(const Archive& arch) {
        RawData rawData;
        String s = MAGIC_START;
        s += _encodeRootBlob(arch);
        rawData.resize(s.length() + 1);
        memcpy(&rawData[0], &s[0], s.length() + 1);
        return rawData;
    }

    // *************** Decoder ***************
    // *

    SrxFormat::_Blob SrxFormat::_decodeBlob(const char* p, const char* end, bool bThrow) {
        if (!bThrow && p >= end) {
            const _Blob blob =  { p, end };
            return blob;
        }
        size_t sz = 0;
        for (; true; ++p) {
            if (p >= end)
                throw Exception("Decode Error: Missing blob");
            const char& c = *p;
            if (c == ':') break;
            if (c < '0' || c > '9')
                throw Exception("Decode Error: Missing blob size");
            sz *= 10;
            sz += size_t(c - '0');
        }
        ++p;
        if (p + sz > end)
            throw Exception("Decode Error: Premature end of blob");
        const _Blob blob = { p, p + sz };
        return blob;
    }

    char SrxFormat::_popCharBlob(const char*& p, const char* end) {
        _Blob blob = _decodeBlob(p, end);
        p   = blob.p;
        end = blob.end;

        if (p >= end)
            throw Exception("Decode Error: premature end of char blob");

        if (end - p != 1)
            throw Exception("Decode Error: size of char blob bigger than 1");

        return *p++;
    }

    void SrxFormat::_popCharBlob(const char*& p, const char* end, RawData& rawData) {
        const char c = _popCharBlob(p, end);
        *(char*)&rawData[0] = c;
    }

    template<typename T_int>
    T_int SrxFormat::_popIntBlob(const char*& p, const char* end) {
        _Blob blob = _decodeBlob(p, end);
        p   = blob.p;
        end = blob.end;

        T_int sign = 1;
        T_int i = 0;
        if (p >= end)
            throw Exception("Decode Error: premature end of int blob");
        if (*p == '-') {
            sign = -1;
            ++p;
        }
        for (; p < end; ++p) {
            const char& c = *p;
            if (c < '0' || c > '9')
                throw Exception("Decode Error: Invalid int blob format");
            i *= 10;
            i += size_t(c - '0');
        }
        return i * sign;
    }

    template<typename T_int>
    void SrxFormat::_popIntBlob(const char*& p, const char* end, RawData& rawData) {
        const T_int i = _popIntBlob<T_int>(p, end);
        *(T_int*)&rawData[0] = i;
    }

    template<typename T_real>
    T_real SrxFormat::_popRealBlob(const char*& p, const char* end) {
        _Blob blob = _decodeBlob(p, end);
        p   = blob.p;
        end = blob.end;

        if (p >= end || (end - p) < 1)
            throw Exception("Decode Error: premature end of real blob");

        String s(p, size_t(end - p));

        T_real r;
        if (sizeof(T_real) <= sizeof(double))
            r = strTo<T_real>(s);
        else
            assert(false /* unknown real type */);

        p += s.length();

        return r;
    }

    template<typename T_real>
    void SrxFormat::_popRealBlob(const char*& p, const char* end, RawData& rawData) {
        const T_real r = _popRealBlob<T_real>(p, end);
        *(T_real*)&rawData[0] = r;
    }

    String SrxFormat::_popStringBlob(const char*& p, const char* end) {
        _Blob blob = _decodeBlob(p, end);
        p   = blob.p;
        end = blob.end;
        if (end - p < 0)
            throw Exception("Decode Error: missing String blob");
        String s;
        const size_t sz = end - p;
        s.resize(sz);
        memcpy(&s[0], p, sz);
        p += sz;
        return s;
    }

    void SrxFormat::_popStringBlob(const char*& p, const char* end, RawData& rawData) {
        String s = _popStringBlob(p, end);
        rawData.resize(s.length() + 1);
        strcpy((char*)&rawData[0], &s[0]);
    }

    time_t SrxFormat::_popTimeBlob(const char*& p, const char* end) {
        const uint64_t i = _popIntBlob<uint64_t>(p, end);
        return (time_t) i;
    }

    DataType SrxFormat::_popDataTypeBlob(const char*& p, const char* end) {
        _Blob blob = _decodeBlob(p, end);
        p   = blob.p;
        end = blob.end;

        DataType type;

        // Srx v1.0 format (mandatory):
        type.m_baseTypeName   = _popStringBlob(p, end);
        type.m_customTypeName = _popStringBlob(p, end);
        type.m_size           = _popIntBlob<int>(p, end);
        type.m_isPointer      = _popIntBlob<bool>(p, end);

        // Srx v1.1 format (optional):
        if (p < end)
            type.m_customTypeName2 = _popStringBlob(p, end);

        return type;
    }

    UID SrxFormat::_popUIDBlob(const char*& p, const char* end) {
        _Blob blob = _decodeBlob(p, end);
        p   = blob.p;
        end = blob.end;

        if (p >= end)
            throw Exception("Decode Error: premature end of UID blob");

        const ID id = (ID) _popIntBlob<size_t>(p, end);
        const size_t size = _popIntBlob<size_t>(p, end);

        const UID uid = { id, size };
        return uid;
    }

    UIDChain SrxFormat::_popUIDChainBlob(const char*& p, const char* end) {
        _Blob blob = _decodeBlob(p, end);
        p   = blob.p;
        end = blob.end;

        UIDChain chain;
        while (p < end) {
            const UID uid = _popUIDBlob(p, end);
            chain.push_back(uid);
        }
        assert(!chain.empty());
        return chain;
    }

    Member SrxFormat::_popMemberBlob(const char*& p, const char* end) {
        _Blob blob = _decodeBlob(p, end, false);
        p   = blob.p;
        end = blob.end;

        Member m;
        if (p >= end) return m;

        m.m_uid    = _popUIDBlob(p, end);
        m.m_offset = _popIntBlob<ssize_t>(p, end);
        m.m_name   = _popStringBlob(p, end);
        m.m_type   = _popDataTypeBlob(p, end);
        assert(m.type());
        assert(!m.name().empty());
        assert(m.uid().isValid());
        return m;
    }

    std::vector<Member> SrxFormat::_popMembersBlob(const char*& p, const char* end) {
        _Blob blob = _decodeBlob(p, end, false);
        p   = blob.p;
        end = blob.end;

        std::vector<Member> members;
        while (p < end) {
            const Member member = _popMemberBlob(p, end);
            if (member)
                members.push_back(member);
            else
                break;
        }
        return members;
    }

    void SrxFormat::_popPrimitiveValue(const char*& p, const char* end, Object& obj) {
        const DataType& type = obj.type();
        if (type.isPrimitive() && !type.isPointer()) {
            obj.m_data.resize(type.size());
            if (type.isInteger() || type.isEnum()) {
                if (type.isSigned()) {
                    if (type.size() == 1)
                        _popIntBlob<int8_t>(p, end, obj.m_data);
                    else if (type.size() == 2)
                        _popIntBlob<int16_t>(p, end, obj.m_data);
                    else if (type.size() == 4)
                        _popIntBlob<int32_t>(p, end, obj.m_data);
                    else if (type.size() == 8)
                        _popIntBlob<int64_t>(p, end, obj.m_data);
                    else
                        assert(false /* unknown signed int type size */);
                } else {
                    if (type.size() == 1)
                        _popIntBlob<uint8_t>(p, end, obj.m_data);
                    else if (type.size() == 2)
                        _popIntBlob<uint16_t>(p, end, obj.m_data);
                    else if (type.size() == 4)
                        _popIntBlob<uint32_t>(p, end, obj.m_data);
                    else if (type.size() == 8)
                        _popIntBlob<uint64_t>(p, end, obj.m_data);
                    else
                        assert(false /* unknown unsigned int type size */);
                }
            } else if (type.isReal()) {
                if (type.size() == sizeof(float))
                    _popRealBlob<float>(p, end, obj.m_data);
                else if (type.size() == sizeof(double))
                    _popRealBlob<double>(p, end, obj.m_data);
                else
                    assert(false /* unknown floating point type */);
            } else if (type.isChar()) {
                _popCharBlob(p, end, obj.m_data);
            } else if (type.isBool()) {
                _popIntBlob<uint8_t>(p, end, obj.m_data);
            } else if (type.isString()) {
                _popStringBlob(p, end, obj.m_data);
            } else {
                assert(false /* unknown primitive type */);
            }

        } else {
            // don't whine if the empty blob was not added on encoder side
            _Blob blob = _decodeBlob(p, end, false);
            p   = blob.p;
            end = blob.end;
        }
    }

    Object SrxFormat::_popObjectBlob(const char*& p, const char* end) {
        _Blob blob = _decodeBlob(p, end, false);
        p   = blob.p;
        end = blob.end;

        Object obj;
        if (p >= end) return obj;

        obj.m_type       = _popDataTypeBlob(p, end);
        obj.m_version    = _popIntBlob<Version>(p, end);
        obj.m_minVersion = _popIntBlob<Version>(p, end);
        obj.m_uid        = _popUIDChainBlob(p, end);
        obj.m_members    = _popMembersBlob(p, end);
        _popPrimitiveValue(p, end, obj);
        assert(obj.type());

        // Srx v1.2 format (optional):
        if (p < end)
            obj.m_parentUID = _popUIDBlob(p, end);

        return obj;
    }

    void SrxFormat::_popObjectsBlob(Archive& arch, const char*& p, const char* end) {
        _Blob blob = _decodeBlob(p, end, false);
        p   = blob.p;
        end = blob.end;

        if (p >= end)
            throw Exception("Decode Error: Premature end of objects blob");

        while (true) {
            const Object obj = _popObjectBlob(p, end);
            if (!obj) break;
            arch.m_allObjects[obj.uid()] = obj;
        }
    }

    void SrxFormat::_popRootBlob(Archive& arch, const char*& p, const char* end) {
        _Blob blob = _decodeBlob(p, end, false);
        p   = blob.p;
        end = blob.end;

        if (p >= end)
            throw Exception("Decode Error: Premature end of root blob");

        // just in case this encoding format will be extended in future
        // (currently not used)
        const int formatMinorVersion = _popIntBlob<int>(p, end);

        arch.m_root = _popUIDBlob(p, end);
        if (!arch.m_root)
            throw Exception("Decode Error: No root object");

        _popObjectsBlob(arch, p, end);
        if (!arch.m_allObjects[arch.m_root])
            throw Exception("Decode Error: Missing declared root object");

        arch.m_name = _popStringBlob(p, end);
        arch.m_comment = _popStringBlob(p, end);
        arch.m_timeCreated = _popTimeBlob(p, end);
        arch.m_timeModified = _popTimeBlob(p, end);
    }

    void SrxFormat::decode(Archive& arch, const RawData& data) {
        if (!checkMagicStart(data))
            throw Exception("Decode Error: Magic start missing!");
        const char* p   = (const char*) &data[0];
        const char* end = p + data.size();
        p += strlen(MAGIC_START);
        _popRootBlob(arch, p, end);
    }

    bool SrxFormat::checkMagicStart(const RawData& data) {
        if (data.empty()) return false;
        const char* p = (const char*) &data[0];
        const size_t sz = data.size();
        return !memcmp(p, MAGIC_START, std::min(strlen(MAGIC_START), sz));
    }

    String SrxFormat::name() {
        return MAGIC_START;
    }

} // namespace Serialization
